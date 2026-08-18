// TermForge example: a deterministic 320x180 game-style software framebuffer.
//
// Default mode runs interactively at a requested 30 FPS. `--benchmark [N]`
// drives the exact App frame body headlessly for N frames (180 by default),
// and `--capture-seconds N --report PATH` runs against a real Kitty terminal
// long enough to record the empirical gate from #198. The workload deliberately
// leaves every 30th frame clean so zero-byte persistent submission is measured,
// not inferred.

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "termforge/core/app.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/pixel_surface.hpp"

using namespace termforge;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kWidth = 320;
constexpr int kHeight = 180;
constexpr int kHoldInterval = 30;
constexpr double kBudgetMs = 1000.0 / 30.0;

struct FrameSample {
  std::uint64_t bytes{};
  std::uint64_t image_bytes{};
  double tick_ms{};
  double generation_ms{};
  double submission_ms{};
  double frame_ms{};
  bool unchanged{};
};

struct CaptureSummary {
  int frames{};
  int unchanged_frames{};
  int missed_deadlines{};
  std::uint64_t wire_bytes{};
  std::uint64_t unchanged_image_bytes{};
  std::uint64_t shutdown_bytes{};
  int data_transmits{};
  int root_updates{};
  int placements{};
  int data_deletes{};
  int placement_deletes{};
  std::size_t unique_image_ids{};
  std::size_t peak_live_image_ids{};
  double elapsed_seconds{};
  double achieved_fps{};
  double bytes_per_frame{};
  double bytes_per_second{};
  double generation_avg_ms{};
  double generation_max_ms{};
  double submission_avg_ms{};
  double submission_max_ms{};
  double frame_avg_ms{};
  double frame_p95_ms{};
  double frame_max_ms{};
};

auto elapsed_ms(Clock::time_point from, Clock::time_point to) noexcept
    -> double {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

auto key_value(std::string_view keys, std::string_view wanted)
    -> std::string_view {
  const std::string needle = std::string{wanted} + "=";
  for (std::size_t at = 0;
       (at = keys.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    if (at != 0 && keys[at - 1] != ',') continue;
    const std::size_t from = at + needle.size();
    const std::size_t comma = keys.find(',', from);
    return keys.substr(from, comma == std::string_view::npos
                                 ? std::string_view::npos
                                 : comma - from);
  }
  return {};
}

auto parse_u32(std::string_view text) -> std::optional<std::uint32_t> {
  if (text.empty()) return std::nullopt;
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

// Timing no longer depends on this sink (#258): App's frame observer supplies
// the phase durations and byte meter. Capture/benchmark mode still needs to
// inspect the wire for lifecycle evidence (logical uploads, root edits,
// placements and deletes), because those historical JSON fields have no
// public counter. Interactive mode therefore uses the driver's ordinary
// output directly, while the two evidence modes install this audit-only sink.
class ProtocolAuditSink final : public ByteSink {
 public:
  auto set_fd(int fd) noexcept -> void { m_fd = fd; }
  auto set_collect(bool collect) noexcept -> void { m_collect = collect; }

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    if (m_fd >= 0) {
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count =
            ::write(m_fd, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
          offset += static_cast<std::size_t>(count);
          continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) {
          return std::unexpected{ErrorEvent{Severity::Warning, "game",
                                            "frame sink wrote zero bytes"}};
        }
        return std::unexpected{ErrorEvent{
            Severity::Warning, "game",
            std::format("frame sink write failed: {}", std::strerror(errno))}};
      }
    }
    if (!m_collect) return {};

    const std::string_view wire{bytes.data(), bytes.size()};
    const bool shutdown = wire.find("a=d,d=A") != std::string_view::npos;
    if (shutdown) {
      m_shutdown_bytes += bytes.size();
      m_live_ids.clear();
      return {};
    }
    parse_apcs(wire);
    return {};
  }

  [[nodiscard]] auto lifecycle_summary() const -> CaptureSummary {
    CaptureSummary out;
    out.shutdown_bytes = m_shutdown_bytes;
    out.data_transmits = m_data_transmits;
    out.root_updates = m_root_updates;
    out.placements = m_placements;
    out.data_deletes = m_data_deletes;
    out.placement_deletes = m_placement_deletes;
    out.unique_image_ids = m_all_ids.size();
    out.peak_live_image_ids = m_peak_live_ids;
    return out;
  }

 private:
  auto parse_apcs(std::string_view wire) -> void {
    for (std::size_t at = 0;
         (at = wire.find("\033_G", at)) != std::string_view::npos;) {
      const std::size_t body = at + 3;
      const std::size_t end = wire.find("\033\\", body);
      if (end == std::string_view::npos) return;
      const std::string_view seq = wire.substr(body, end - body);
      const std::size_t semi = seq.find(';');
      const std::string_view keys = seq.substr(0, semi);
      const std::string_view action = key_value(keys, "a");
      const std::string_view deletion = key_value(keys, "d");
      const auto id = parse_u32(key_value(keys, "i"));
      if (id) m_all_ids.insert(*id);

      if (action == "t") {
        ++m_data_transmits;
        if (id) m_live_ids.insert(*id);
      } else if (action == "f" && id) {
        // Every animation-frame continuation repeats a=f but only the opener
        // names i= (#259). Count logical updates, not protocol chunks.
        ++m_root_updates;
      } else if (action == "p") {
        ++m_placements;
      } else if (action == "d" && deletion == "I") {
        ++m_data_deletes;
        if (id) m_live_ids.erase(*id);
      } else if (action == "d" && deletion == "i") {
        ++m_placement_deletes;
      }
      m_peak_live_ids = std::max(m_peak_live_ids, m_live_ids.size());
      at = end + 2;
    }
  }

  int m_fd{-1};
  bool m_collect{false};
  std::set<std::uint32_t> m_all_ids;
  std::set<std::uint32_t> m_live_ids;
  std::size_t m_peak_live_ids{};
  std::uint64_t m_shutdown_bytes{};
  int m_data_transmits{};
  int m_root_updates{};
  int m_placements{};
  int m_data_deletes{};
  int m_placement_deletes{};
};

auto aspect_fit(Rect available, Extent per_cell) -> Rect {
  if (available.empty()) return {};
  const int cell_w = std::max(1, per_cell.w);
  const int cell_h = std::max(1, per_cell.h);
  int width = available.w;
  int height =
      static_cast<int>((static_cast<std::int64_t>(width) * kHeight * cell_w) /
                       (static_cast<std::int64_t>(kWidth) * cell_h));
  if (height <= 0) height = 1;
  if (height > available.h) {
    height = available.h;
    width =
        static_cast<int>((static_cast<std::int64_t>(height) * kWidth * cell_h) /
                         (static_cast<std::int64_t>(kHeight) * cell_w));
    width = std::clamp(width, 1, available.w);
  }
  return Rect{available.x + (available.w - width) / 2,
              available.y + (available.h - height) / 2, width, height};
}

class GameWorkload final : public App {
 public:
  explicit GameWorkload(double capture_seconds = 0.0)
      : m_capture_seconds(capture_seconds) {
    set_frame_ms(33);
    set_tick_hz(120);
    set_max_tick_dt(std::chrono::duration<double>{0.125});
    set_mouse_mode(MouseMode::None);
    m_audit.set_collect(capture_seconds > 0.0);
    if (capture_seconds > 0.0) enable_observation();
    render_frame();
  }

  auto on_start() -> void override {
    if (m_capture_seconds > 0.0) {
      m_audit.set_fd(terminal().io().out);
      driver().set_output(&m_audit);
      m_audit_bound = true;
    }
    if (m_capture_seconds > 0.0 && !capabilities().kitty_graphics) {
      m_capture_error =
          "capture mode requires a terminal that negotiated Kitty graphics";
      quit();
    }
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      m_last_error = error->message;
      return;
    }
    App::on_event(event);
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override {
    m_phase += dt.count();
  }

  auto on_render(Screen& screen) -> void override {
    // test_run_frames does not call on_start, so the headless benchmark binds
    // its discard/count sink here before the frame's only flush.
    if (m_headless && !m_audit_bound) {
      driver().set_output(&m_audit);
      m_audit_bound = true;
    }

    // The headless benchmark is uncapped so it measures work instead of a
    // sleep. Advance the same 1/30 s of deterministic simulation explicitly;
    // the live path receives four 1/120 s ticks from App's fixed-step clock.
    if (m_headless) m_phase += 1.0 / 30.0;

    const bool unchanged = m_frame != 0 && m_frame % kHoldInterval == 0;
    if (m_started == Clock::time_point{}) m_started = Clock::now();
    m_pending_unchanged = unchanged;

    m_pending_generation_ms = 0.0;
    if (!unchanged) {
      const auto generation_started = Clock::now();
      render_frame();
      m_pending_generation_ms = elapsed_ms(generation_started, Clock::now());
    }

    screen.clear();
    const auto bytes = driver().total_bytes();
    screen.write_text(
        0, 0,
        std::format(" game workload 320x180 | frame {} | {} | {:.1f} MiB ",
                    m_frame, unchanged ? "clean" : "changed",
                    static_cast<double>(bytes.total()) / (1024.0 * 1024.0)),
        Rgb{235, 240, 250}, Rgb{20, 45, 85});
    screen.write_text(0, screen.rows() - 1,
                      " fixed 120 Hz simulation | 30 FPS display | ESC quits ",
                      Rgb{180, 205, 225}, Rgb{10, 20, 35});

    const Rect available{0, 1, screen.cols(), std::max(0, screen.rows() - 2)};
    const Extent one_cell = driver().preferred_pixel_extent({0, 0, 1, 1});
    m_surface.set_geometry(aspect_fit(available, one_cell));
    m_surface.draw(screen);
    render_pixel_regions(m_surface);
    ++m_frame;

    if (m_capture_seconds > 0.0 &&
        std::chrono::duration<double>(Clock::now() - m_started).count() >=
            m_capture_seconds) {
      quit();
    }
  }

  auto benchmark(int frames) -> void {
    m_headless = true;
    m_audit.set_collect(true);
    enable_observation();
    set_frame_ms(0);
    auto selected = std::make_unique<KittyDriver>();
    selected->set_cell_pixel_size({8, 16});
    test_run_frames(frames, 120, 40, nullptr, std::move(selected));
  }

  [[nodiscard]] auto summary() const -> CaptureSummary {
    CaptureSummary out = m_audit.lifecycle_summary();
    out.frames = static_cast<int>(m_samples.size());
    std::vector<double> frame_times;
    frame_times.reserve(m_samples.size());
    for (const auto& sample : m_samples) {
      out.wire_bytes += sample.bytes;
      out.generation_avg_ms += sample.generation_ms;
      out.submission_avg_ms += sample.submission_ms;
      out.frame_avg_ms += sample.frame_ms;
      out.generation_max_ms =
          std::max(out.generation_max_ms, sample.generation_ms);
      out.submission_max_ms =
          std::max(out.submission_max_ms, sample.submission_ms);
      out.frame_max_ms = std::max(out.frame_max_ms, sample.frame_ms);
      if (sample.frame_ms > kBudgetMs) ++out.missed_deadlines;
      if (sample.unchanged) {
        ++out.unchanged_frames;
        out.unchanged_image_bytes += sample.image_bytes;
      }
      frame_times.push_back(sample.frame_ms);
    }
    if (!m_samples.empty()) {
      const double n = static_cast<double>(m_samples.size());
      out.generation_avg_ms /= n;
      out.submission_avg_ms /= n;
      out.frame_avg_ms /= n;
      out.bytes_per_frame = static_cast<double>(out.wire_bytes) / n;
      out.elapsed_seconds = std::chrono::duration<double>(m_last_observation -
                                                          m_first_observation)
                                .count();
      if (out.elapsed_seconds > 0.0) {
        out.achieved_fps = n / out.elapsed_seconds;
        out.bytes_per_second =
            static_cast<double>(out.wire_bytes) / out.elapsed_seconds;
      }
      std::sort(frame_times.begin(), frame_times.end());
      const std::size_t p95 = std::min(
          frame_times.size() - 1,
          static_cast<std::size_t>(std::ceil(frame_times.size() * 0.95)) - 1);
      out.frame_p95_ms = frame_times[p95];
    }
    return out;
  }
  [[nodiscard]] auto capture_error() const -> const std::string& {
    return m_capture_error;
  }
  [[nodiscard]] auto last_error() const -> const std::string& {
    return m_last_error;
  }

 private:
  static auto milliseconds(std::chrono::nanoseconds duration) noexcept
      -> double {
    return std::chrono::duration<double, std::milli>(duration).count();
  }

  auto enable_observation() -> void {
    set_frame_observer([this](const FrameObservation& observation) {
      const auto observed = Clock::now();
      if (m_samples.empty()) m_first_observation = observed;
      m_last_observation = observed;

      const auto render_and_submission = observation.application_render +
                                         observation.framework_submission +
                                         observation.sink_write;
      const double tick_ms = milliseconds(observation.tick);
      const double frame_ms = tick_ms + milliseconds(render_and_submission);
      // The procedural raster is the one application-private phase App cannot
      // identify. Everything after it -- the rest of on_render, TermForge's
      // submission, and the blocking sink handoff -- remains the historical
      // "submission pipeline" field in the evidence JSON.
      const double submission_ms = std::max(
          0.0, milliseconds(render_and_submission) - m_pending_generation_ms);
      m_samples.push_back(FrameSample{
          .bytes = observation.bytes.total(),
          .image_bytes =
              observation.bytes.image_transmit + observation.bytes.image_edit,
          .tick_ms = tick_ms,
          .generation_ms = m_pending_generation_ms,
          .submission_ms = submission_ms,
          .frame_ms = frame_ms,
          .unchanged = m_pending_unchanged,
      });
    });
  }

  auto render_frame() -> void {
    auto pixels = m_surface.pixels();
    const double ship_x = 160.0 + std::sin(m_phase * 1.7) * 90.0;
    const double ship_y = 90.0 + std::cos(m_phase * 1.1) * 48.0;
    const int phase = static_cast<int>(m_phase * 90.0);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int stars = (x * 73 + y * 151 + phase * 7) & 1023;
        const auto base = static_cast<std::uint8_t>(8 + (y * 18) / kHeight);
        Pixel pixel{static_cast<std::uint8_t>(base / 2), base,
                    static_cast<std::uint8_t>(base + 12), 255};
        if (stars < 5) pixel = Pixel{180, 210, 255, 255};
        const double dx = x - ship_x;
        const double dy = y - ship_y;
        if (dx * dx + dy * dy < 17.0 * 17.0) {
          const auto glow = static_cast<std::uint8_t>(std::clamp(
              255.0 - std::sqrt(dx * dx + dy * dy) * 8.0, 70.0, 255.0));
          pixel = Pixel{glow, static_cast<std::uint8_t>(glow * 3 / 4), 60, 255};
        }
        pixels[static_cast<std::size_t>(y) * kWidth + x] = pixel;
      }
    }
  }

  PixelSurface m_surface{Extent{kWidth, kHeight}, Pixel{0, 0, 0, 255}};
  ProtocolAuditSink m_audit;
  std::vector<FrameSample> m_samples;
  Clock::time_point m_started{};
  Clock::time_point m_first_observation{};
  Clock::time_point m_last_observation{};
  double m_capture_seconds{};
  double m_phase{};
  double m_pending_generation_ms{};
  int m_frame{};
  bool m_pending_unchanged{false};
  bool m_headless{false};
  bool m_audit_bound{false};
  std::string m_capture_error;
  std::string m_last_error;
};

auto print_human(const CaptureSummary& s) -> void {
  std::printf(
      "game workload: frames=%d elapsed=%.3fs achieved=%.2ffps missed=%d\n"
      "timing: generation avg/max %.3f/%.3fms, submission %.3f/%.3fms, "
      "frame avg/p95/max %.3f/%.3f/%.3fms\n"
      "wire: %.1f KiB/frame, %.2f MiB/s, total %.2f MiB, clean frames "
      "%d (%llu image bytes)\n"
      "lifecycle: ids=%zu peak-live=%zu uploads=%d updates=%d placements=%d "
      "data-deletes=%d placement-deletes=%d shutdown=%lluB\n",
      s.frames, s.elapsed_seconds, s.achieved_fps, s.missed_deadlines,
      s.generation_avg_ms, s.generation_max_ms, s.submission_avg_ms,
      s.submission_max_ms, s.frame_avg_ms, s.frame_p95_ms, s.frame_max_ms,
      s.bytes_per_frame / 1024.0, s.bytes_per_second / (1024.0 * 1024.0),
      static_cast<double>(s.wire_bytes) / (1024.0 * 1024.0), s.unchanged_frames,
      static_cast<unsigned long long>(s.unchanged_image_bytes),
      s.unique_image_ids, s.peak_live_image_ids, s.data_transmits,
      s.root_updates, s.placements, s.data_deletes, s.placement_deletes,
      static_cast<unsigned long long>(s.shutdown_bytes));
}

auto json(const CaptureSummary& s) -> std::string {
  return std::format(
      "{{\n"
      "  \"workload\": \"game-320x180-rgba\",\n"
      "  \"requested_fps\": 30,\n"
      "  \"frames\": {},\n"
      "  \"elapsed_seconds\": {:.6f},\n"
      "  \"achieved_fps\": {:.6f},\n"
      "  \"missed_deadlines\": {},\n"
      "  \"generation_avg_ms\": {:.6f},\n"
      "  \"generation_max_ms\": {:.6f},\n"
      "  \"submission_avg_ms\": {:.6f},\n"
      "  \"submission_max_ms\": {:.6f},\n"
      "  \"frame_avg_ms\": {:.6f},\n"
      "  \"frame_p95_ms\": {:.6f},\n"
      "  \"frame_max_ms\": {:.6f},\n"
      "  \"wire_bytes\": {},\n"
      "  \"bytes_per_frame\": {:.6f},\n"
      "  \"bytes_per_second\": {:.6f},\n"
      "  \"unchanged_frames\": {},\n"
      "  \"unchanged_image_bytes\": {},\n"
      "  \"unique_image_ids\": {},\n"
      "  \"peak_live_image_ids\": {},\n"
      "  \"data_transmits\": {},\n"
      "  \"root_frame_updates\": {},\n"
      "  \"placements\": {},\n"
      "  \"data_deletes\": {},\n"
      "  \"placement_deletes\": {},\n"
      "  \"shutdown_bytes\": {}\n"
      "}}\n",
      s.frames, s.elapsed_seconds, s.achieved_fps, s.missed_deadlines,
      s.generation_avg_ms, s.generation_max_ms, s.submission_avg_ms,
      s.submission_max_ms, s.frame_avg_ms, s.frame_p95_ms, s.frame_max_ms,
      s.wire_bytes, s.bytes_per_frame, s.bytes_per_second, s.unchanged_frames,
      s.unchanged_image_bytes, s.unique_image_ids, s.peak_live_image_ids,
      s.data_transmits, s.root_updates, s.placements, s.data_deletes,
      s.placement_deletes, s.shutdown_bytes);
}

auto parse_positive(std::string_view text, int& out) -> bool {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size() && out > 0;
}

auto usage() -> void {
  std::puts(
      "Usage: termforge_example_game [--benchmark [FRAMES]]\n"
      "       termforge_example_game [--capture-seconds N --report PATH]\n\n"
      "Default mode runs the deterministic 320x180 workload interactively.\n"
      "Benchmark mode is headless; capture mode requires real Kitty graphics.");
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::optional<int> benchmark_frames;
  int capture_seconds = 0;
  std::filesystem::path report_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--benchmark") {
      benchmark_frames = 180;
      if (i + 1 < argc) {
        int value = 0;
        const std::string_view next{argv[i + 1]};
        if (parse_positive(next, value)) {
          benchmark_frames = value;
          ++i;
        }
      }
      continue;
    }
    if (arg == "--capture-seconds" && i + 1 < argc) {
      if (!parse_positive(argv[++i], capture_seconds)) {
        std::fprintf(stderr, "game: capture seconds must be positive\n");
        return 2;
      }
      continue;
    }
    if (arg == "--report" && i + 1 < argc) {
      report_path = argv[++i];
      continue;
    }
    std::fprintf(stderr, "game: unknown or incomplete option '%.*s'\n",
                 static_cast<int>(arg.size()), arg.data());
    return 2;
  }

  if (benchmark_frames && capture_seconds > 0) {
    std::fprintf(stderr, "game: benchmark and live capture are exclusive\n");
    return 2;
  }
  if (capture_seconds > 0 && report_path.empty()) {
    std::fprintf(stderr, "game: live capture requires --report PATH\n");
    return 2;
  }
  if (!benchmark_frames && capture_seconds == 0 && !report_path.empty()) {
    std::fprintf(stderr,
                 "game: --report requires --benchmark or --capture-seconds\n");
    return 2;
  }

  try {
    GameWorkload app{static_cast<double>(capture_seconds)};
    int result = 0;
    if (benchmark_frames) {
      app.benchmark(*benchmark_frames);
    } else {
      result = app.run();
    }
    if (!app.capture_error().empty()) {
      std::fprintf(stderr, "game: %s\n", app.capture_error().c_str());
      return 3;
    }
    if (!app.last_error().empty()) {
      std::fprintf(stderr, "game: last ErrorEvent: %s\n",
                   app.last_error().c_str());
    }
    const CaptureSummary summary = app.summary();
    if (benchmark_frames || capture_seconds > 0) {
      print_human(summary);
      const std::string document = json(summary);
      if (report_path.empty()) {
        std::fwrite(document.data(), 1, document.size(), stdout);
      } else {
        std::ofstream out{report_path};
        if (!out) {
          std::fprintf(stderr, "game: cannot open report '%s'\n",
                       report_path.string().c_str());
          return 1;
        }
        out << document;
      }
    }
    return result;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "game: %s\n", error.what());
    return 1;
  }
}
