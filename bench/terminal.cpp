// TermForge W5 terminal-throughput benchmark (#88).
//
// Unlike main.cpp's O(1)-sink measurements, this program runs inside the
// terminal under test. Each batch ends at an ordered terminal reply, so the
// result includes the pty handoff and terminal parser/graphics work rather
// than merely filling the kernel's output buffer.

#include <algorithm>
#include <array>
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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "termforge/core/byte_sink.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view kGraphicsFence =
    "\033_Gi=424242,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";
constexpr std::string_view kGraphicsOk = "\033_Gi=424242;OK\033\\";
constexpr std::string_view kGraphicsPrefix = "\033_Gi=424242;";
constexpr std::string_view kDa1Fence = "\033[c";

enum class Route { Auto, Kitty, Ansi };
enum class Format { Table, Json };
enum class Fence { Graphics, Da1 };

struct Options {
  Route route{Route::Auto};
  Format format{Format::Table};
  std::optional<std::filesystem::path> output;
  std::string terminal{"unknown"};
  std::string terminal_version{"unknown"};
  int samples{5};
  int warmup{1};
  int timeout_ms{5000};
  std::size_t batch_bytes{std::size_t{16} * 1024U * 1024U};
  bool smoke{false};
};

struct PhaseStats {
  double median_ms{};
  double p95_ms{};
};

struct Result {
  std::string route;
  std::string image_format;
  int pixels_w{};
  int pixels_h{};
  int cols{};
  int rows{};
  int frames_per_batch{};
  int samples{};
  FrameBytes bytes;
  PhaseStats assembly;
  PhaseStats sink_write;
  PhaseStats fence_wait;
  PhaseStats end_to_reply;
  double throughput_mib_s{};
  double maximum_fps{};
  bool meets_30hz{};
  bool meets_60hz{};
  std::uint64_t checksum{};
};

struct Wall {
  std::string route;
  std::string image_format;
  double budget_ms{};
  std::optional<std::string> largest_passing;
  std::optional<std::string> first_failing;
};

[[nodiscard]] auto parse_positive(std::string_view value, std::string_view name)
    -> int {
  int out = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size() || out <= 0)
    throw std::runtime_error{
        std::format("{} must be a positive integer", name)};
  return out;
}

[[nodiscard]] auto parse_size(std::string_view value, std::string_view name)
    -> std::size_t {
  std::uint64_t out = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size() || out == 0 ||
      out > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error{
        std::format("{} must be a positive byte count", name)};
  }
  return static_cast<std::size_t>(out);
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
    if (arg == "--route") {
      const auto v = value(arg);
      if (v == "auto")
        out.route = Route::Auto;
      else if (v == "kitty")
        out.route = Route::Kitty;
      else if (v == "ansi")
        out.route = Route::Ansi;
      else
        throw std::runtime_error{"--route must be auto, kitty, or ansi"};
    } else if (arg == "--format") {
      const auto v = value(arg);
      if (v == "table")
        out.format = Format::Table;
      else if (v == "json")
        out.format = Format::Json;
      else
        throw std::runtime_error{"--format must be table or json"};
    } else if (arg == "--output") {
      out.output = std::filesystem::path{value(arg)};
    } else if (arg == "--terminal") {
      out.terminal = value(arg);
    } else if (arg == "--terminal-version") {
      out.terminal_version = value(arg);
    } else if (arg == "--samples") {
      out.samples = parse_positive(value(arg), arg);
    } else if (arg == "--warmup") {
      out.warmup = parse_positive(value(arg), arg);
    } else if (arg == "--timeout-ms") {
      out.timeout_ms = parse_positive(value(arg), arg);
    } else if (arg == "--batch-bytes") {
      out.batch_bytes = parse_size(value(arg), arg);
    } else if (arg == "--smoke") {
      out.smoke = true;
      out.samples = 1;
      out.warmup = 1;
      out.batch_bytes = 1;
    } else if (arg == "--help") {
      std::cout << "Usage: termforge_terminal_bench [--route auto|kitty|ansi] "
                   "[--format table|json] [--output PATH] [--terminal NAME] "
                   "[--terminal-version VERSION] [--samples N] [--warmup N] "
                   "[--timeout-ms N] [--batch-bytes N] [--smoke]\n";
      std::exit(0);
    } else {
      throw std::runtime_error{std::format("unknown argument: {}", arg)};
    }
  }
  return out;
}

[[nodiscard]] auto percentile(std::vector<double> values, double p) -> double {
  std::sort(values.begin(), values.end());
  const auto index =
      std::min(values.size() - 1, static_cast<std::size_t>(std::ceil(
                                      static_cast<double>(values.size()) * p)) -
                                      1);
  return values[index];
}

[[nodiscard]] auto stats(const std::vector<double>& values) -> PhaseStats {
  return PhaseStats{percentile(values, 0.5), percentile(values, 0.95)};
}

[[nodiscard]] auto make_image(int width, int height, std::uint32_t seed)
    -> Image {
  const auto count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<Pixel> pixels(count);
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = state * 1664525U + 1013904223U;
    pixels[i] = Pixel{static_cast<std::uint8_t>(state >> 24),
                      static_cast<std::uint8_t>(state >> 16),
                      static_cast<std::uint8_t>(state >> 8), 255};
  }
  return Image{width, height, std::move(pixels)};
}

[[nodiscard]] auto packed_rgb(const Image& image) -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  bytes.reserve(image.pixels().size() * 3U);
  for (const Pixel pixel : image.pixels()) {
    bytes.push_back(static_cast<std::byte>(pixel.r));
    bytes.push_back(static_cast<std::byte>(pixel.g));
    bytes.push_back(static_cast<std::byte>(pixel.b));
  }
  return bytes;
}

[[nodiscard]] auto frame_total(FrameBytes bytes) -> std::uint64_t {
  return bytes.cells + bytes.image_transmit + bytes.image_edit;
}

auto add_bytes(FrameBytes& into, FrameBytes add) -> void {
  into.cells += add.cells;
  into.image_transmit += add.image_transmit;
  into.image_edit += add.image_edit;
}

[[nodiscard]] auto checksum_bytes(std::span<const char> bytes)
    -> std::uint64_t {
  std::uint64_t hash = bytes.size();
  if (!bytes.empty()) {
    hash ^=
        static_cast<std::uint64_t>(static_cast<unsigned char>(bytes.front()))
        << 8;
    hash ^= static_cast<std::uint64_t>(
                static_cast<unsigned char>(bytes[bytes.size() / 2]))
            << 16;
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes.back()))
            << 24;
  }
  return hash;
}

class MeasuredSink final : public ByteSink {
 public:
  explicit MeasuredSink(int fd) : m_fd(fd) {}

  auto reset_batch() noexcept -> void {
    m_write_time = {};
    m_bytes = 0;
    m_checksum = kChecksumSeed;
  }
  [[nodiscard]] auto write_time() const noexcept -> std::chrono::nanoseconds {
    return m_write_time;
  }
  [[nodiscard]] auto bytes() const noexcept -> std::uint64_t { return m_bytes; }
  [[nodiscard]] auto checksum() const noexcept -> std::uint64_t {
    return m_checksum;
  }

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    const auto started = Clock::now();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto n =
          ::write(m_fd, bytes.data() + offset, bytes.size() - offset);
      if (n > 0) {
        offset += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      return std::unexpected{
          ErrorEvent{Severity::Error, "terminal-benchmark",
                     std::string{"write: "} + std::strerror(errno)}};
    }
    m_write_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - started);
    m_bytes += bytes.size();
    const auto value = checksum_bytes(bytes);
    m_checksum ^=
        value + 0x9E3779B97F4A7C15ULL + (m_checksum << 6) + (m_checksum >> 2);
    return {};
  }

 private:
  int m_fd;
  std::chrono::nanoseconds m_write_time{};
  std::uint64_t m_bytes{};
  static constexpr std::uint64_t kChecksumSeed = 14695981039346656037ULL;
  std::uint64_t m_checksum{kChecksumSeed};
};

class CountingSink final : public ByteSink {
 public:
  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    m_bytes += bytes.size();
    const auto value = checksum_bytes(bytes);
    m_checksum ^=
        value + 0x9E3779B97F4A7C15ULL + (m_checksum << 6) + (m_checksum >> 2);
    return {};
  }
  [[nodiscard]] auto checksum() const noexcept -> std::uint64_t {
    return m_checksum;
  }
  [[nodiscard]] auto bytes() const noexcept -> std::uint64_t { return m_bytes; }

 private:
  std::uint64_t m_bytes{};
  std::uint64_t m_checksum{14695981039346656037ULL};
};

[[nodiscard]] auto da1_complete(std::string_view input) -> bool {
  for (std::size_t i = 0; i + 3 < input.size(); ++i) {
    if (input[i] != '\033' || input[i + 1] != '[' || input[i + 2] != '?')
      continue;
    std::size_t p = i + 3;
    bool digit = false;
    while (p < input.size() &&
           ((input[p] >= '0' && input[p] <= '9') || input[p] == ';')) {
      digit = digit || (input[p] >= '0' && input[p] <= '9');
      ++p;
    }
    if (digit && p < input.size() && input[p] == 'c') return true;
  }
  return false;
}

[[nodiscard]] auto fence_status(std::string_view input, Fence fence)
    -> std::optional<bool> {
  if (fence == Fence::Da1)
    return da1_complete(input) ? std::optional{true} : std::nullopt;
  if (input.find(kGraphicsOk) != std::string_view::npos) return true;
  const auto prefix = input.find(kGraphicsPrefix);
  if (prefix == std::string_view::npos) return std::nullopt;
  const auto end = input.find("\033\\", prefix + kGraphicsPrefix.size());
  if (end == std::string_view::npos) return std::nullopt;
  return false;
}

auto write_all(int fd, std::string_view bytes) -> void {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    throw std::runtime_error{std::string{"fence write: "} +
                             std::strerror(errno)};
  }
}

[[nodiscard]] auto wait_for_fence(Terminal& terminal, Fence fence,
                                  int timeout_ms) -> double {
  write_all(terminal.io().out,
            fence == Fence::Graphics ? kGraphicsFence : kDa1Fence);
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::milliseconds{timeout_ms};
  std::string reply;
  std::array<char, 1024> buffer{};
  while (Clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (!terminal.wait_readable(std::max(1, static_cast<int>(left.count()))))
      continue;
    const int n =
        terminal.read_input(buffer.data(), static_cast<int>(buffer.size()));
    if (n <= 0) continue;
    reply.append(buffer.data(), static_cast<std::size_t>(n));
    if (const auto status = fence_status(reply, fence); status.has_value()) {
      if (!*status)
        throw std::runtime_error{
            "terminal rejected the ordered graphics fence"};
      return std::chrono::duration<double, std::milli>(Clock::now() - started)
          .count();
    }
    if (reply.size() > std::size_t{16} * 1024U)
      reply.erase(0, reply.size() - std::size_t{4096});
  }
  throw std::runtime_error{
      "terminal did not answer the ordered fence before timeout"};
}

auto drain_driver(TerminalDriver& driver) -> void {
  if (const auto error = driver.take_output_error())
    throw std::runtime_error{error->message};
  auto events = driver.take_driver_events();
  for (const auto& event : events) {
    if (event.severity != Severity::Info)
      throw std::runtime_error{event.message};
  }
}

[[nodiscard]] auto batch_frames(const Options& options,
                                std::uint64_t approximate_bytes) -> int {
  const auto safe = std::max<std::uint64_t>(1, approximate_bytes);
  const auto count =
      (static_cast<std::uint64_t>(options.batch_bytes) + safe - 1) / safe;
  return static_cast<int>(std::clamp<std::uint64_t>(count, 2, 512));
}

template <typename Draw>
[[nodiscard]] auto measure_live_case(const Options& options, std::string route,
                                     std::string image_format, Extent pixels,
                                     Rect cells, TerminalDriver& driver,
                                     MeasuredSink& sink, Terminal& terminal,
                                     Fence fence, Draw&& draw) -> Result {
  // One untimed frame establishes the exact driver wire size from the same
  // call sequence the measured batches use.
  sink.reset_batch();
  const auto primed = draw(0);
  if (!primed) throw std::runtime_error{primed.error().message};
  driver.flush();
  drain_driver(driver);
  const auto approximate = frame_total(driver.last_frame_bytes());
  static_cast<void>(wait_for_fence(terminal, fence, options.timeout_ms));
  const int frames = batch_frames(options, approximate);

  std::vector<double> assembly_samples;
  std::vector<double> write_samples;
  std::vector<double> fence_samples;
  std::vector<double> total_samples;
  std::vector<double> throughput_samples;
  FrameBytes representative{};
  std::uint64_t representative_checksum{};

  const int batches = options.warmup + options.samples;
  for (int sample = 0; sample < batches; ++sample) {
    sink.reset_batch();
    FrameBytes bytes{};
    double outer_ms = 0.0;
    const auto batch_started = Clock::now();
    for (int frame = 0; frame < frames; ++frame) {
      const auto started = Clock::now();
      const auto result = draw(frame + sample * frames + 1);
      if (!result) throw std::runtime_error{result.error().message};
      driver.flush();
      outer_ms +=
          std::chrono::duration<double, std::milli>(Clock::now() - started)
              .count();
      drain_driver(driver);
      add_bytes(bytes, driver.last_frame_bytes());
    }
    const double fence_ms = wait_for_fence(terminal, fence, options.timeout_ms);
    const double total_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - batch_started)
            .count();
    const double write_ms =
        std::chrono::duration<double, std::milli>(sink.write_time()).count();
    if (sink.bytes() != frame_total(bytes))
      throw std::runtime_error{"driver meter disagrees with bytes written"};
    if (sample < options.warmup) continue;
    const double divisor = frames;
    assembly_samples.push_back(std::max(0.0, outer_ms - write_ms) / divisor);
    write_samples.push_back(write_ms / divisor);
    fence_samples.push_back(fence_ms);
    total_samples.push_back(total_ms / divisor);
    throughput_samples.push_back(static_cast<double>(frame_total(bytes)) /
                                 (1024.0 * 1024.0) / (total_ms / 1000.0));
    representative =
        FrameBytes{bytes.cells / static_cast<std::uint64_t>(frames),
                   bytes.image_transmit / static_cast<std::uint64_t>(frames),
                   bytes.image_edit / static_cast<std::uint64_t>(frames)};
    representative_checksum = sink.checksum();
  }

  const auto total = stats(total_samples);
  return Result{.route = std::move(route),
                .image_format = std::move(image_format),
                .pixels_w = pixels.w,
                .pixels_h = pixels.h,
                .cols = cells.w,
                .rows = cells.h,
                .frames_per_batch = frames,
                .samples = options.samples,
                .bytes = representative,
                .assembly = stats(assembly_samples),
                .sink_write = stats(write_samples),
                .fence_wait = stats(fence_samples),
                .end_to_reply = total,
                .throughput_mib_s = percentile(throughput_samples, 0.5),
                .maximum_fps =
                    total.median_ms > 0 ? 1000.0 / total.median_ms : 0,
                .meets_30hz = total.median_ms <= 1000.0 / 30.0,
                .meets_60hz = total.median_ms <= 1000.0 / 60.0,
                .checksum = representative_checksum};
}

[[nodiscard]] auto run_live_kitty(const Options& options, Terminal& terminal,
                                  const Capabilities& caps, Rect placement)
    -> std::vector<Result> {
  auto driver = terminal.select_driver(caps, BuiltinDriver::Kitty);
  if (auto initialized = driver->init(); !initialized)
    throw std::runtime_error{initialized.error().message};
  MeasuredSink sink{terminal.io().out};
  driver->set_output(&sink);
  std::vector<Result> results;
  constexpr std::array extents{Extent{320, 180}, Extent{640, 360},
                               Extent{1280, 720}, Extent{1920, 1080}};
  for (const auto extent : extents) {
    auto first = make_image(extent.w, extent.h, 0x12345678U);
    auto second = make_image(extent.w, extent.h, 0xA5A5A5A5U);
    const auto first_rgb = packed_rgb(first);
    const auto second_rgb = packed_rgb(second);
    // W5 needs one route that every selected Kitty-protocol implementation
    // actually decodes. Mutable-root a=f,r=1 is a Kitty extension that broad
    // KGP implementations such as Ghostty may reject; draw_image retransmits
    // under the region's stable id with the baseline a=t action instead.
    auto draw_rgba = [&](int frame) -> std::expected<void, ErrorEvent> {
      const auto& image = frame % 2 == 0 ? second : first;
      return driver->draw_image(
          placement, EncodedImage{ImageFormat::Rgba32,
                                  std::as_bytes(image.pixels()), extent});
    };
    results.push_back(measure_live_case(
        options, "kitty-full-transmit", "rgba32", extent, placement, *driver,
        sink, terminal, Fence::Graphics, draw_rgba));
    auto draw_rgb = [&](int frame) -> std::expected<void, ErrorEvent> {
      const auto& bytes = frame % 2 == 0 ? second_rgb : first_rgb;
      return driver->draw_image(
          placement, EncodedImage{ImageFormat::Rgb24, bytes, extent});
    };
    results.push_back(measure_live_case(options, "kitty-full-transmit", "rgb24",
                                        extent, placement, *driver, sink,
                                        terminal, Fence::Graphics, draw_rgb));
  }
  driver->shutdown();
  drain_driver(*driver);
  return results;
}

[[nodiscard]] auto run_live_ansi(const Options& options, Terminal& terminal,
                                 const Capabilities& caps,
                                 Extent terminal_cells) -> std::vector<Result> {
  auto driver = terminal.select_driver(caps, BuiltinDriver::AnsiRgb);
  if (auto initialized = driver->init(); !initialized)
    throw std::runtime_error{initialized.error().message};
  MeasuredSink sink{terminal.io().out};
  driver->set_output(&sink);
  std::vector<Result> results;
  constexpr std::array grids{Extent{80, 24}, Extent{120, 40}, Extent{200, 50},
                             Extent{300, 80}, Extent{400, 120}};
  for (const auto grid : grids) {
    if (grid.w > terminal_cells.w || grid.h > terminal_cells.h) continue;
    const Rect cells{0, 0, grid.w, grid.h};
    const Extent pixels{grid.w, grid.h * 2};
    auto first = make_image(pixels.w, pixels.h, 0x12345678U);
    auto second = make_image(pixels.w, pixels.h, 0xA5A5A5A5U);
    auto draw = [&](int frame) -> std::expected<void, ErrorEvent> {
      return driver->draw_image(cells, frame % 2 == 0 ? second : first,
                                PlacementFit::Exact);
    };
    results.push_back(measure_live_case(options, "ansi-half-block", "rgba32",
                                        pixels, cells, *driver, sink, terminal,
                                        Fence::Da1, draw));
  }
  if (results.empty())
    throw std::runtime_error{"terminal is smaller than the 80x24 ANSI floor"};
  driver->shutdown();
  drain_driver(*driver);
  return results;
}

[[nodiscard]] auto smoke_result_kitty(ImageFormat format) -> Result {
  KittyDriver driver;
  CountingSink sink;
  driver.set_output(&sink);
  auto image = make_image(8, 4, 1);
  const auto rgb = packed_rgb(image);
  const EncodedImage payload{format,
                             format == ImageFormat::Rgb24
                                 ? std::span<const std::byte>{rgb}
                                 : std::as_bytes(image.pixels()),
                             Extent{8, 4}};
  if (const auto drawn = driver.draw_image(Rect{0, 0, 4, 2}, payload); !drawn)
    throw std::runtime_error{drawn.error().message};
  driver.flush();
  const auto bytes = driver.last_frame_bytes();
  if (sink.bytes() != frame_total(bytes))
    throw std::runtime_error{"Kitty smoke meter disagrees with its sink"};
  const auto checksum = sink.checksum();
  driver.shutdown();
  return Result{.route = "kitty-full-transmit",
                .image_format =
                    format == ImageFormat::Rgb24 ? "rgb24" : "rgba32",
                .pixels_w = 8,
                .pixels_h = 4,
                .cols = 4,
                .rows = 2,
                .frames_per_batch = 1,
                .samples = 1,
                .bytes = bytes,
                .assembly = {},
                .sink_write = {},
                .fence_wait = {},
                .end_to_reply = {},
                .throughput_mib_s = 0,
                .maximum_fps = 0,
                .meets_30hz = true,
                .meets_60hz = true,
                .checksum = checksum};
}

[[nodiscard]] auto smoke_result_ansi() -> Result {
  AnsiRgbDriver driver;
  CountingSink sink;
  driver.set_output(&sink);
  auto image = make_image(4, 4, 3);
  if (const auto drawn =
          driver.draw_image(Rect{0, 0, 4, 2}, image, PlacementFit::Exact);
      !drawn) {
    throw std::runtime_error{drawn.error().message};
  }
  driver.flush();
  const auto bytes = driver.last_frame_bytes();
  if (sink.bytes() != frame_total(bytes))
    throw std::runtime_error{"ANSI smoke meter disagrees with its sink"};
  const auto checksum = sink.checksum();
  driver.shutdown();
  return Result{.route = "ansi-half-block",
                .image_format = "rgba32",
                .pixels_w = 4,
                .pixels_h = 4,
                .cols = 4,
                .rows = 2,
                .frames_per_batch = 1,
                .samples = 1,
                .bytes = bytes,
                .assembly = {},
                .sink_write = {},
                .fence_wait = {},
                .end_to_reply = {},
                .throughput_mib_s = 0,
                .maximum_fps = 0,
                .meets_30hz = true,
                .meets_60hz = true,
                .checksum = checksum};
}

auto verify_reply_parser() -> void {
  std::string fragmented{"noise\033_Gi=7;OK\033\\"};
  if (fence_status(fragmented, Fence::Graphics).has_value())
    throw std::runtime_error{"graphics fence accepted the wrong id"};
  fragmented += "\033_Gi=424242;O";
  if (fence_status(fragmented, Fence::Graphics).has_value())
    throw std::runtime_error{"graphics fence accepted a truncated reply"};
  fragmented += "K\033\\";
  if (fence_status(fragmented, Fence::Graphics) != std::optional{true})
    throw std::runtime_error{"graphics fence rejected its fragmented reply"};
  if (fence_status("\033_Gi=424242;EINVAL\033\\", Fence::Graphics) !=
      std::optional{false})
    throw std::runtime_error{"graphics fence ignored terminal rejection"};
  if (fence_status("\033[?62;4", Fence::Da1).has_value() ||
      fence_status("\033[?62;4;22c", Fence::Da1) != std::optional{true})
    throw std::runtime_error{"DA1 fence lost its truncation boundary"};
}

[[nodiscard]] auto case_name(const Result& result) -> std::string {
  if (result.route == "kitty-full-transmit")
    return std::format("{}x{}px", result.pixels_w, result.pixels_h);
  return std::format("{}x{}cells", result.cols, result.rows);
}

[[nodiscard]] auto derive_walls(const std::vector<Result>& results)
    -> std::vector<Wall> {
  std::vector<Wall> walls;
  constexpr std::array routes{std::pair{std::string_view{"kitty-full-transmit"},
                                        std::string_view{"rgba32"}},
                              std::pair{std::string_view{"kitty-full-transmit"},
                                        std::string_view{"rgb24"}},
                              std::pair{std::string_view{"ansi-half-block"},
                                        std::string_view{"rgba32"}}};
  for (const auto& [route, image_format] : routes) {
    for (const double budget : {1000.0 / 60.0, 1000.0 / 30.0}) {
      Wall wall{std::string{route}, std::string{image_format}, budget,
                std::nullopt, std::nullopt};
      for (const auto& result : results) {
        if (result.route != route || result.image_format != image_format)
          continue;
        if (result.end_to_reply.median_ms <= budget && !wall.first_failing)
          wall.largest_passing = case_name(result);
        else if (!wall.first_failing)
          wall.first_failing = case_name(result);
      }
      if (wall.largest_passing || wall.first_failing) walls.push_back(wall);
    }
  }
  return walls;
}

[[nodiscard]] auto json_escape(std::string_view value) -> std::string {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20)
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
          out += static_cast<char>(c);
    }
  }
  return out;
}

[[nodiscard]] auto compiler_name() -> std::string {
#if defined(__clang__)
  return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__,
                     __clang_patchlevel__);
#elif defined(__GNUC__)
  return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__,
                     __GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

[[nodiscard]] auto host_name() -> std::string {
  utsname info{};
  if (::uname(&info) != 0) return "unknown";
  return std::format("{} {} {}", info.sysname, info.release, info.machine);
}

[[nodiscard]] auto json_report(const Options& options,
                               const std::vector<Result>& results,
                               const std::vector<Wall>& walls,
                               Extent terminal_cells) -> std::string {
  const auto env = [](const char* name) -> std::string_view {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
  };
  std::string out = std::format(
      "{{\n  \"schema_version\":2,\n  \"termforge_version\":\"{}\",\n"
      "  \"live\":{},\n  \"compiler\":\"{}\",\n  \"host\":\"{}\",\n"
      "  \"terminal\":{{\"name\":\"{}\",\"version\":\"{}\","
      "\"term\":\"{}\",\"colorterm\":\"{}\",\"cols\":{},\"rows\":{},"
      "\"tmux\":{},\"ssh\":{}}},\n  \"samples\":{},\n  \"warmup\":{},\n"
      "  \"batch_target_bytes\":{},\n  \"results\":[\n",
      TERMFORGE_BENCH_VERSION, options.smoke ? "false" : "true",
      json_escape(compiler_name()), json_escape(host_name()),
      json_escape(options.terminal), json_escape(options.terminal_version),
      json_escape(env("TERM")), json_escape(env("COLORTERM")), terminal_cells.w,
      terminal_cells.h, env("TMUX").empty() ? "false" : "true",
      (env("SSH_CONNECTION").empty() && env("SSH_TTY").empty()) ? "false"
                                                                : "true",
      options.samples, options.warmup, options.batch_bytes);
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    out += std::format(
        "    {{\"route\":\"{}\",\"image_format\":\"{}\","
        "\"pixels\":{{\"w\":{},\"h\":{}}},"
        "\"cells\":{{\"w\":{},\"h\":{}}},\"frames_per_batch\":{},"
        "\"samples\":{},\"frame_bytes\":{{\"cells\":{},"
        "\"image_transmit\":{},\"image_edit\":{},\"total\":{}}},"
        "\"timing_ms\":{{\"assembly\":{{\"median\":{:.6f},\"p95\":{:.6f}}},"
        "\"sink_write\":{{\"median\":{:.6f},\"p95\":{:.6f}}},"
        "\"fence_wait_batch\":{{\"median\":{:.6f},\"p95\":{:.6f}}},"
        "\"end_to_reply_per_frame\":{{\"median\":{:.6f},\"p95\":{:.6f}}}}},"
        "\"throughput_mib_s\":{:.6f},\"maximum_fps\":{:.6f},"
        "\"meets_30hz\":{},\"meets_60hz\":{},\"checksum\":\"{:016x}\"}}{}\n",
        result.route, result.image_format, result.pixels_w, result.pixels_h,
        result.cols, result.rows, result.frames_per_batch, result.samples,
        result.bytes.cells, result.bytes.image_transmit,
        result.bytes.image_edit, frame_total(result.bytes),
        result.assembly.median_ms, result.assembly.p95_ms,
        result.sink_write.median_ms, result.sink_write.p95_ms,
        result.fence_wait.median_ms, result.fence_wait.p95_ms,
        result.end_to_reply.median_ms, result.end_to_reply.p95_ms,
        result.throughput_mib_s, result.maximum_fps,
        result.meets_30hz ? "true" : "false",
        result.meets_60hz ? "true" : "false", result.checksum,
        i + 1 == results.size() ? "" : ",");
  }
  out += "  ],\n  \"walls\":[\n";
  for (std::size_t i = 0; i < walls.size(); ++i) {
    const auto& wall = walls[i];
    const auto optional_json = [](const std::optional<std::string>& value) {
      return value ? std::format("\"{}\"", json_escape(*value)) : "null";
    };
    out += std::format("    {{\"route\":\"{}\",\"image_format\":\"{}\","
                       "\"budget_ms\":{:.6f},"
                       "\"largest_passing\":{},\"first_failing\":{}}}{}\n",
                       wall.route, wall.image_format, wall.budget_ms,
                       optional_json(wall.largest_passing),
                       optional_json(wall.first_failing),
                       i + 1 == walls.size() ? "" : ",");
  }
  out += "  ]\n}\n";
  return out;
}

[[nodiscard]] auto table_report(const Options& options,
                                const std::vector<Result>& results,
                                const std::vector<Wall>& walls,
                                Extent terminal_cells) -> std::string {
  std::string out =
      std::format("TermForge W5 terminal throughput\nterminal: {} {} ({}x{})\n"
                  "route/format/case          bytes/frame  total ms   p95 ms   "
                  "MiB/s    FPS  "
                  "30/60\n",
                  options.terminal, options.terminal_version, terminal_cells.w,
                  terminal_cells.h);
  for (const auto& result : results) {
    out += std::format(
        "{:<27} {:>11} {:>9.3f} {:>9.3f} {:>7.2f} {:>6.1f}  {}/{}\n",
        result.route + "/" + result.image_format + "/" + case_name(result),
        frame_total(result.bytes), result.end_to_reply.median_ms,
        result.end_to_reply.p95_ms, result.throughput_mib_s, result.maximum_fps,
        result.meets_30hz ? "Y" : "N", result.meets_60hz ? "Y" : "N");
  }
  out += "walls:\n";
  for (const auto& wall : walls) {
    out += std::format("  {}/{} {:.1f}ms: pass {}, fail {}\n", wall.route,
                       wall.image_format, wall.budget_ms,
                       wall.largest_passing.value_or("none"),
                       wall.first_failing.value_or("not reached"));
  }
  return out;
}

class ScreenGuard {
 public:
  explicit ScreenGuard(Terminal& terminal) : m_terminal(terminal) {
    m_terminal.enter_screen();
  }
  ~ScreenGuard() { m_terminal.leave_screen(); }
  ScreenGuard(const ScreenGuard&) = delete;
  auto operator=(const ScreenGuard&) -> ScreenGuard& = delete;

 private:
  Terminal& m_terminal;
};

[[nodiscard]] auto terminal_extent(TerminalIo io) -> Extent {
  winsize size{};
  if (io.out >= 0 && ::ioctl(io.out, TIOCGWINSZ, &size) == 0 &&
      size.ws_col > 0 && size.ws_row > 0) {
    return Extent{size.ws_col, size.ws_row};
  }
  return Extent{80, 24};
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    std::vector<Result> results;
    Extent cells{80, 24};
    if (options.smoke) {
      verify_reply_parser();
      if (options.route != Route::Ansi) {
        results.push_back(smoke_result_kitty(ImageFormat::Rgba32));
        results.push_back(smoke_result_kitty(ImageFormat::Rgb24));
      }
      if (options.route != Route::Kitty) results.push_back(smoke_result_ansi());
    } else {
      if (!options.output)
        throw std::runtime_error{"live runs require --output so reports cannot "
                                 "enter the measured tty"};
      Terminal terminal;
      if (auto raw = terminal.enter_raw(); !raw)
        throw std::runtime_error{raw.error().message};
      const auto caps = terminal.query_capabilities();
      if (!caps) throw std::runtime_error{caps.error().message};
      cells = terminal_extent(terminal.io());
      ScreenGuard screen{terminal};
      Route route = options.route;
      if (route == Route::Auto)
        route = caps->kitty_graphics ? Route::Kitty : Route::Ansi;
      if (route == Route::Kitty) {
        if (!caps->kitty_graphics)
          throw std::runtime_error{
              "forced Kitty route is unsupported by this terminal"};
        const Rect placement{0, 0, std::min(80, cells.w),
                             std::min(24, cells.h)};
        results = run_live_kitty(options, terminal, *caps, placement);
      } else {
        results = run_live_ansi(options, terminal, *caps, cells);
      }
    }
    const auto walls = derive_walls(results);
    const std::string report =
        options.format == Format::Json
            ? json_report(options, results, walls, cells)
            : table_report(options, results, walls, cells);
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
    std::cerr << "termforge_terminal_bench: " << error.what() << '\n';
    return 2;
  }
}
