// TermForge performance harness (#88).
//
// This is evidence, not a speed test: it records deterministic workloads and
// never treats a host-dependent duration as pass/fail. Build only in Release.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/utsname.h>

#include "detail/base64.hpp"
#include "detail/payload_hash.hpp"
#include "detail/simd.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/text.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/detail/width.hpp"

using namespace termforge;

namespace {

using Clock = std::chrono::steady_clock;

enum class Suite { Kernels, W3, All };
enum class Format { Table, Json };
enum class KernelChoice { Auto, Scalar, Avx2 };

struct Options {
  Suite suite{Suite::All};
  Format format{Format::Table};
  std::optional<std::filesystem::path> output;
  int samples{9};
  int warmup{2};
  bool smoke{false};
  KernelChoice kernel_tier{KernelChoice::Auto};
};

struct Result {
  std::string suite;
  std::string name;
  std::string content{"-"};
  int cols{};
  int rows{};
  int dirty_percent{-1};
  std::size_t input_bytes{};
  std::uint64_t output_bytes{};
  std::uint64_t checksum{};
  int iterations{};
  double median_ms{};
  double p95_ms{};
  double throughput_mib_s{};
};

struct Wall {
  std::string content;
  int dirty_percent{};
  double budget_ms{};
  std::string largest_passing;
  std::optional<std::string> first_failing;
};

std::atomic<std::uint64_t> g_observed{0};

class CountingSink final : public ByteSink {
 public:
  auto reset_frame() noexcept -> void { m_last_bytes = 0; }
  [[nodiscard]] auto last_bytes() const noexcept -> std::uint64_t {
    return m_last_bytes;
  }
  [[nodiscard]] auto checksum() const noexcept -> std::uint64_t {
    return m_checksum;
  }

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    m_last_bytes = bytes.size();
    // O(1) observation: the harness measures protocol construction, not a
    // second whole-frame hash in its discard sink. The virtual call and these
    // samples keep the emitted span observable without making output length
    // an extra CPU cost inside the timed path.
    std::uint64_t hash = bytes.size();
    if (!bytes.empty()) {
      hash ^= static_cast<unsigned char>(bytes.front()) << 8;
      hash ^= static_cast<unsigned char>(bytes[bytes.size() / 2]) << 16;
      hash ^= static_cast<unsigned char>(bytes.back()) << 24;
    }
    m_checksum ^= hash + 0x9E3779B97F4A7C15ULL + (m_checksum << 6) +
                  (m_checksum >> 2);
    return {};
  }

 private:
  std::uint64_t m_last_bytes{};
  std::uint64_t m_checksum{14695981039346656037ULL};
};

[[nodiscard]] auto parse_positive(std::string_view value, std::string_view name)
    -> int {
  int out = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size() || out <= 0) {
    throw std::runtime_error{std::format("{} must be a positive integer", name)};
  }
  return out;
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](std::string_view name) -> std::string_view {
      if (i + 1 >= argc)
        throw std::runtime_error{std::format("{} requires a value", name)};
      return argv[++i];
    };
    if (arg == "--suite") {
      const auto v = value(arg);
      if (v == "kernels") out.suite = Suite::Kernels;
      else if (v == "w3") out.suite = Suite::W3;
      else if (v == "all") out.suite = Suite::All;
      else throw std::runtime_error{"--suite must be kernels, w3, or all"};
    } else if (arg == "--format") {
      const auto v = value(arg);
      if (v == "table") out.format = Format::Table;
      else if (v == "json") out.format = Format::Json;
      else throw std::runtime_error{"--format must be table or json"};
    } else if (arg == "--output") {
      out.output = std::filesystem::path{value(arg)};
    } else if (arg == "--samples") {
      out.samples = parse_positive(value(arg), arg);
    } else if (arg == "--warmup") {
      out.warmup = parse_positive(value(arg), arg);
    } else if (arg == "--smoke") {
      out.smoke = true;
      out.samples = 2;
      out.warmup = 1;
    } else if (arg == "--kernel-tier") {
      const auto v = value(arg);
      if (v == "auto") out.kernel_tier = KernelChoice::Auto;
      else if (v == "scalar") out.kernel_tier = KernelChoice::Scalar;
      else if (v == "avx2") out.kernel_tier = KernelChoice::Avx2;
      else throw std::runtime_error{"--kernel-tier must be auto, scalar, or avx2"};
    } else if (arg == "--help") {
      std::cout
          << "Usage: termforge_bench [--suite kernels|w3|all] "
             "[--format table|json] [--output PATH] [--samples N] "
             "[--warmup N] [--kernel-tier auto|scalar|avx2] [--smoke]\n";
      std::exit(0);
    } else {
      throw std::runtime_error{std::format("unknown argument: {}", arg)};
    }
  }
  return out;
}

[[nodiscard]] auto percentile(std::vector<double> values, double p) -> double {
  std::sort(values.begin(), values.end());
  const auto index = std::min(
      values.size() - 1,
      static_cast<std::size_t>(std::ceil(values.size() * p)) - 1);
  return values[index];
}

template <typename Operation>
[[nodiscard]] auto measure(const Options& options, Result result,
                           int minimum_iterations,
                           Operation&& operation) -> Result {
  int iterations = minimum_iterations;
  if (!options.smoke) {
    while (iterations < 4096) {
      const auto started = Clock::now();
      for (int i = 0; i < iterations; ++i)
        g_observed.fetch_xor(operation(), std::memory_order_relaxed);
      const auto elapsed =
          std::chrono::duration<double, std::milli>(Clock::now() - started)
              .count();
      if (elapsed >= 8.0) break;
      iterations *= 2;
    }
  }
  for (int n = 0; n < options.warmup; ++n) {
    for (int i = 0; i < iterations; ++i)
      g_observed.fetch_xor(operation(), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(options.samples));
  std::uint64_t checksum = 0;
  for (int n = 0; n < options.samples; ++n) {
    const auto started = Clock::now();
    for (int i = 0; i < iterations; ++i) {
      const std::uint64_t value = operation();
      checksum ^= value + 0x9E3779B97F4A7C15ULL + (checksum << 6) +
                  (checksum >> 2);
    }
    const auto stopped = Clock::now();
    samples.push_back(
        std::chrono::duration<double, std::milli>(stopped - started).count() /
        iterations);
  }
  g_observed.fetch_xor(checksum, std::memory_order_relaxed);
  result.checksum = checksum;
  result.iterations = iterations;
  result.median_ms = percentile(samples, 0.5);
  result.p95_ms = percentile(samples, 0.95);
  if (result.input_bytes != 0 && result.median_ms > 0.0) {
    result.throughput_mib_s =
        static_cast<double>(result.input_bytes) / (1024.0 * 1024.0) /
        (result.median_ms / 1000.0);
  }
  return result;
}

[[nodiscard]] auto pixels(int width, int height, int seed = 0)
    -> std::vector<Pixel> {
  std::vector<Pixel> out(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = Pixel{static_cast<std::uint8_t>((i * 17 + seed) & 0xFF),
                   static_cast<std::uint8_t>((i * 29 + seed * 3) & 0xFF),
                   static_cast<std::uint8_t>((i * 43 + seed * 7) & 0xFF),
                   static_cast<std::uint8_t>(64 + (i % 192))};
  }
  return out;
}

[[nodiscard]] auto opaque_pixels(int width, int height, int seed = 0)
    -> std::vector<Pixel> {
  auto out = pixels(width, height, seed);
  for (auto& pixel : out) pixel.a = 255;
  return out;
}

[[nodiscard]] auto run_kernels(const Options& options) -> std::vector<Result> {
  const int width = options.smoke ? 64 : 640;
  const int height = options.smoke ? 48 : 384;
  const int iterations = options.smoke ? 1 : 3;
  Image src{width, height, pixels(width, height, 1)};
  Image dst{width, height, pixels(width, height, 2)};
  const auto bytes = std::as_bytes(src.pixels());
  std::vector<Result> out;
  const auto base = [&](std::string name) {
    Result r{.suite = "kernels", .name = std::move(name)};
    r.input_bytes = bytes.size();
    return r;
  };

  out.push_back(measure(options, base("payload_hash"), iterations, [&] {
    return detail::payload_hash(bytes, Extent{width, height},
                                ImageFormat::Rgba32);
  }));
  Result base64_result = base("base64_encode");
  base64_result.output_bytes = ((bytes.size() + 2) / 3) * 4;
  out.push_back(measure(options, base64_result, iterations, [&] {
    const auto encoded = detail::base64_encode(bytes);
    return static_cast<std::uint64_t>(encoded.size()) ^
           static_cast<unsigned char>(encoded.front());
  }));
  out.push_back(measure(options, base("image_fill"), iterations, [&] {
    dst.fill(Rect{0, 0, width, height}, Pixel{11, 22, 33, 44});
    return static_cast<std::uint64_t>(dst.at(width - 1, height - 1).b);
  }));
  out.push_back(measure(options, base("image_blit"), iterations, [&] {
    dst.blit(src, 0, 0);
    return static_cast<std::uint64_t>(dst.at(width / 2, height / 2).g);
  }));
  out.push_back(measure(options, base("image_blend"), iterations, [&] {
    dst.blend(src, 0, 0);
    return static_cast<std::uint64_t>(dst.at(width / 2, height / 2).r);
  }));

  const std::string ascii(4096, 'A');
  const std::string controls = ascii + "\033[31m" + ascii + "\033[0m";
  Result sanitize{.suite = "kernels", .name = "sanitize"};
  sanitize.input_bytes = controls.size();
  out.push_back(measure(options, sanitize, options.smoke ? 1 : 32, [&] {
    const auto clean = text::sanitize(controls);
    return static_cast<std::uint64_t>(clean.size());
  }));
  Result width_result{.suite = "kernels", .name = "display_width"};
  width_result.input_bytes = ascii.size();
  out.push_back(measure(options, width_result, options.smoke ? 1 : 128, [&] {
    return static_cast<std::uint64_t>(detail::display_width(ascii));
  }));

  std::vector<Cell> cells_a(static_cast<std::size_t>(width) * height);
  std::vector<Cell> cells_b = cells_a;
  Result cell_result{.suite = "kernels", .name = "cell_compare"};
  cell_result.input_bytes = cells_a.size() * sizeof(Cell) * 2;
  out.push_back(measure(options, cell_result, iterations, [&] {
    std::uint64_t equal = 0;
    for (std::size_t i = 0; i < cells_a.size(); ++i)
      equal += cells_a[i] == cells_b[i];
    return equal;
  }));

  const int driver_cols = options.smoke ? 16 : 80;
  const int driver_rows = options.smoke ? 8 : 24;
  Image driver_image{driver_cols, driver_rows * 2,
                     opaque_pixels(driver_cols, driver_rows * 2, 4)};
  AnsiRgbDriver ansi;
  FallbackDriver fallback;
  CountingSink ansi_sink;
  CountingSink fallback_sink;
  ansi.set_output(&ansi_sink);
  fallback.set_output(&fallback_sink);
  const Rect driver_rect{0, 0, driver_cols, driver_rows};
  Result ansi_result{.suite = "kernels", .name = "ansi_half_block"};
  ansi_result.input_bytes = std::as_bytes(driver_image.pixels()).size();
  auto ansi_measurement = measure(options, ansi_result, 1, [&] {
    ansi_sink.reset_frame();
    if (auto ok = ansi.draw_image(driver_rect, driver_image); !ok)
      throw std::runtime_error{ok.error().message};
    ansi.flush();
    return ansi_sink.checksum() ^ ansi_sink.last_bytes();
  });
  ansi_measurement.output_bytes = ansi_sink.last_bytes();
  out.push_back(std::move(ansi_measurement));
  Image fallback_image{driver_cols, driver_rows,
                       opaque_pixels(driver_cols, driver_rows, 5)};
  Result fallback_result{.suite = "kernels", .name = "fallback_luminance"};
  fallback_result.input_bytes = std::as_bytes(fallback_image.pixels()).size();
  auto fallback_measurement = measure(options, fallback_result, 1, [&] {
    fallback_sink.reset_frame();
    if (auto ok = fallback.draw_image(driver_rect, fallback_image); !ok)
      throw std::runtime_error{ok.error().message};
    fallback.flush();
    return fallback_sink.checksum() ^ fallback_sink.last_bytes();
  });
  fallback_measurement.output_bytes = fallback_sink.last_bytes();
  out.push_back(std::move(fallback_measurement));
  return out;
}

struct W3Case {
  std::string_view content;
  std::string_view first;
  std::string_view second;
  int width;
};

[[nodiscard]] auto run_w3_case(const Options& options, int cols, int rows,
                               int dirty_percent, const W3Case& fixture)
    -> Result {
  FallbackDriver driver;
  CountingSink sink;
  driver.set_output(&sink);
  Renderer renderer{driver};
  Screen screen{cols, rows};
  const Rgb fg{220, 220, 230};
  const Rgb bg{8, 10, 16};
  const int anchors_per_row = (cols + fixture.width - 1) / fixture.width;
  const int anchors = anchors_per_row * rows;
  const int changed = (anchors * dirty_percent + 99) / 100;
  bool phase = false;

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; x += fixture.width)
      screen.write_text(x, y, fixture.first, fg, bg);
  }
  renderer.present(screen);
  renderer.flush();

  Result result{.suite = "w3",
                .name = std::format("{}x{}-{}-{}", cols, rows,
                                    fixture.content, dirty_percent),
                .content = std::string{fixture.content},
                .cols = cols,
                .rows = rows,
                .dirty_percent = dirty_percent,
                .input_bytes = 0};
  auto measurement = measure(options, result, options.smoke ? 1 : 2, [&] {
    phase = !phase;
    const auto glyph = phase ? fixture.second : fixture.first;
    for (int index = 0; index < changed; ++index) {
      const int y = index / anchors_per_row;
      const int x = (index % anchors_per_row) * fixture.width;
      screen.write_text(x, y, glyph, fg, bg);
    }
    sink.reset_frame();
    renderer.present(screen);
    renderer.flush();
    return sink.checksum() ^ sink.last_bytes();
  });
  measurement.output_bytes = sink.last_bytes();
  return measurement;
}

[[nodiscard]] auto run_w3(const Options& options) -> std::vector<Result> {
  static constexpr std::array sizes = {
      std::pair{80, 24}, std::pair{120, 40}, std::pair{200, 50},
      std::pair{300, 80}, std::pair{400, 120}};
  static constexpr std::array dirty = {0, 10, 50, 100};
  static constexpr std::array fixtures = {
      W3Case{"ascii", "A", "B", 1},
      W3Case{"cjk", "界", "語", 2},
      W3Case{"combining", "e\xCC\x81", "o\xCC\x82", 1}};
  std::vector<Result> out;
  const std::size_t size_count = options.smoke ? 2 : sizes.size();
  const std::size_t fixture_count = options.smoke ? 2 : fixtures.size();
  const std::size_t dirty_count = options.smoke ? 2 : dirty.size();
  for (std::size_t s = 0; s < size_count; ++s) {
    for (std::size_t f = 0; f < fixture_count; ++f) {
      for (std::size_t d = 0; d < dirty_count; ++d) {
        out.push_back(run_w3_case(options, sizes[s].first, sizes[s].second,
                                  dirty[d], fixtures[f]));
      }
    }
  }
  return out;
}

[[nodiscard]] auto derive_walls(const std::vector<Result>& results)
    -> std::vector<Wall> {
  std::vector<Wall> walls;
  for (const std::string_view content : {"ascii", "cjk", "combining"}) {
    for (const int dirty : {0, 10, 50, 100}) {
      for (const double budget : {16.6, 33.3}) {
        Wall wall{.content = std::string{content},
                  .dirty_percent = dirty,
                  .budget_ms = budget,
                  .largest_passing = "none",
                  .first_failing = std::nullopt};
        for (const auto& result : results) {
          if (result.suite != "w3" || result.content != content ||
              result.dirty_percent != dirty)
            continue;
          const auto size = std::format("{}x{}", result.cols, result.rows);
          if (result.median_ms <= budget) {
            wall.largest_passing = size;
          } else if (!wall.first_failing) {
            wall.first_failing = size;
          }
        }
        if (wall.largest_passing != "none" || wall.first_failing)
          walls.push_back(std::move(wall));
      }
    }
  }
  return walls;
}

[[nodiscard]] auto json_escape(std::string_view value) -> std::string {
  std::string out;
  out.reserve(value.size() + 8);
  for (const unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) out += std::format("\\u{:04x}", c);
        else out += static_cast<char>(c);
    }
  }
  return out;
}

[[nodiscard]] auto host_name() -> std::string {
  struct utsname name {};
  if (::uname(&name) != 0) return "unknown";
  return std::format("{} {} {}", name.sysname, name.release, name.machine);
}

[[nodiscard]] auto suite_name(Suite suite) -> std::string_view {
  switch (suite) {
    case Suite::Kernels: return "kernels";
    case Suite::W3: return "w3";
    case Suite::All: return "all";
  }
  return "unknown";
}

[[nodiscard]] auto requested_tier_name(KernelChoice choice)
    -> std::string_view {
  switch (choice) {
    case KernelChoice::Auto: return "auto";
    case KernelChoice::Scalar: return "scalar";
    case KernelChoice::Avx2: return "avx2";
  }
  return "unknown";
}

[[nodiscard]] auto resolved_tier_name() -> std::string_view {
  return detail::resolved_kernel_tier() == detail::KernelTier::Avx2 ? "avx2"
                                                                    : "scalar";
}

[[nodiscard]] auto json_report(const Options& options,
                               const std::vector<Result>& results,
                               const std::vector<Wall>& walls) -> std::string {
  std::string out = std::format(
      "{{\n  \"schema_version\": 2,\n  \"termforge_version\": \"{}\",\n"
      "  \"compiler\": \"{}\",\n  \"host\": \"{}\",\n"
      "  \"build_type\": \"Release\",\n  \"requested_suite\": \"{}\",\n"
      "  \"requested_kernel_tier\": \"{}\",\n"
      "  \"resolved_kernel_tier\": \"{}\",\n"
      "  \"samples\": {},\n  \"warmup\": {},\n  \"smoke\": {},\n"
      "  \"results\": [\n",
      json_escape(TERMFORGE_BENCH_VERSION), json_escape(__VERSION__),
      json_escape(host_name()), suite_name(options.suite),
      requested_tier_name(options.kernel_tier), resolved_tier_name(),
      options.samples, options.warmup,
      options.smoke ? "true" : "false");
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out += std::format(
        "    {{\"suite\":\"{}\",\"name\":\"{}\",\"content\":\"{}\","
        "\"cols\":{},\"rows\":{},\"dirty_percent\":{},"
        "\"input_bytes\":{},\"output_bytes\":{},\"checksum\":\"{:016x}\","
        "\"iterations\":{},"
        "\"median_ms\":{:.6f},\"p95_ms\":{:.6f},"
        "\"throughput_mib_s\":{:.3f}}}{}\n",
        json_escape(r.suite), json_escape(r.name), json_escape(r.content),
        r.cols, r.rows, r.dirty_percent, r.input_bytes, r.output_bytes,
        r.checksum, r.iterations, r.median_ms, r.p95_ms, r.throughput_mib_s,
        i + 1 == results.size() ? "" : ",");
  }
  out += "  ],\n  \"walls\": [\n";
  for (std::size_t i = 0; i < walls.size(); ++i) {
    const auto& w = walls[i];
    out += std::format(
        "    {{\"content\":\"{}\",\"dirty_percent\":{},"
        "\"budget_ms\":{:.1f},\"largest_passing\":\"{}\","
        "\"first_failing\":{}}}{}\n",
        json_escape(w.content), w.dirty_percent, w.budget_ms,
        json_escape(w.largest_passing),
        w.first_failing ? std::format("\"{}\"", json_escape(*w.first_failing))
                        : "null",
        i + 1 == walls.size() ? "" : ",");
  }
  out += "  ]\n}\n";
  return out;
}

[[nodiscard]] auto table_report(const Options& options,
                                const std::vector<Result>& results,
                                const std::vector<Wall>& walls) -> std::string {
  std::string out = std::format("TermForge {} performance evidence\n{}\n{}\n\n",
                                TERMFORGE_BENCH_VERSION, __VERSION__, host_name());
  out += std::format("kernel tier: {} -> {}\n\n",
                     requested_tier_name(options.kernel_tier),
                     resolved_tier_name());
  out += std::format("{:<9} {:<30} {:>10} {:>10} {:>11}\n", "suite", "case",
                     "median ms", "p95 ms", "MiB/s");
  for (const auto& r : results) {
    out += std::format("{:<9} {:<30} {:>10.4f} {:>10.4f} {:>11.2f}\n",
                       r.suite, r.name, r.median_ms, r.p95_ms,
                       r.throughput_mib_s);
  }
  if (!walls.empty()) {
    out += "\nW3 walls\n";
    for (const auto& w : walls) {
      out += std::format("  {:<10} dirty {:>3}% @ {:>4.1f} ms: pass {}, fail {}\n",
                         w.content, w.dirty_percent, w.budget_ms,
                         w.largest_passing,
                         w.first_failing.value_or("not reached"));
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    std::optional<detail::KernelTier> override;
    if (options.kernel_tier == KernelChoice::Scalar)
      override = detail::KernelTier::Scalar;
    if (options.kernel_tier == KernelChoice::Avx2)
      override = detail::KernelTier::Avx2;
    if (!detail::set_kernel_tier_override(override))
      throw std::runtime_error{
          "requested AVX2 kernel tier is unsupported on this host"};
    std::vector<Result> results;
    if (options.suite == Suite::Kernels || options.suite == Suite::All) {
      auto kernels = run_kernels(options);
      results.insert(results.end(), std::make_move_iterator(kernels.begin()),
                     std::make_move_iterator(kernels.end()));
    }
    if (options.suite == Suite::W3 || options.suite == Suite::All) {
      auto w3 = run_w3(options);
      results.insert(results.end(), std::make_move_iterator(w3.begin()),
                     std::make_move_iterator(w3.end()));
    }
    const auto walls = derive_walls(results);
    const std::string report = options.format == Format::Json
                                   ? json_report(options, results, walls)
                                   : table_report(options, results, walls);
    if (options.output) {
      std::ofstream file{*options.output};
      if (!file) throw std::runtime_error{"cannot open output file"};
      file << report;
      if (!file) throw std::runtime_error{"cannot write output file"};
    } else {
      std::cout << report;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "termforge_bench: " << error.what() << '\n';
    return 2;
  }
}
