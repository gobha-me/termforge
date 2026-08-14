// TermForge — image lifecycle across terminal transitions (#113), Phase 1.
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
// They deliberately do NOT assert an ImageInvalidatedEvent: that event does
// not exist on Event yet, and this offline seam cannot model suspend/reattach.
// The suite header states that limitation instead of freezing the missing API
// in a test that would fail when the intended event is added.
//
// All offline against an in-memory sink / string. No pty.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

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
    segments.emplace_back(bytes.data(), bytes.size());
    return {};
  }

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
