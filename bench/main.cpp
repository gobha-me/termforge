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
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/utsname.h>

#include "detail/base64.hpp"
#include "detail/payload_hash.hpp"
#include "detail/simd.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/text.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;

namespace {

using Clock = std::chrono::steady_clock;

enum class Suite { Kernels, W2, W3, W4, All };
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

struct PhaseStats {
  double median_ms{};
  double p95_ms{};
};

struct W2Result {
  std::string path;
  int canvas_w{};
  int canvas_h{};
  int dirty_w{};
  int dirty_h{};
  int samples{};
  int motion_events{};
  PhaseStats input_to_write;
  PhaseStats tick;
  PhaseStats application_render;
  PhaseStats framework_submission;
  PhaseStats sink_write;
  PhaseStats frame_work;
  FrameBytes bytes;
  ImageResidency residency_before;
  ImageResidency residency_after;
  double wire_mib_s_30{};
  double wire_mib_s_60{};
  std::uint64_t checksum{};
};

struct W2Wall {
  std::string path;
  int dirty_w{};
  int dirty_h{};
  double budget_ms{};
  int stroke_hz{};
  std::optional<Extent> largest_passing;
  std::optional<Extent> first_failing;
};

struct W4Result {
  std::string mode;
  int region_count{};
  int cell_w{};
  int cell_h{};
  int samples{};
  PhaseStats tick;
  PhaseStats application_render;
  PhaseStats framework_submission;
  PhaseStats sink_write;
  PhaseStats frame_work;
  FrameBytes bytes;
  ImageResidency residency;
  std::uint64_t checksum{};
};

struct W4BudgetWall {
  double budget_ms{};
  std::optional<int> largest_passing_count;
  std::optional<int> first_failing_count;
};

struct W4Wall {
  std::string mode;
  int cell_w{};
  int cell_h{};
  std::optional<int> first_retransmit_count;
  std::array<W4BudgetWall, 2> budgets;
};

std::atomic<std::uint64_t> g_observed{0};

class CountingSink final : public ByteSink {
 public:
  auto reset_frame() noexcept -> void { m_last_bytes = 0; }
  auto mark_input() noexcept -> void { m_input_started = Clock::now(); }
  [[nodiscard]] auto last_bytes() const noexcept -> std::uint64_t {
    return m_last_bytes;
  }
  [[nodiscard]] auto last_input_to_write() const noexcept
      -> std::chrono::nanoseconds {
    return m_last_input_to_write;
  }
  [[nodiscard]] auto checksum() const noexcept -> std::uint64_t {
    return m_checksum;
  }

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    if (m_input_started) {
      m_last_input_to_write =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - *m_input_started);
      m_input_started.reset();
    }
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
  std::optional<Clock::time_point> m_input_started;
  std::chrono::nanoseconds m_last_input_to_write{};
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
      else if (v == "w2") out.suite = Suite::W2;
      else if (v == "w3") out.suite = Suite::W3;
      else if (v == "w4") out.suite = Suite::W4;
      else if (v == "all") out.suite = Suite::All;
      else throw std::runtime_error{"--suite must be kernels, w2, w3, w4, or all"};
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
          << "Usage: termforge_bench [--suite kernels|w2|w3|w4|all] "
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

template <typename Value, typename Projection>
[[nodiscard]] auto phase_stats(const std::vector<Value>& values,
                               std::size_t begin, Projection&& projection)
    -> PhaseStats {
  std::vector<double> samples;
  samples.reserve(values.size() - begin);
  for (std::size_t index = begin; index < values.size(); ++index) {
    samples.push_back(
        std::chrono::duration<double, std::milli>(projection(values[index]))
            .count());
  }
  return {.median_ms = percentile(samples, 0.5),
          .p95_ms = percentile(samples, 0.95)};
}

enum class W2Path { Replace, Edit };

[[nodiscard]] constexpr auto w2_path_name(W2Path path) noexcept
    -> std::string_view {
  return path == W2Path::Replace ? "replace" : "edit";
}

// W2 measures the production input-to-write shape. Each frame receives one
// SGR buttonless-motion record, mutates the same logical canvas, then either
// replaces the complete pinned root or sends only the dirty block. The sink's
// timestamp begins when the synthetic fd makes the input bytes available, so
// input_to_write includes parsing and dispatch in addition to
// FrameObservation's tick/render/submission partitions.
class W2App final : public App {
 public:
  W2App(W2Path path, Extent canvas, Extent dirty)
      : m_path{path},
        m_canvas{canvas},
        m_dirty{dirty},
        m_root{canvas.w, canvas.h, opaque_pixels(canvas.w, canvas.h, 11)},
        m_block{dirty.w, dirty.h, opaque_pixels(dirty.w, dirty.h, 12)} {
    set_frame_ms(0);
    set_mouse_mode(MouseMode::Motion);
    set_frame_observer([this](const FrameObservation& observation) {
      m_observations.push_back(observation);
      m_residencies.push_back(driver().residency());
      m_checksums.push_back(m_sink.checksum());
      m_input_to_write.push_back(m_sink.last_input_to_write());
      m_motion_counts.push_back(m_motion_events);
      m_input_served = false;
    });
  }

  auto run(int frames) -> void {
    auto selected = std::make_unique<KittyDriver>();
    selected->set_placement_mode(KittyDriver::PlacementMode::Classic);
    test_run_frames(frames, 128, 64, static_cast<std::string*>(nullptr),
                    std::move(selected));
    if (!m_errors.empty()) throw std::runtime_error{m_errors.front().message};
  }

  [[nodiscard]] auto observations() const
      -> const std::vector<FrameObservation>& {
    return m_observations;
  }
  [[nodiscard]] auto residencies() const -> const std::vector<ImageResidency>& {
    return m_residencies;
  }
  [[nodiscard]] auto checksums() const -> const std::vector<std::uint64_t>& {
    return m_checksums;
  }
  [[nodiscard]] auto input_to_write() const
      -> const std::vector<std::chrono::nanoseconds>& {
    return m_input_to_write;
  }
  [[nodiscard]] auto motion_counts() const -> const std::vector<int>& {
    return m_motion_counts;
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      m_errors.push_back(*error);
      return;
    }
    const auto* mouse = std::get_if<MouseEvent>(&event);
    if (mouse == nullptr || mouse->action() != MouseAction::Move) return;
    m_mouse = *mouse;
    m_has_mouse = true;
    ++m_motion_events;
  }

  auto on_render(Screen& screen) -> void override {
    if (!m_bound) {
      driver().set_output(&m_sink);
      m_bound = true;
    }
    screen.fill_rect(0, 0, 80, 24, {}, {});
    m_changed = false;
    if (!m_pinned || !m_has_mouse) return;

    const int max_x = m_canvas.w - m_dirty.w;
    const int max_y = m_canvas.h - m_dirty.h;
    m_destination = PixelPoint{max_x == 0 ? 0 : (m_mouse.x * max_x) / 127,
                               max_y == 0 ? 0 : (m_mouse.y * max_y) / 63};
    const auto seed = static_cast<std::uint8_t>(m_motion_events & 0xFF);
    const Pixel colour{static_cast<std::uint8_t>(seed ^ 0x5A),
                       static_cast<std::uint8_t>(seed * 3U),
                       static_cast<std::uint8_t>(seed * 7U), 255};
    m_block.fill(Rect{0, 0, m_dirty.w, m_dirty.h}, colour);
    m_root.blit(m_block, m_destination.x, m_destination.y);
    m_changed = true;
  }

  auto on_pixels(TerminalDriver& selected) -> void override {
    constexpr Rect placement{0, 0, 80, 24};
    if (!m_pinned) {
      auto pinned = selected.pin_image(m_root);
      if (!pinned) {
        m_errors.push_back(std::move(pinned.error()));
        return;
      }
      m_pinned = *pinned;
      if (auto placed = selected.draw_pinned(placement, *m_pinned); !placed)
        m_errors.push_back(std::move(placed.error()));
      return;
    }

    if (auto retained = selected.retain_pinned(placement, *m_pinned);
        !retained) {
      m_errors.push_back(std::move(retained.error()));
      return;
    }
    if (!m_changed) return;

    auto submitted = m_path == W2Path::Replace
                         ? selected.replace_pinned(*m_pinned, m_root)
                         : selected.edit_pinned(*m_pinned, m_destination,
                                                m_block,
                                                ImageComposition::Overwrite);
    if (!submitted) m_errors.push_back(std::move(submitted.error()));
  }

 protected:
  auto wait_readable(int) -> bool override { return false; }

  auto read_available(char* out, int max) -> int override {
    if (m_input_served) return 0;
    const int x = 1 + (m_input_sequence * 37) % 128;
    const int y = 1 + (m_input_sequence * 23) % 64;
    const std::string input = std::format("\033[<35;{};{}M", x, y);
    if (input.size() > static_cast<std::size_t>(max))
      throw std::runtime_error{
          "W2 synthetic mouse record exceeds input buffer"};
    std::copy(input.begin(), input.end(), out);
    ++m_input_sequence;
    m_input_served = true;
    m_sink.mark_input();
    return static_cast<int>(input.size());
  }

 private:
  W2Path m_path;
  Extent m_canvas;
  Extent m_dirty;
  Image m_root;
  Image m_block;
  CountingSink m_sink;
  std::optional<PinnedImage> m_pinned;
  PixelPoint m_destination;
  MouseEvent m_mouse;
  bool m_has_mouse{false};
  bool m_bound{false};
  bool m_changed{false};
  bool m_input_served{false};
  int m_input_sequence{};
  int m_motion_events{};
  std::vector<FrameObservation> m_observations;
  std::vector<ImageResidency> m_residencies;
  std::vector<std::uint64_t> m_checksums;
  std::vector<std::chrono::nanoseconds> m_input_to_write;
  std::vector<int> m_motion_counts;
  std::vector<ErrorEvent> m_errors;
};

[[nodiscard]] auto run_w2_case(const Options& options, W2Path path,
                               Extent canvas, Extent dirty) -> W2Result {
  const int setup_frames = 1;
  const int total_frames = setup_frames + options.warmup + options.samples;
  W2App app{path, canvas, dirty};
  app.run(total_frames);
  const auto& observations = app.observations();
  const auto& residencies = app.residencies();
  const auto& checksums = app.checksums();
  const auto& input_to_write = app.input_to_write();
  const auto& motion_counts = app.motion_counts();
  if (observations.size() != static_cast<std::size_t>(total_frames) ||
      residencies.size() != observations.size() ||
      checksums.size() != observations.size() ||
      input_to_write.size() != observations.size() ||
      motion_counts.size() != observations.size()) {
    throw std::runtime_error{
        "W2 did not observe every input and rendered frame"};
  }

  const std::size_t begin = static_cast<std::size_t>(setup_frames +
                                                     options.warmup);
  const ImageResidency residency_before = residencies[begin - 1];
  const ImageResidency residency_after = residencies.back();
  std::vector<FrameBytes> frame_bytes;
  frame_bytes.reserve(observations.size() - begin);
  std::uint64_t checksum = 14695981039346656037ULL;
  for (std::size_t index = begin; index < observations.size(); ++index) {
    const FrameBytes bytes = observations[index].bytes;
    if (!observations[index].output_accepted)
      throw std::runtime_error{"W2 sink refused a sampled frame"};
    if (residencies[index].region_images != 0 ||
        residencies[index].pinned_images != 1) {
      throw std::runtime_error{"W2 lost its one-pinned-root residency shape"};
    }
    if (path == W2Path::Replace &&
        (bytes.image_transmit == 0 || bytes.image_edit != 0)) {
      throw std::runtime_error{"W2 replace path lost its transmit-only shape"};
    }
    if (path == W2Path::Edit &&
        (bytes.image_transmit != 0 || bytes.image_edit == 0)) {
      throw std::runtime_error{"W2 edit path lost its edit-only shape"};
    }
    frame_bytes.push_back(bytes);
    checksum ^= checksums[index] + bytes.total() +
                static_cast<std::uint64_t>(motion_counts[index]) +
                0x9E3779B97F4A7C15ULL + (checksum << 6) + (checksum >> 2);
  }
  std::sort(frame_bytes.begin(), frame_bytes.end(),
            [](const FrameBytes& left, const FrameBytes& right) {
              return left.total() < right.total();
            });
  const FrameBytes bytes = frame_bytes[(frame_bytes.size() - 1) / 2];

  const std::uint64_t payload_delta = residency_after.source_payload_bytes -
                                      residency_before.source_payload_bytes;
  const std::uint64_t block_bytes = static_cast<std::uint64_t>(dirty.w) *
                                    static_cast<std::uint64_t>(dirty.h) *
                                    sizeof(Pixel);
  const std::uint64_t expected_delta = path == W2Path::Edit
                                           ? block_bytes *
                                                 static_cast<std::uint64_t>(
                                                     options.samples)
                                           : 0;
  if (payload_delta != expected_delta) {
    throw std::runtime_error{
        std::format("W2 {} residency delta {} != accepted payload {}",
                    w2_path_name(path), payload_delta, expected_delta)};
  }
  const int measured_motion_events = motion_counts.back() -
                                     motion_counts[begin - 1];
  if (measured_motion_events != options.samples)
    throw std::runtime_error{"W2 did not deliver one motion per sampled frame"};

  const auto total = [](const FrameObservation& observation) {
    return observation.tick + observation.application_render +
           observation.framework_submission + observation.sink_write;
  };
  const double frame_mib = static_cast<double>(bytes.total()) /
                           (1024.0 * 1024.0);
  return {
      .path = std::string{w2_path_name(path)},
      .canvas_w = canvas.w,
      .canvas_h = canvas.h,
      .dirty_w = dirty.w,
      .dirty_h = dirty.h,
      .samples = options.samples,
      .motion_events = measured_motion_events,
      .input_to_write = phase_stats(input_to_write, begin,
                                    [](const auto value) { return value; }),
      .tick = phase_stats(observations, begin,
                          [](const auto& frame) { return frame.tick; }),
      .application_render = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.application_render; }),
      .framework_submission = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.framework_submission; }),
      .sink_write = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.sink_write; }),
      .frame_work = phase_stats(observations, begin, total),
      .bytes = bytes,
      .residency_before = residency_before,
      .residency_after = residency_after,
      .wire_mib_s_30 = frame_mib * 30.0,
      .wire_mib_s_60 = frame_mib * 60.0,
      .checksum = checksum,
  };
}

[[nodiscard]] auto run_w2(const Options& options) -> std::vector<W2Result> {
  static constexpr std::array canvases = {Extent{320, 180}, Extent{640, 360},
                                          Extent{1280, 720},
                                          Extent{1920, 1080}};
  static constexpr std::array smoke_canvases = {Extent{64, 48},
                                                Extent{128, 96}};
  static constexpr std::array dirty = {Extent{1, 1}, Extent{8, 8},
                                       Extent{32, 32}, Extent{128, 128}};
  static constexpr std::array paths = {W2Path::Replace, W2Path::Edit};
  std::vector<W2Result> out;
  for (const auto path : paths) {
    const std::size_t dirty_count = options.smoke ? 2 : dirty.size();
    for (std::size_t d = 0; d < dirty_count; ++d) {
      if (options.smoke) {
        for (const auto canvas : smoke_canvases)
          out.push_back(run_w2_case(options, path, canvas, dirty[d]));
      } else {
        for (const auto canvas : canvases)
          out.push_back(run_w2_case(options, path, canvas, dirty[d]));
      }
    }
  }
  return out;
}

[[nodiscard]] auto derive_w2_walls(const std::vector<W2Result>& results)
    -> std::vector<W2Wall> {
  static constexpr std::array paths = {std::string_view{"replace"},
                                       std::string_view{"edit"}};
  static constexpr std::array dirty = {Extent{1, 1}, Extent{8, 8},
                                       Extent{32, 32}, Extent{128, 128}};
  std::vector<W2Wall> walls;
  for (const auto path : paths) {
    for (const auto block : dirty) {
      for (const auto& [budget_ms, stroke_hz] :
           {std::pair{16.6, 60}, std::pair{33.3, 30}}) {
        W2Wall wall{.path = std::string{path},
                    .dirty_w = block.w,
                    .dirty_h = block.h,
                    .budget_ms = budget_ms,
                    .stroke_hz = stroke_hz,
                    .largest_passing = std::nullopt,
                    .first_failing = std::nullopt};
        bool found = false;
        for (const auto& result : results) {
          if (result.path != path || result.dirty_w != block.w ||
              result.dirty_h != block.h) {
            continue;
          }
          found = true;
          const Extent canvas{result.canvas_w, result.canvas_h};
          if (result.input_to_write.median_ms <= budget_ms)
            wall.largest_passing = canvas;
          else if (!wall.first_failing)
            wall.first_failing = canvas;
        }
        if (found) walls.push_back(std::move(wall));
      }
    }
  }
  return walls;
}

[[nodiscard]] constexpr auto w4_region_key(Rect rect) noexcept
    -> std::uint64_t {
  return static_cast<std::uint64_t>(static_cast<std::uint16_t>(rect.x)) |
         (static_cast<std::uint64_t>(static_cast<std::uint16_t>(rect.y))
          << 16) |
         (static_cast<std::uint64_t>(static_cast<std::uint16_t>(rect.w))
          << 32) |
         (static_cast<std::uint64_t>(static_cast<std::uint16_t>(rect.h)) << 48);
}

// W4 uses the production App caller order, not a replay: draw the Baseline,
// collect every region, queue the image window, then perform the frame's one
// write. Immediate deliberately exercises Kitty's ordinary 16-slot region
// cache; Persistent uses the retained pin path current forge-top waveforms use.
class W4Regions final : public Widget {
 public:
  W4Regions(PixelRegionMode mode, int count, Extent cells) : m_mode{mode} {
    constexpr int kScreenCols = 128;
    const int per_row = kScreenCols / cells.w;
    m_regions.reserve(static_cast<std::size_t>(count));
    m_images.resize(static_cast<std::size_t>(count));
    m_dirty.resize(static_cast<std::size_t>(count), true);
    for (int index = 0; index < count; ++index) {
      const Rect region{(index % per_row) * cells.w,
                        (index / per_row) * cells.h, cells.w, cells.h};
      m_lookup.emplace(w4_region_key(region), static_cast<std::size_t>(index));
      m_regions.push_back(region);
    }
  }

  auto draw(Screen& screen) -> void override {
    for (const Rect region : m_regions)
      screen.fill_rect(region.x, region.y, region.w, region.h, {}, {});
    clear_dirty();
  }

  auto pixel_regions() -> std::vector<Rect> override { return m_regions; }

  auto draw_pixels(Rect region, Extent pixels) -> const Image* override {
    const auto index = index_of(region);
    if (!index || pixels.empty()) return nullptr;
    auto& image = m_images[*index];
    if (image.width() != pixels.w || image.height() != pixels.h) {
      image = Image{pixels.w, pixels.h,
                    opaque_pixels(pixels.w, pixels.h,
                                  static_cast<int>(*index) + 1)};
    }
    return &image;
  }

  [[nodiscard]] auto pixel_region_state(Rect region) const noexcept
      -> PixelRegionState override {
    const auto index = index_of(region);
    return {.mode = m_mode,
            .content_dirty = index ? m_dirty[*index] : true,
            .content_revision = 1};
  }

  auto pixel_region_submitted(Rect region) noexcept -> void override {
    if (const auto index = index_of(region)) m_dirty[*index] = false;
  }

 private:
  [[nodiscard]] auto index_of(Rect region) const noexcept
      -> std::optional<std::size_t> {
    const auto found = m_lookup.find(w4_region_key(region));
    if (found == m_lookup.end()) return std::nullopt;
    return found->second;
  }

  PixelRegionMode m_mode;
  std::vector<Rect> m_regions;
  std::vector<Image> m_images;
  std::vector<bool> m_dirty;
  std::unordered_map<std::uint64_t, std::size_t> m_lookup;
};

class W4App final : public App {
 public:
  W4App(PixelRegionMode mode, int count, Extent cells)
      : m_regions{mode, count, cells} {
    set_frame_ms(0);
    set_frame_observer([this](const FrameObservation& observation) {
      m_observations.push_back(observation);
      m_residencies.push_back(driver().residency());
      m_checksums.push_back(m_sink.checksum());
    });
  }

  auto run(int frames) -> void {
    auto selected = std::make_unique<KittyDriver>();
    selected->set_placement_mode(KittyDriver::PlacementMode::Classic);
    test_run_frames(frames, 128, 64, static_cast<std::string*>(nullptr),
                    std::move(selected));
    if (!m_errors.empty()) throw std::runtime_error{m_errors.front().message};
  }

  [[nodiscard]] auto observations() const
      -> const std::vector<FrameObservation>& {
    return m_observations;
  }
  [[nodiscard]] auto residencies() const
      -> const std::vector<ImageResidency>& {
    return m_residencies;
  }
  [[nodiscard]] auto checksums() const -> const std::vector<std::uint64_t>& {
    return m_checksums;
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event))
      m_errors.push_back(*error);
  }

  auto on_render(Screen& screen) -> void override {
    if (!m_bound) {
      driver().set_output(&m_sink);
      m_bound = true;
    }
    m_regions.draw(screen);
    render_pixel_regions(m_regions);
  }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  W4Regions m_regions;
  CountingSink m_sink;
  bool m_bound{false};
  std::vector<FrameObservation> m_observations;
  std::vector<ImageResidency> m_residencies;
  std::vector<std::uint64_t> m_checksums;
  std::vector<ErrorEvent> m_errors;
};

[[nodiscard]] auto run_w4_case(const Options& options, PixelRegionMode mode,
                               int region_count, Extent cells) -> W4Result {
  const int setup_frames = 1;
  const int total_frames = setup_frames + options.warmup + options.samples;
  W4App app{mode, region_count, cells};
  app.run(total_frames);
  const auto& observations = app.observations();
  const auto& residencies = app.residencies();
  const auto& checksums = app.checksums();
  if (observations.size() != static_cast<std::size_t>(total_frames) ||
      residencies.size() != observations.size() ||
      checksums.size() != observations.size()) {
    throw std::runtime_error{"W4 did not observe every rendered frame"};
  }

  const std::size_t begin =
      static_cast<std::size_t>(setup_frames + options.warmup);
  const ImageResidency residency = residencies[begin];
  std::vector<FrameBytes> frame_bytes;
  frame_bytes.reserve(observations.size() - begin);
  std::uint64_t checksum = 14695981039346656037ULL;
  for (std::size_t index = begin; index < observations.size(); ++index) {
    if (!observations[index].output_accepted ||
        residency != residencies[index]) {
      throw std::runtime_error{std::format(
          "W4 {} {}x{} count {} sample {} changed residency "
          "{}/{}/{} -> {}/{}/{}",
          mode == PixelRegionMode::Immediate ? "immediate" : "persistent",
          cells.w, cells.h, region_count, index - begin,
          residency.region_images, residency.pinned_images,
          residency.source_payload_bytes,
          residencies[index].region_images, residencies[index].pinned_images,
          residencies[index].source_payload_bytes)};
    }
    frame_bytes.push_back(observations[index].bytes);
    checksum ^= checksums[index] + observations[index].bytes.total() +
                0x9E3779B97F4A7C15ULL + (checksum << 6) + (checksum >> 2);
  }
  std::sort(frame_bytes.begin(), frame_bytes.end(),
            [](const FrameBytes& left, const FrameBytes& right) {
              return left.total() < right.total();
            });
  const FrameBytes bytes = frame_bytes[(frame_bytes.size() - 1) / 2];

  const auto total = [](const FrameObservation& observation) {
    return observation.tick + observation.application_render +
           observation.framework_submission + observation.sink_write;
  };
  return {
      .mode = mode == PixelRegionMode::Immediate ? "immediate" : "persistent",
      .region_count = region_count,
      .cell_w = cells.w,
      .cell_h = cells.h,
      .samples = options.samples,
      .tick = phase_stats(observations, begin,
                          [](const auto& frame) { return frame.tick; }),
      .application_render = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.application_render; }),
      .framework_submission = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.framework_submission; }),
      .sink_write = phase_stats(
          observations, begin,
          [](const auto& frame) { return frame.sink_write; }),
      .frame_work = phase_stats(observations, begin, total),
      .bytes = bytes,
      .residency = residency,
      .checksum = checksum,
  };
}

[[nodiscard]] auto run_w4(const Options& options) -> std::vector<W4Result> {
  static constexpr std::array counts = {1, 8, 16, 17, 32, 64};
  static constexpr std::array sizes = {Extent{1, 1}, Extent{4, 2},
                                       Extent{8, 4}};
  static constexpr std::array modes = {PixelRegionMode::Immediate,
                                       PixelRegionMode::Persistent};
  std::vector<W4Result> out;
  const std::size_t size_count = options.smoke ? 1 : sizes.size();
  for (const auto mode : modes) {
    for (std::size_t size = 0; size < size_count; ++size) {
      if (options.smoke) {
        out.push_back(run_w4_case(options, mode, 16, sizes[size]));
        out.push_back(run_w4_case(options, mode, 17, sizes[size]));
        continue;
      }
      for (const int count : counts)
        out.push_back(run_w4_case(options, mode, count, sizes[size]));
    }
  }
  return out;
}

[[nodiscard]] auto derive_w4_walls(const std::vector<W4Result>& results)
    -> std::vector<W4Wall> {
  static constexpr std::array modes = {std::string_view{"immediate"},
                                       std::string_view{"persistent"}};
  static constexpr std::array sizes = {Extent{1, 1}, Extent{4, 2},
                                       Extent{8, 4}};
  std::vector<W4Wall> walls;
  for (const auto mode : modes) {
    for (const auto cells : sizes) {
      W4Wall wall{.mode = std::string{mode},
                  .cell_w = cells.w,
                  .cell_h = cells.h,
                  .first_retransmit_count = std::nullopt,
                  .budgets = {
                      W4BudgetWall{.budget_ms = 16.6,
                                   .largest_passing_count = std::nullopt,
                                   .first_failing_count = std::nullopt},
                      W4BudgetWall{.budget_ms = 33.3,
                                   .largest_passing_count = std::nullopt,
                                   .first_failing_count = std::nullopt}}};
      bool found = false;
      for (const auto& result : results) {
        if (result.mode != mode || result.cell_w != cells.w ||
            result.cell_h != cells.h) {
          continue;
        }
        found = true;
        if (!wall.first_retransmit_count && result.bytes.image_transmit != 0)
          wall.first_retransmit_count = result.region_count;
        for (auto& budget : wall.budgets) {
          if (result.frame_work.median_ms <= budget.budget_ms) {
            budget.largest_passing_count = result.region_count;
          } else if (!budget.first_failing_count) {
            budget.first_failing_count = result.region_count;
          }
        }
      }
      if (found) walls.push_back(std::move(wall));
    }
  }
  return walls;
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
    case Suite::W2: return "w2";
    case Suite::W3: return "w3";
    case Suite::W4: return "w4";
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
                               const std::vector<Wall>& walls,
                               const std::vector<W2Result> &w2_results,
                               const std::vector<W2Wall> &w2_walls,
                               const std::vector<W4Result>& w4_results,
                               const std::vector<W4Wall>& w4_walls)
    -> std::string {
  std::string out = std::format(
      "{{\n  \"schema_version\": 4,\n  \"termforge_version\": \"{}\",\n"
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
  const auto phase = [](const PhaseStats &value) {
    return std::format("{{\"median\":{:.6f},\"p95\":{:.6f}}}", value.median_ms,
                       value.p95_ms);
  };
  const auto residency = [](const ImageResidency &value) {
    return std::format(
        "{{\"region_images\":{},\"pinned_images\":{},"
        "\"source_payload_bytes\":{}}}",
        value.region_images, value.pinned_images, value.source_payload_bytes);
  };
  const auto optional_extent = [](std::optional<Extent> value) {
    return value ? std::format("{{\"w\":{},\"h\":{}}}", value->w, value->h)
                 : std::string{"null"};
  };

  out += "  ],\n  \"w2_results\": [\n";
  for (std::size_t i = 0; i < w2_results.size(); ++i) {
    const auto &r = w2_results[i];
    out += std::format(
        "    {{\"path\":\"{}\","
        "\"canvas_pixels\":{{\"w\":{},\"h\":{}}},"
        "\"dirty_pixels\":{{\"w\":{},\"h\":{}}},"
        "\"samples\":{},\"motion_events\":{},"
        "\"timing_ms\":{{\"input_to_write\":{},\"tick\":{},"
        "\"application_render\":{},\"framework_submission\":{},"
        "\"sink_write\":{},\"frame_work\":{}}},"
        "\"frame_bytes\":{{\"selection\":\"median_total\","
        "\"cells\":{},\"image_transmit\":{},\"image_edit\":{},"
        "\"total\":{}}},"
        "\"wire_mib_s\":{{\"30_hz\":{:.6f},\"60_hz\":{:.6f}}},"
        "\"residency_before\":{},\"residency_after\":{},"
        "\"checksum\":\"{:016x}\"}}{}\n",
        json_escape(r.path), r.canvas_w, r.canvas_h, r.dirty_w, r.dirty_h,
        r.samples, r.motion_events, phase(r.input_to_write), phase(r.tick),
        phase(r.application_render), phase(r.framework_submission),
        phase(r.sink_write), phase(r.frame_work), r.bytes.cells,
        r.bytes.image_transmit, r.bytes.image_edit, r.bytes.total(),
        r.wire_mib_s_30, r.wire_mib_s_60, residency(r.residency_before),
        residency(r.residency_after), r.checksum,
        i + 1 == w2_results.size() ? "" : ",");
  }
  out += "  ],\n  \"w2_walls\": [\n";
  for (std::size_t i = 0; i < w2_walls.size(); ++i) {
    const auto &wall = w2_walls[i];
    out += std::format(
        "    {{\"path\":\"{}\","
        "\"dirty_pixels\":{{\"w\":{},\"h\":{}}},"
        "\"budget_ms\":{:.1f},\"stroke_hz\":{},"
        "\"largest_passing\":{},\"first_failing\":{}}}{}\n",
        json_escape(wall.path), wall.dirty_w, wall.dirty_h, wall.budget_ms,
        wall.stroke_hz, optional_extent(wall.largest_passing),
        optional_extent(wall.first_failing),
        i + 1 == w2_walls.size() ? "" : ",");
  }

  out += "  ],\n  \"w4_results\": [\n";
  for (std::size_t i = 0; i < w4_results.size(); ++i) {
    const auto& r = w4_results[i];
    out += std::format(
        "    {{\"mode\":\"{}\",\"region_count\":{},"
        "\"region_cells\":{{\"w\":{},\"h\":{}}},\"samples\":{},"
        "\"timing_ms\":{{\"tick\":{},\"application_render\":{},"
        "\"framework_submission\":{},\"sink_write\":{},"
        "\"frame_work\":{}}},"
        "\"frame_bytes\":{{\"selection\":\"median_total\","
        "\"cells\":{},\"image_transmit\":{},"
        "\"image_edit\":{},\"total\":{}}},"
        "\"residency\":{{\"region_images\":{},\"pinned_images\":{},"
        "\"source_payload_bytes\":{}}},\"checksum\":\"{:016x}\"}}{}\n",
        json_escape(r.mode), r.region_count, r.cell_w, r.cell_h, r.samples,
        phase(r.tick), phase(r.application_render),
        phase(r.framework_submission), phase(r.sink_write),
        phase(r.frame_work), r.bytes.cells, r.bytes.image_transmit,
        r.bytes.image_edit, r.bytes.total(), r.residency.region_images,
        r.residency.pinned_images, r.residency.source_payload_bytes,
        r.checksum, i + 1 == w4_results.size() ? "" : ",");
  }
  out += "  ],\n  \"w4_walls\": [\n";
  for (std::size_t i = 0; i < w4_walls.size(); ++i) {
    const auto& wall = w4_walls[i];
    const auto optional_int = [](std::optional<int> value) {
      return value ? std::to_string(*value) : std::string{"null"};
    };
    out += std::format(
        "    {{\"mode\":\"{}\",\"region_cells\":{{\"w\":{},"
        "\"h\":{}}},\"first_retransmit_count\":{},\"budgets\":[",
        json_escape(wall.mode), wall.cell_w, wall.cell_h,
        optional_int(wall.first_retransmit_count));
    for (std::size_t b = 0; b < wall.budgets.size(); ++b) {
      const auto& budget = wall.budgets[b];
      out += std::format(
          "{{\"budget_ms\":{:.1f},\"largest_passing_count\":{},"
          "\"first_failing_count\":{}}}{}",
          budget.budget_ms, optional_int(budget.largest_passing_count),
          optional_int(budget.first_failing_count),
          b + 1 == wall.budgets.size() ? "" : ",");
    }
    out += std::format("]}}{}\n", i + 1 == w4_walls.size() ? "" : ",");
  }
  out += "  ]\n}\n";
  return out;
}

[[nodiscard]] auto table_report(const Options& options,
                                const std::vector<Result>& results,
                                const std::vector<Wall>& walls,
                                const std::vector<W2Result> &w2_results,
                                const std::vector<W2Wall> &w2_walls,
                                const std::vector<W4Result>& w4_results,
                                const std::vector<W4Wall>& w4_walls)
    -> std::string {
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
  if (!w2_results.empty()) {
    out += "\nW2 motion-to-paint steady state\n";
    out += std::format(
        "{:<9} {:>11} {:>9} {:>10} {:>9} {:>9} {:>11} {:>10} {:>10}\n", "path",
        "canvas", "dirty", "input ms", "app ms", "fw ms", "wire bytes",
        "MiB/s@30", "MiB/s@60");
    for (const auto &r : w2_results) {
      out += std::format(
          "{:<9} {:>4}x{:<6} {:>3}x{:<5} {:>10.4f} {:>9.4f} {:>9.4f} "
          "{:>11} {:>10.3f} {:>10.3f}\n",
          r.path, r.canvas_w, r.canvas_h, r.dirty_w, r.dirty_h,
          r.input_to_write.median_ms, r.application_render.median_ms,
          r.framework_submission.median_ms, r.bytes.total(), r.wire_mib_s_30,
          r.wire_mib_s_60);
    }
  }
  if (!w2_walls.empty()) {
    out += "\nW2 walls\n";
    const auto extent_name = [](std::optional<Extent> value) {
      return value ? std::format("{}x{}", value->w, value->h)
                   : std::string{"none"};
    };
    for (const auto &wall : w2_walls) {
      out += std::format(
          "  {:<7} dirty {:>3}x{:<3} @ {:>2} Hz/{:>4.1f} ms: pass {}, "
          "fail {}\n",
          wall.path, wall.dirty_w, wall.dirty_h, wall.stroke_hz, wall.budget_ms,
          extent_name(wall.largest_passing),
          wall.first_failing ? extent_name(wall.first_failing) : "not reached");
    }
  }
  if (!w4_results.empty()) {
    out += "\nW4 many-region steady state\n";
    out += std::format(
        "{:<11} {:>7} {:>7} {:>9} {:>9} {:>9} {:>9} {:>10} {:>12} {:>10}\n",
        "mode", "regions", "cells", "app ms", "fw ms", "sink ms",
        "frame ms", "median tx", "resident", "payload");
    for (const auto& r : w4_results) {
      out += std::format(
          "{:<11} {:>7} {:>3}x{:<3} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f} "
          "{:>10} {:>5}+{:<5} {:>10}\n",
          r.mode, r.region_count, r.cell_w, r.cell_h,
          r.application_render.median_ms, r.framework_submission.median_ms,
          r.sink_write.median_ms, r.frame_work.median_ms,
          r.bytes.image_transmit, r.residency.region_images,
          r.residency.pinned_images, r.residency.source_payload_bytes);
    }
  }
  if (!w4_walls.empty()) {
    out += "\nW4 walls\n";
    for (const auto& wall : w4_walls) {
      out += std::format("  {:<10} {:>2}x{:<2}: retransmit {}",
                         wall.mode, wall.cell_w, wall.cell_h,
                         wall.first_retransmit_count
                             ? std::to_string(*wall.first_retransmit_count)
                             : "not reached");
      for (const auto& budget : wall.budgets) {
        out += std::format(", {:>4.1f} ms pass {} fail {}", budget.budget_ms,
                           budget.largest_passing_count
                               ? std::to_string(*budget.largest_passing_count)
                               : "none",
                           budget.first_failing_count
                               ? std::to_string(*budget.first_failing_count)
                               : "not reached");
      }
      out += '\n';
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
    std::vector<W2Result> w2_results;
    if (options.suite == Suite::W2 || options.suite == Suite::All)
      w2_results = run_w2(options);
    std::vector<W4Result> w4_results;
    if (options.suite == Suite::W4 || options.suite == Suite::All)
      w4_results = run_w4(options);
    const auto walls = derive_walls(results);
    const auto w2_walls = derive_w2_walls(w2_results);
    const auto w4_walls = derive_w4_walls(w4_results);
    const std::string report = options.format == Format::Json
                                   ? json_report(options, results, walls,
                                                 w2_results, w2_walls,
                                                 w4_results, w4_walls)
                                   : table_report(options, results, walls,
                                                  w2_results, w2_walls,
                                                  w4_results, w4_walls);
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
