// TermForge — persistent PixelSurface (#195).
//
// The direct cases pin the owned-buffer and Baseline contracts. The App cases
// use test_run_frames with real drivers, so they observe production ordering:
// authored cells first, one collected image region after the diff, one frame
// write. They do not replay a driver call sequence.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "support/apc.hpp"
#include "termforge/core/app.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/pixel_surface.hpp"

using namespace termforge;

namespace {

auto text_at(const Screen& screen, int x, int y) -> const std::string& {
  return screen.at(x, y).text;
}

struct BlockingOverlay final : Widget {
  auto draw(Screen& screen) -> void override {
    screen.fill_rect(0, 0, screen.cols(), screen.rows(), Rgb{240, 240, 240},
                     Rgb{20, 20, 20});
  }
};

class FailingOnceSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++writes;
    if (writes == fail_on) {
      return std::unexpected{ErrorEvent{Severity::Warning, "sink",
                                        "first frame refused"}};
    }
    accepted.append(bytes.data(), bytes.size());
    return {};
  }

  int writes{0};
  int fail_on{1};
  std::string accepted;
};

class SegmentSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    segments.emplace_back(bytes.data(), bytes.size());
    return {};
  }

  std::vector<std::string> segments;
};

class SurfaceApp final : public App {
 public:
  PixelSurface surface{Extent{320, 180}, Pixel{0, 0, 0, 255}};
  std::string wire;
  int errors{0};
  std::string last_error;
  Severity last_severity{Severity::Info};

  auto on_event(const Event& ev) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&ev)) {
      ++errors;
      last_error = error->message;
      last_severity = error->severity;
    }
  }

  auto on_render(Screen& screen) -> void override {
    if (output_override != nullptr) driver().set_output(output_override);
    if (m_frame == 1) content_dirty_on_second_frame = surface.content_dirty();
    if (m_frame == suspend_on_frame) push_overlay(m_overlay);
    if (m_frame == resume_on_frame) pop_overlay();
    if (m_frame == invalidate_on_frame) {
      surface.pixels()[0] = Pixel{220, 40, 80, 255};
    }
    if (m_frame == reset_on_frame) {
      surface.reset(Extent{160, 90}, Pixel{90, 40, 180, 255});
    }
    if (m_frame == move_on_frame) destination = moved_destination;
    if (m_frame == resize_on_frame) request_resize();
    const bool submit_pixels =
        stop_submitting_on_frame < 0 || m_frame < stop_submitting_on_frame;
    ++m_frame;
    screen.clear();
    surface.set_geometry(destination);
    surface.draw(screen);
    if (submit_pixels) render_pixel_regions(surface);
  }

  auto run_with(std::unique_ptr<TerminalDriver> driver, int frames = 1)
      -> void {
    test_run_frames(frames, 20, 8, &wire, std::move(driver));
  }

  Rect destination{2, 1, 4, 2};
  int suspend_on_frame{-1};
  int resume_on_frame{-1};
  int invalidate_on_frame{-1};
  int reset_on_frame{-1};
  int move_on_frame{-1};
  int resize_on_frame{-1};
  int stop_submitting_on_frame{-1};
  Rect moved_destination{8, 3, 4, 2};
  ByteSink* output_override{nullptr};
  bool content_dirty_on_second_frame{false};

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
  std::chrono::steady_clock::time_point m_now{};
  BlockingOverlay m_overlay;
  int m_frame{0};
};

// #198 step 6's production workload shape. It mutates one fixed 320x180
// surface on every display frame except two deliberate clean frames. The
// point is the App cadence, not the cost of the tiny synthetic producer: one
// initial resident upload, root-frame edits for changes, and no image wire at
// all when the producer leaves the surface clean.
class DynamicSurfaceApp final : public App {
 public:
  PixelSurface surface{Extent{320, 180}, Pixel{8, 16, 24, 255}};
  SegmentSink sink;
  std::vector<FrameBytes> observed_frames;

  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (m_frame > 0) observed_frames.push_back(driver().last_frame_bytes());
    const bool unchanged = m_frame == 30 || m_frame == 60;
    if (!unchanged) {
      surface.pixels()[0] =
          Pixel{static_cast<std::uint8_t>(m_frame), 80, 160, 255};
    }
    screen.clear();
    surface.set_geometry({2, 1, 16, 6});
    surface.draw(screen);
    render_pixel_regions(surface);
    ++m_frame;
  }

  auto run() -> void {
    test_run_frames(62, 20, 8, nullptr, std::make_unique<KittyDriver>());
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

TEST_CASE("Image exposes mutable values without exposing its shape",
          "[pixelsurface][image]") {
  Image image{2, 1, {Pixel{1, 2, 3, 4}, Pixel{5, 6, 7, 8}}};

  image.at(0, 0) = Pixel{9, 10, 11, 12};
  auto pixels = image.pixels();
  REQUIRE(pixels.size() == 2);
  pixels[1] = Pixel{13, 14, 15, 16};

  CHECK(image.width() == 2);
  CHECK(image.height() == 1);
  CHECK(std::as_const(image).at(0, 0) == Pixel{9, 10, 11, 12});
  CHECK(std::as_const(image).at(1, 0) == Pixel{13, 14, 15, 16});
}

TEST_CASE("PixelSurface owns one fixed-resolution mutable framebuffer",
          "[pixelsurface]") {
  PixelSurface surface{Extent{320, 180}, Pixel{1, 2, 3, 255}};
  const Pixel* storage = std::as_const(surface).pixels().data();

  REQUIRE(surface.extent() == Extent{320, 180});
  REQUIRE(std::as_const(surface).pixels().size() == 320U * 180U);

  surface.set_geometry({1, 2, 40, 12});
  CHECK(surface.pixel_regions() == std::vector<Rect>{{1, 2, 40, 12}});
  CHECK(surface.draw_pixels(surface.rect(), Extent{640, 192}) ==
        &std::as_const(surface).image());
  CHECK(std::as_const(surface).pixels().data() == storage);
  CHECK(surface.extent() == Extent{320, 180});

  surface.set_geometry({3, 4, 60, 20});
  CHECK(std::as_const(surface).pixels().data() == storage);
  CHECK(surface.extent() == Extent{320, 180});
}

TEST_CASE("PixelSurface mutable access and recreation update dirty state",
          "[pixelsurface]") {
  PixelSurface surface{Extent{2, 1}, Pixel{0, 0, 0, 255}};
  surface.set_geometry({0, 0, 2, 1});
  Screen screen{2, 1};

  REQUIRE(surface.content_dirty());
  REQUIRE(surface.submission_count() == 0);
  surface.draw(screen);
  REQUIRE_FALSE(surface.dirty());
  REQUIRE(surface.content_dirty());
  (void)std::as_const(surface).pixels();
  CHECK_FALSE(surface.dirty());

  surface.pixels()[1] = Pixel{255, 255, 255, 255};
  REQUIRE(surface.dirty());
  REQUIRE(surface.content_dirty());
  surface.draw(screen);
  REQUIRE_FALSE(surface.dirty());

  surface.image().fill({0, 0, 1, 1}, Pixel{80, 80, 80, 255});
  REQUIRE(surface.dirty());
  surface.reset(Extent{3, 2}, Pixel{7, 8, 9, 255});
  CHECK(surface.extent() == Extent{3, 2});
  CHECK(std::as_const(surface).pixels().size() == 6);
  CHECK(std::as_const(surface).image().at(2, 1) == Pixel{7, 8, 9, 255});
}

TEST_CASE("PixelSurface empty inputs expose no region",
          "[pixelsurface][failure]") {
  PixelSurface empty{Extent{}};
  empty.set_geometry({0, 0, 4, 2});
  CHECK(empty.pixel_regions().empty());
  CHECK(empty.draw_pixels(empty.rect(), Extent{4, 4}) == nullptr);

  PixelSurface no_rect{Extent{2, 2}};
  CHECK(no_rect.pixel_regions().empty());
  no_rect.set_geometry({0, 0, 2, 2});
  CHECK(no_rect.draw_pixels({1, 0, 2, 2}, Extent{2, 2}) == nullptr);
}

TEST_CASE("Widget pixel fit is backward-compatible and surface-controlled",
          "[pixelsurface][fit]") {
  struct LegacyWidget final : Widget {
    auto draw(Screen&) -> void override {}
  } legacy;
  CHECK(legacy.pixel_fit({}) == PlacementFit::Stretch);

  PixelSurface surface{Extent{2, 1}};
  CHECK(surface.fit() == PlacementFit::Stretch);
  CHECK(surface.pixel_fit({}) == PlacementFit::Stretch);
  surface.set_fit(PlacementFit::Exact);
  CHECK(surface.fit() == PlacementFit::Exact);
  CHECK(surface.pixel_fit({}) == PlacementFit::Exact);
}

TEST_CASE("PixelSurface Stretch samples the complete cell destination",
          "[pixelsurface][fallback]") {
  PixelSurface surface{Extent{2, 1}, Pixel{128, 128, 128, 255}};
  surface.pixels()[1] = Pixel{255, 255, 255, 255};
  surface.set_geometry({0, 0, 4, 2});
  Screen screen{4, 2};

  surface.draw(screen);

  for (int y = 0; y < 2; ++y) {
    CHECK(text_at(screen, 0, y) == "=");
    CHECK(text_at(screen, 1, y) == "=");
    CHECK(text_at(screen, 2, y) == "@");
    CHECK(text_at(screen, 3, y) == "@");
  }
}

TEST_CASE("PixelSurface Exact keeps a one-pixel-per-cell map",
          "[pixelsurface][fallback][fit]") {
  PixelSurface surface{Extent{2, 1}, Pixel{128, 128, 128, 255}};
  surface.pixels()[1] = Pixel{255, 255, 255, 255};
  surface.set_fit(PlacementFit::Exact);
  surface.set_geometry({0, 0, 4, 2});
  Screen screen{4, 2};

  surface.draw(screen);

  CHECK(text_at(screen, 0, 0) == "=");
  CHECK(text_at(screen, 1, 0) == "@");
  CHECK(text_at(screen, 2, 0).empty());
  CHECK(text_at(screen, 3, 0).empty());
  for (int x = 0; x < 4; ++x) CHECK(text_at(screen, x, 1).empty());
}

TEST_CASE(
    "PixelSurface fallback composites alpha instead of showing hidden RGB",
    "[pixelsurface][fallback][alpha]") {
  PixelSurface surface{Extent{2, 1}, Pixel{255, 255, 255, 0}};
  surface.pixels()[1] = Pixel{255, 255, 255, 255};
  surface.set_geometry({0, 0, 2, 1});
  Screen screen{2, 1};

  surface.draw(screen);

  CHECK(text_at(screen, 0, 0) == " ");
  CHECK(text_at(screen, 1, 0) == "@");
}

TEST_CASE("App submits the fixed 320x180 image through Kitty once",
          "[pixelsurface][app][kitty]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.run_with(std::make_unique<KittyDriver>(), 2);

  CHECK(tfsupport::total_transmits(app.wire) == 1);
  CHECK(tfsupport::placements_of(app.wire, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
  CHECK_FALSE(app.surface.content_dirty());
  CHECK(app.wire.find("s=320,v=180") != std::string::npos);
  CHECK(app.wire.find("c=4,r=2") != std::string::npos);
}

TEST_CASE("300 clean PixelSurface frames retain one upload and placement",
          "[pixelsurface][app][kitty][persistent]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.run_with(std::make_unique<KittyDriver>(), 300);

  CHECK(tfsupport::transmits_of(app.wire, 272) == 1);
  CHECK(tfsupport::frame_updates_of(app.wire, 272) == 0);
  CHECK(tfsupport::placements_of(app.wire, 272) == 1);
  CHECK(tfsupport::data_deletes_of(app.wire, 272) == 0);
  CHECK(tfsupport::placement_deletes_of(app.wire, 272) == 0);
  CHECK(app.surface.submission_count() == 1);
}

TEST_CASE("320x180 dynamic App workload keeps one id and emits no clean-frame "
          "image bytes",
          "[pixelsurface][app][kitty][persistent][game]") {
  DynamicSurfaceApp app;
  app.run();

  // 62 frame writes plus the explicit shutdown handoff. Keeping the segments
  // separate is load-bearing: the clean-frame assertion is about the caller's
  // production frame boundary, not merely the aggregate stream.
  REQUIRE(app.sink.segments.size() == 63);
  std::string wire;
  for (std::size_t i = 0; i < 62; ++i) wire += app.sink.segments[i];

  CHECK(tfsupport::ids_named(wire) == std::set<std::uint32_t>{272});
  CHECK(tfsupport::transmits_of(wire, 272) == 1);
  CHECK(tfsupport::frame_updates_of(wire, 272) == 59);
  CHECK(tfsupport::placements_of(wire, 272) == 1);
  CHECK(tfsupport::data_deletes_of(wire, 272) == 0);
  CHECK(tfsupport::placement_deletes_of(wire, 272) == 0);
  CHECK(tfsupport::apcs(app.sink.segments[30]).empty());
  CHECK(tfsupport::apcs(app.sink.segments[60]).empty());
  CHECK(app.surface.submission_count() == 60);

  REQUIRE(app.observed_frames.size() == 61);
  for (std::size_t i = 0; i < app.observed_frames.size(); ++i) {
    CHECK(app.observed_frames[i].total() == app.sink.segments[i].size());
  }
  CHECK(app.observed_frames[30].image_transmit == 0);
  CHECK(app.observed_frames[30].image_edit == 0);
  CHECK(app.observed_frames[60].image_transmit == 0);
  CHECK(app.observed_frames[60].image_edit == 0);
  CHECK(app.sink.segments.back().find("a=d,d=A") != std::string::npos);
}

TEST_CASE("clean Unicode-placeholder frames do not repaint the retained grid",
          "[pixelsurface][app][kitty][persistent][unicode]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  auto driver = std::make_unique<KittyDriver>();
  driver->set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  app.run_with(std::move(driver), 30);

  CHECK(tfsupport::transmits_of(app.wire, 272) == 1);
  CHECK(tfsupport::placements_of(app.wire, 272) == 1);
  // One 4x2 grid. Re-emitting it on each clean frame would count 240.
  CHECK(tfsupport::count_of(app.wire, "\xF4\x8E\xBB\xAE") == 8);
  CHECK(app.surface.submission_count() == 1);
}

TEST_CASE("one invalidation after 300 clean frames replaces one root frame",
          "[pixelsurface][app][kitty][persistent]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.invalidate_on_frame = 300;
  app.run_with(std::make_unique<KittyDriver>(), 302);

  CHECK(tfsupport::transmits_of(app.wire, 272) == 1);
  CHECK(tfsupport::frame_updates_of(app.wire, 272) == 1);
  CHECK(tfsupport::placements_of(app.wire, 272) == 1);
  CHECK(tfsupport::data_deletes_of(app.wire, 272) == 0);
  CHECK(app.surface.submission_count() == 2);
  CHECK_FALSE(app.surface.content_dirty());
}

TEST_CASE("moving a clean PixelSurface changes placement without payload",
          "[pixelsurface][app][kitty][persistent][placement]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.move_on_frame = 100;
  app.run_with(std::make_unique<KittyDriver>(), 200);

  CHECK(tfsupport::total_data_transmits(app.wire) == 1);
  CHECK(tfsupport::placements_of(app.wire, 272) == 2);
  CHECK(tfsupport::placement_deletes_of(app.wire, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
}

TEST_CASE("reset recreates resident content instead of refusing its extent",
          "[pixelsurface][app][kitty][persistent][reset]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.reset_on_frame = 2;
  app.run_with(std::make_unique<KittyDriver>(), 4);

  CHECK(tfsupport::transmits_of(app.wire, 272) == 2);
  CHECK(tfsupport::frame_updates_of(app.wire, 272) == 0);
  CHECK(tfsupport::data_deletes_of(app.wire, 272) == 1);
  CHECK(app.errors == 0);
  CHECK(app.surface.submission_count() == 2);
  CHECK(app.surface.extent() == Extent{160, 90});
}

TEST_CASE("omitting a persistent region on a normal frame releases its data",
          "[pixelsurface][app][kitty][persistent][lifetime]") {
  SurfaceApp app;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{30, 80, 160, 255});
  app.stop_submitting_on_frame = 2;
  app.run_with(std::make_unique<KittyDriver>(), 4);

  CHECK(tfsupport::transmits_of(app.wire, 272) == 1);
  CHECK(tfsupport::data_deletes_of(app.wire, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
}

TEST_CASE("App sends PixelSurface to ANSI and keeps ASCII on Baseline",
          "[pixelsurface][app][ansi][fallback]") {
  SurfaceApp ansi;
  ansi.surface.image().fill({0, 0, 320, 180}, Pixel{220, 40, 80, 255});
  ansi.run_with(std::make_unique<AnsiRgbDriver>());
  CHECK(ansi.wire.find("\xE2\x96\x80") != std::string::npos);
  CHECK(ansi.wire.find("38;2;220;40;80") != std::string::npos);

  SurfaceApp baseline;
  baseline.surface.image().fill({0, 0, 320, 180}, Pixel{255, 255, 255, 255});
  baseline.run_with(std::make_unique<FallbackDriver>());
  CHECK(baseline.wire.find('@') != std::string::npos);
  CHECK(baseline.wire.find("\033_G") == std::string::npos);
  CHECK(baseline.wire.find("\xE2\x96\x80") == std::string::npos);
}

TEST_CASE("clean ANSI surfaces redraw only for placement invalidation",
          "[pixelsurface][app][ansi][persistent]") {
  SurfaceApp stable;
  stable.surface.image().fill({0, 0, 320, 180}, Pixel{220, 40, 80, 255});
  stable.run_with(std::make_unique<AnsiRgbDriver>(), 30);
  CHECK(tfsupport::count_of(stable.wire, "\xE2\x96\x80") == 8);
  CHECK(stable.surface.submission_count() == 1);

  SurfaceApp resized;
  resized.surface.image().fill({0, 0, 320, 180}, Pixel{220, 40, 80, 255});
  resized.resize_on_frame = 1;
  resized.run_with(std::make_unique<AnsiRgbDriver>(), 4);
  CHECK(tfsupport::count_of(resized.wire, "\xE2\x96\x80") == 16);
  CHECK(resized.surface.submission_count() == 1);
}

TEST_CASE("App reports a PixelSurface fit refusal once",
          "[pixelsurface][app][failure][fit]") {
  SurfaceApp app;
  app.surface.set_fit(PlacementFit::Exact);
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(tfsupport::total_transmits(app.wire) == 0);
  REQUIRE(app.errors == 1);
  CHECK(app.last_severity == Severity::Warning);
  CHECK(app.last_error.find("PlacementFit::Exact needs") != std::string::npos);
  CHECK(app.surface.submission_count() == 0);
  CHECK(app.surface.content_dirty());
}

TEST_CASE("PixelSurface suspension retains data and retires only placement",
          "[pixelsurface][app][kitty][overlay]") {
  SurfaceApp app;
  const Pixel* storage = std::as_const(app.surface).pixels().data();
  app.surface.image().fill({0, 0, 320, 180}, Pixel{40, 100, 180, 255});
  app.suspend_on_frame = 1;
  app.resume_on_frame = 2;
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(std::as_const(app.surface).pixels().data() == storage);
  CHECK(tfsupport::transmits_of(app.wire, 272) == 1);
  CHECK(tfsupport::data_deletes_of(app.wire, 272) == 0);
  CHECK(tfsupport::placement_deletes_of(app.wire, 272) == 1);
  CHECK(tfsupport::placements_of(app.wire, 272) == 2);
  CHECK(app.surface.submission_count() == 1);
}

TEST_CASE("a refused frame keeps PixelSurface dirty and retries its payload",
          "[pixelsurface][app][kitty][persistent][failure]") {
  SurfaceApp app;
  FailingOnceSink sink;
  app.output_override = &sink;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{40, 100, 180, 255});
  app.run_with(std::make_unique<KittyDriver>(), 2);

  REQUIRE(sink.writes == 3);  // two frames, then shutdown
  CHECK(app.errors == 1);
  CHECK(app.content_dirty_on_second_frame);
  CHECK(tfsupport::transmits_of(sink.accepted, 272) == 1);
  CHECK(tfsupport::placements_of(sink.accepted, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
  CHECK_FALSE(app.surface.content_dirty());
}

TEST_CASE("a refused clean frame does not manufacture new pixel content",
          "[pixelsurface][app][kitty][persistent][failure]") {
  SurfaceApp app;
  FailingOnceSink sink;
  sink.fail_on = 2;
  app.output_override = &sink;
  app.surface.image().fill({0, 0, 320, 180}, Pixel{40, 100, 180, 255});
  app.run_with(std::make_unique<KittyDriver>(), 3);

  REQUIRE(sink.writes == 4);  // three frames, then shutdown
  CHECK(app.errors == 1);
  CHECK(tfsupport::transmits_of(sink.accepted, 272) == 1);
  CHECK(tfsupport::frame_updates_of(sink.accepted, 272) == 0);
  CHECK(tfsupport::placements_of(sink.accepted, 272) == 1);
  CHECK(app.surface.submission_count() == 1);
  CHECK_FALSE(app.surface.content_dirty());
}
