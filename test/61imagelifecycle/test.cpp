// TermForge — image lifecycle across terminal transitions (#113).
//
// docs/pixel-regions.md "Image lifecycle across terminal transitions" is the
// written contract. These cases pin the offline half of CURRENT behaviour:
//
//   * in-session resize keeps resident payloads (no second a=t) and refreshes
//     placement;
//   * normal shutdown requests protocol-wide cleanup with a=d,d=A while the
//     sink is alive;
//   * per-frame collection retires an undrawn unpinned region with d=I and an
//     undrawn pinned placement with d=i.
//
// Phase 2 adds the explicit invalidation boundary: no deletes are emitted for
// data the terminal already discarded, old handles stay stale across id reuse,
// and App recreates Persistent widget content before delivering the next
// enhanced frame.
//
// All offline against an in-memory sink / string. No pty.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <signal.h>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <unistd.h>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/pixel_surface.hpp"

using namespace termforge;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placements_of;
using tfsupport::total_data_transmits;
using tfsupport::transmits_of;

namespace {

auto art(int seed) -> Image {
  const auto v = static_cast<std::uint8_t>(seed);
  return tfsupport::checker(2, 2, Pixel{v, 0, 0, 255},
                            Pixel{0, static_cast<std::uint8_t>(255 - v), 0,
                                  255});
}

class SegmentSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++writes;
    if (refuse_on_write != 0 && writes == refuse_on_write) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "sink", "image invalidation frame refused"}};
    }
    segments.emplace_back(bytes.data(), bytes.size());
    return {};
  }

  int writes{0};
  int refuse_on_write{0};
  std::vector<std::string> segments;
};

// Fixed-extent Persistent surface so a grid resize cannot look like a content
// recreate. PixelSurface returns the same Image Extent across preferred-pixel
// answers; App therefore re-places without unpin/re-pin.
class ResizeLifecycleApp final : public App {
 public:
  PixelSurface surface{Extent{320, 180}, Pixel{30, 80, 160, 255}};
  SegmentSink sink;
  int resize_events{0};
  int error_events{0};

  auto on_event(const Event& ev) -> void override {
    if (std::holds_alternative<ResizeEvent>(ev)) ++resize_events;
    if (std::holds_alternative<ErrorEvent>(ev)) ++error_events;
  }

  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    // Arm on frame index 1 so frame 0 transmits once, then the next frame_step
    // consumes the same resize path SIGWINCH uses (set_size + pending flag).
    if (m_frame == 1) {
      REQUIRE(set_size(Size{24, 10, 240, 160}).has_value());
    }
    screen.clear();
    surface.set_geometry({2, 1, 4, 2});
    surface.draw(screen);
    render_pixel_regions(surface);
    ++m_frame;
  }

  auto run(int frames) -> void {
    // Seed a pushed size so current_size() is deterministic offline (no ioctl).
    // Clear the arm set_size leaves: a pre-run push would otherwise make frame 0
    // a resize and double-count ResizeEvent against the mid-run transition.
    REQUIRE(set_size(Size{20, 8, 160, 128}).has_value());
    REQUIRE(test_take_resize());
    test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
  }

 protected:
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  int m_frame{0};
  std::chrono::steady_clock::time_point m_now{};
};

class InvalidationLifecycleApp final : public App {
 public:
  PixelSurface surface{Extent{320, 180}, Pixel{20, 100, 180, 255}};
  SegmentSink sink;
  int invalidations{0};
  bool event_before_recreate{false};
  ImageInvalidationReason last_reason{
      ImageInvalidationReason::TerminalReset};

  auto on_event(const Event& ev) -> void override {
    if (const auto* invalidated = std::get_if<ImageInvalidatedEvent>(&ev)) {
      ++invalidations;
      last_reason = invalidated->reason;
    }
  }

  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (m_frame == 2) event_before_recreate = invalidations == 1;
    if (m_frame == 1) {
      REQUIRE(invalidate_images(ImageInvalidationReason::Reattach).has_value());
    }
    screen.clear();
    surface.set_geometry({2, 1, 4, 2});
    surface.draw(screen);
    render_pixel_regions(surface);
    ++m_frame;
  }

  auto run(int frames) -> void {
    test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
  }

 protected:
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  int m_frame{0};
  std::chrono::steady_clock::time_point m_now{};
};

class InvalidationEventApp final : public App {
 public:
  auto on_event(const Event& ev) -> void override {
    if (const auto* invalidated = std::get_if<ImageInvalidatedEvent>(&ev)) {
      ++count;
      reason = invalidated->reason;
    }
  }
  auto on_render(Screen& screen) -> void override { screen.clear(); }

  int count{0};
  ImageInvalidationReason reason{ImageInvalidationReason::TerminalReset};
};

class QuietPipe {
 public:
  QuietPipe() { m_ok = ::pipe(m_fd) == 0; }
  ~QuietPipe() {
    for (const int fd : m_fd)
      if (fd >= 0) ::close(fd);
  }
  QuietPipe(const QuietPipe&) = delete;
  auto operator=(const QuietPipe&) -> QuietPipe& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto read_fd() const noexcept -> int { return m_fd[0]; }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class ResumeLifecycleApp final : public App {
 public:
  PixelSurface surface{Extent{320, 180}, Pixel{80, 30, 160, 255}};
  SegmentSink sink;
  int invalidations{0};

  auto configure(int fd) -> bool {
    Capabilities caps;
    caps.kitty_graphics = true;
    set_frame_ms(0);
    return terminal().set_io(TerminalIo{fd, -1}).has_value() &&
           terminal().set_capabilities(caps).has_value() &&
           set_size(Size{20, 8, 160, 128}).has_value();
  }

  auto on_start() -> void override { driver().set_output(&sink); }

  auto on_event(const Event& ev) -> void override {
    if (const auto* invalidated = std::get_if<ImageInvalidatedEvent>(&ev)) {
      CHECK(invalidated->reason == ImageInvalidationReason::SuspendResume);
      ++invalidations;
    }
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    surface.set_geometry({2, 1, 4, 2});
    surface.draw(screen);
    render_pixel_regions(surface);
    if (m_frame == 1) REQUIRE(::raise(SIGCONT) == 0);
    ++m_frame;
    if (invalidations == 1) quit();
  }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  int m_frame{0};
};

volatile sig_atomic_t g_prior_cont_calls{0};
void prior_cont_handler(int) { g_prior_cont_calls = 1; }

volatile sig_atomic_t g_newer_cont_calls{0};
void newer_cont_handler(int) { g_newer_cont_calls = 1; }

class NewerContinueHandlerApp final : public App {
 public:
  auto configure(int fd) -> bool {
    Capabilities caps;
    set_frame_ms(0);
    return terminal().set_io(TerminalIo{fd, -1}).has_value() &&
           terminal().set_capabilities(caps).has_value() &&
           set_size(Size{20, 8, 160, 128}).has_value();
  }

  auto on_start() -> void override {
    struct sigaction action {};
    action.sa_handler = newer_cont_handler;
    ::sigemptyset(&action.sa_mask);
    installed = ::sigaction(SIGCONT, &action, nullptr) == 0;
    quit();
  }

  auto on_event(const Event&) -> void override {}
  auto on_render(Screen&) -> void override {}

  bool installed{false};
};

class SignalActionGuard {
 public:
  explicit SignalActionGuard(int signal) : m_signal(signal) {
    m_ok = ::sigaction(signal, nullptr, &m_prior) == 0;
  }
  ~SignalActionGuard() {
    if (m_ok) (void)::sigaction(m_signal, &m_prior, nullptr);
  }
  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }

 private:
  int m_signal;
  struct sigaction m_prior {};
  bool m_ok{false};
};

}  // namespace

TEST_CASE("in-session resize keeps one resident transmit (#113)",
          "[imagelifecycle][app][kitty][resize]") {
  ResizeLifecycleApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.run(4);

  REQUIRE(app.sink.segments.size() >= 4);
  std::string frames;
  // Last segment is shutdown's d=A; pin the production frames only.
  for (std::size_t i = 0; i + 1 < app.sink.segments.size(); ++i)
    frames += app.sink.segments[i];

  CHECK(ids_named(frames) == std::set<std::uint32_t>{272});
  CHECK(transmits_of(frames, 272) == 1);
  CHECK(data_deletes_of(frames, 272) == 0);
  // Classic: already-live same-rect placement is a no-op on force-repaint, so
  // the wire still shows exactly one a=p for the life of the region.
  CHECK(placements_of(frames, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
  CHECK(app.resize_events == 1);
  CHECK(app.error_events == 0);
  CHECK(app.sink.segments.back().find("a=d,d=A") != std::string::npos);
}

TEST_CASE("in-session resize re-emits Unicode placeholder grid without "
          "retransmit (#113)",
          "[imagelifecycle][app][kitty][resize][unicode]") {
  ResizeLifecycleApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  auto driver = std::make_unique<KittyDriver>();
  driver->set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  REQUIRE(app.set_size(App::Size{20, 8, 160, 128}).has_value());
  REQUIRE(app.test_take_resize());
  app.test_run_frames(4, 20, 8, nullptr, std::move(driver));

  REQUIRE(app.sink.segments.size() >= 4);
  std::string frames;
  for (std::size_t i = 0; i + 1 < app.sink.segments.size(); ++i)
    frames += app.sink.segments[i];

  CHECK(transmits_of(frames, 272) == 1);
  CHECK(placements_of(frames, 272) == 1);
  // 4x2 grid twice: initial place + force-repaint after ResizeEvent.
  CHECK(tfsupport::count_of(frames, "\xF4\x8E\xBB\xAE") == 16);
  CHECK(app.surface.submission_count() == 1);
  CHECK(app.resize_events == 1);
}

TEST_CASE("shutdown emits uppercase delete-all while the sink is alive (#113)",
          "[imagelifecycle][kitty][teardown]") {
  KittyDriver d;
  SegmentSink sink;
  d.set_output(&sink);

  const auto pinned = d.pin_image(art(3));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();
  REQUIRE(d.draw_image(Rect{4, 0, 2, 2}, art(4)).has_value());
  d.flush();

  d.shutdown();

  REQUIRE_FALSE(sink.segments.empty());
  const std::string& last = sink.segments.back();
  CHECK(last.find("a=d,d=A") != std::string::npos);
  // A second shutdown is a no-op (base latch); no duplicate delete-all.
  const std::size_t writes = sink.segments.size();
  d.shutdown();
  CHECK(sink.segments.size() == writes);
}

TEST_CASE("collection retires undrawn region data and undrawn pin placement "
          "(#113)",
          "[imagelifecycle][kitty][collection]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(5));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  REQUIRE(d.draw_image(Rect{4, 0, 2, 2}, art(6)).has_value());
  d.flush();

  const auto region_ids = ids_named(out);
  REQUIRE(region_ids.contains(1));
  REQUIRE(region_ids.contains(pinned->id));
  out.clear();

  // Redraw neither the pin placement nor the unpinned region.
  d.flush();

  CHECK(data_deletes_of(out, 1) == 1);
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(total_data_transmits(out) == 0);

  // Pin handle remains live: a later place must not retransmit.
  out.clear();
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("invalidation forgets pins without wire and stale handles survive id "
          "reuse (#113)",
          "[imagelifecycle][kitty][invalidate][pinned]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto old = d.pin_image(art(7));
  REQUIRE(old.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *old).has_value());
  d.flush();
  REQUIRE(transmits_of(out, old->id) == 1);
  out.clear();

  d.invalidate_images();
  d.flush();
  CHECK(out.empty());
  CHECK(d.last_frame_bytes().total() == 0);

  const auto stale_draw = d.draw_pinned(Rect{0, 0, 2, 2}, *old);
  REQUIRE_FALSE(stale_draw.has_value());
  CHECK(stale_draw.error().severity == Severity::Warning);
  CHECK(stale_draw.error().message.find("stale") != std::string::npos);
  CHECK(out.empty());

  const auto fresh = d.pin_image(art(8));
  REQUIRE(fresh.has_value());
  CHECK(fresh->id == old->id);
  CHECK(fresh->serial != old->serial);
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *fresh).has_value());
  d.flush();
  CHECK(transmits_of(out, fresh->id) == 1);
  CHECK(data_deletes_of(out, fresh->id) == 0);

  out.clear();
  const auto stale_unpin = d.unpin_image(*old);
  REQUIRE_FALSE(stale_unpin.has_value());
  CHECK(stale_unpin.error().message.find("stale") != std::string::npos);
  // The refused operation itself queues nothing.  Keep the fresh placement
  // live before crossing another flush boundary so ordinary GC cannot obscure
  // that property with its independent placement retirement.
  CHECK(out.empty());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *fresh).has_value());
  d.flush();
  CHECK(out.empty());
}

TEST_CASE("invalidation makes an ordinary region retransmit without a delete "
          "(#113)",
          "[imagelifecycle][kitty][invalidate][region]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{1, 1, 2, 2}, art(9)).has_value());
  d.flush();
  REQUIRE(total_data_transmits(out) == 1);
  out.clear();

  d.invalidate_images();
  REQUIRE(d.draw_image(Rect{1, 1, 2, 2}, art(9)).has_value());
  d.flush();
  CHECK(total_data_transmits(out) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
}

TEST_CASE("App invalidation recreates Persistent content in the event frame "
          "(#113)",
          "[imagelifecycle][app][kitty][invalidate][persistent]") {
  InvalidationLifecycleApp app;
  app.run(4);

  std::string frames;
  REQUIRE(app.sink.segments.size() >= 4);
  for (std::size_t i = 0; i + 1 < app.sink.segments.size(); ++i)
    frames += app.sink.segments[i];

  CHECK(app.invalidations == 1);
  CHECK(app.event_before_recreate);
  CHECK(app.last_reason == ImageInvalidationReason::Reattach);
  CHECK(app.surface.submission_count() == 2);
  CHECK(transmits_of(frames, 272) == 2);
  CHECK(data_deletes_of(frames, 272) == 0);
}

TEST_CASE("pending invalidations coalesce at one clean frame boundary (#113)",
          "[imagelifecycle][app][invalidate][event]") {
  InvalidationEventApp app;
  REQUIRE(app.invalidate_images(ImageInvalidationReason::SuspendResume)
              .has_value());
  REQUIRE(app.invalidate_images(ImageInvalidationReason::Reattach).has_value());
  app.test_run_frames(1, 10, 4, nullptr);

  CHECK(app.count == 1);
  CHECK(app.reason == ImageInvalidationReason::Reattach);

  const auto invalid = app.invalidate_images(
      static_cast<ImageInvalidationReason>(99));
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().severity == Severity::Warning);
}

TEST_CASE("a refused invalidation frame retries Persistent recreation (#113)",
          "[imagelifecycle][app][invalidate][persistent][sink]") {
  InvalidationLifecycleApp app;
  // Frame 0 uploads, frame 1 stages invalidation, and frame 2 attempts the
  // recreation. Refuse that complete frame so App must retain recreate=true
  // until frame 3's accepted write.
  app.sink.refuse_on_write = 3;
  app.run(5);

  CHECK(app.invalidations == 1);
  CHECK(app.event_before_recreate);
  CHECK(app.surface.submission_count() == 2);
  CHECK(app.sink.writes == 6);  // five frames plus accepted shutdown
  std::string accepted;
  for (const auto& segment : app.sink.segments) accepted += segment;
  CHECK(transmits_of(accepted, 272) == 2);
}

TEST_CASE("SIGCONT invalidates at the next frame and restores the prior handler "
          "(#113)",
          "[imagelifecycle][app][signal][invalidate]") {
  SignalActionGuard restore{SIGCONT};
  REQUIRE(restore.ok());
  struct sigaction prior {};
  prior.sa_handler = prior_cont_handler;
  ::sigemptyset(&prior.sa_mask);
  REQUIRE(::sigaction(SIGCONT, &prior, nullptr) == 0);
  g_prior_cont_calls = 0;

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  ResumeLifecycleApp app;
  REQUIRE(app.configure(pipe.read_fd()));
  REQUIRE(app.run() == 0);

  CHECK(app.invalidations == 1);
  CHECK(app.surface.submission_count() == 2);
  std::string wire;
  for (const auto& segment : app.sink.segments) wire += segment;
  CHECK(transmits_of(wire, 272) == 2);
  CHECK(data_deletes_of(wire, 272) == 0);

  struct sigaction current {};
  REQUIRE(::sigaction(SIGCONT, nullptr, &current) == 0);
  REQUIRE((current.sa_flags & SA_SIGINFO) == 0);
  CHECK(current.sa_handler == prior_cont_handler);
  REQUIRE(::raise(SIGCONT) == 0);
  CHECK(g_prior_cont_calls == 1);
}

TEST_CASE("SIGCONT teardown preserves a newer process handler (#113)",
          "[imagelifecycle][app][signal][ownership]") {
  SignalActionGuard restore{SIGCONT};
  REQUIRE(restore.ok());
  g_newer_cont_calls = 0;

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  NewerContinueHandlerApp app;
  REQUIRE(app.configure(pipe.read_fd()));
  REQUIRE(app.run() == 0);
  REQUIRE(app.installed);

  struct sigaction current {};
  REQUIRE(::sigaction(SIGCONT, nullptr, &current) == 0);
  REQUIRE((current.sa_flags & SA_SIGINFO) == 0);
  CHECK(current.sa_handler == newer_cont_handler);
  REQUIRE(::raise(SIGCONT) == 0);
  CHECK(g_newer_cont_calls == 1);
}
