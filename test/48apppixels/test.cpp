// TermForge — App's frame loop OVER THE PIXEL PATH (#189, #191).
//
// `test/47frameshape` asserts on the caller's call order by REPLAYING one
// complete frame as `draw...(); flush();`, because `App::test_wire_headless`
// once hardcoded a `FallbackDriver` whose
// `capabilities().kitty_graphics` is false, so `App::collect_pixel_regions`
// returned before the pixel path and `frame_step()` is private. A replay is one
// transcription away from being wrong: nothing failed if `App`'s real cadence
// changed underneath it.
//
// #189 added the seam — `test_run_frames(frames, cols, rows, sink, driver)` —
// and this suite is what it is for. Every case here drives a REAL `App`
// subclass through its REAL `frame_step()` over a REAL selected driver, and
// reads the wire. Kitty covers the out-of-band placement path; since #108 ANSI
// covers the in-band truecolour raster path. Nothing is replayed. If a case
// here can be written against a hand-made driver instead, it belongs in
// 47frameshape.
//
// #148 changes that production cadence: Renderer::present queues the cell diff,
// App queues pixel regions and on_pixels after it, and one flush carries the
// complete frame. This suite observes that real order, including the separate
// explicit shutdown write, through the App seam #189 added.
//
// The three loop seams (now_steady/wait_readable/read_available) are duplicated
// per suite by the convention stated at test/24tick/test.cpp:28-31: each test
// dir is its own executable, and hoisting would mean editing a landed suite to
// serve an unrelated one.
//
// Assertions parse the APC stream (test/support/apc.hpp) rather than grepping
// it: several properties here are ABSENCES, and `out.find("i=1")` is satisfied
// by `i=16`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/map_widget.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;
using tfsupport::apcs;
using tfsupport::data_deletes_of;
using tfsupport::frame_updates_of;
using tfsupport::ids_named;
using tfsupport::key_value;
using tfsupport::placement_deletes_of;
using tfsupport::placements;
using tfsupport::placements_of;
using tfsupport::total_data_transmits;
using tfsupport::total_transmits;
using tfsupport::transmits_of;

namespace {

// One region, one buffer, handed out unchanged forever: the shape #187's dedup
// exists for, and the shape a plate or a viewport has.
struct PlateWidget final : Widget {
  Rect region{0, 0, 4, 2};
  Image cache = tfsupport::checker(4, 4, Pixel{200, 30, 30, 255},
                                   Pixel{30, 30, 200, 255});
  bool present{true}; // false models draw_pixels returning nullptr
  PixelRegionState state{};
  ImagePlacementOptions placement{};
  int pixel_calls{0};
  Extent last_extent{};

  auto draw(Screen& screen) -> void override {
    // The Baseline presentation, and a SENTINEL rather than decoration: ANSI
    // and Kitty must blank it before their image pass, while Fallback must keep
    // it because that tier deliberately never calls draw_pixels (#108). QZJV
    // uses letters absent from ANSI's control stream, so its absence is a real
    // cell-blanking assertion rather than a raw-stream adjacency guess.
    screen.write_text(region.x, region.y, "QZJV", Rgb{220, 220, 220},
                      Rgb{10, 10, 10});
  }
  auto pixel_regions() -> std::vector<Rect> override { return {region}; }
  [[nodiscard]] auto pixel_region_state(Rect) const noexcept
      -> PixelRegionState override {
    return state;
  }
  auto pixel_region_submitted(Rect) noexcept -> void override {
    state.content_dirty = false;
  }
  [[nodiscard]] auto pixel_placement(Rect) const noexcept
      -> ImagePlacementOptions override {
    return placement;
  }
  auto draw_pixels(Rect, Extent pixels) -> const Image* override {
    ++pixel_calls;
    last_extent = pixels;
    return present ? &cache : nullptr;
  }
};

// A real App over a real KittyDriver. Renders one pixel-region widget; what a
// case varies is where (and whether) it ALSO draws a pinned image.
class PixelApp : public App {
 public:
  PlateWidget plate;
  std::vector<ErrorEvent> errors;

  auto on_render(Screen& s) -> void override {
    s.write_text(0, 4, "app", Rgb{0xE0, 0xE0, 0xF0}, Rgb{0x10, 0x10, 0x18});
    plate.draw(s);
    render_pixel_regions(plate);
  }

  auto run(int frames) -> void {
    test_run_frames(frames, 20, 8, &m_sink, std::make_unique<KittyDriver>());
  }
  auto run_unicode(int frames) -> void {
    auto driver = std::make_unique<KittyDriver>();
    driver->set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    test_run_frames(frames, 20, 8, &m_sink, std::move(driver));
  }
  auto run_ansi(int frames) -> void {
    test_run_frames(frames, 20, 8, &m_sink, std::make_unique<AnsiRgbDriver>());
  }
  // The three-argument overload, unchanged: what every landed suite calls.
  auto run_default(int frames) -> void {
    test_run_frames(frames, 20, 8, &m_sink);
  }
  [[nodiscard]] auto wire() const -> const std::string& { return m_sink; }
  auto clear_wire() -> void { m_sink.clear(); }
  // driver() is protected -- the same access path a consumer has, and the same
  // one test/37bytes' MeterProbe re-exports for the same reason.
  [[nodiscard]] auto caps() -> Capabilities { return driver().capabilities(); }
  [[nodiscard]] auto meter() -> FrameBytes { return m_frame_meter; }
  [[nodiscard]] auto cumulative() -> FrameBytes {
    return driver().total_bytes();
  }

 protected:
  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      errors.push_back(*error);
      return;
    }
    App::on_event(event);
  }

  // No sleeping and no fd: the clock only ever moves because wait_readable was
  // asked to wait, so a frame costs nothing and the suite is deterministic.
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    // frame_step calls this after the frame's single flush. Capture before
    // test_run_frames performs the separate end-of-session shutdown write.
    m_frame_meter = driver().last_frame_bytes();
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

  std::string m_sink;
  FrameBytes m_frame_meter{};

 private:
  std::chrono::steady_clock::time_point m_now{};
};

// The #191 subject: an app that ALSO owns a resident image and places it every
// frame. `where` is the whole experiment -- the same draw, in either of App's
// two windows.
enum class Window { OnRender, OnPixels };

class SpriteApp final : public PixelApp {
 public:
  explicit SpriteApp(Window where) : m_where(where) {}

  int pixel_calls{0};

  // Set before a run to model an app whose ONLY images are its own -- gloam's
  // shape: pinned plates and sprites, no widget pixel regions at all.
  bool sprite_only{false};

  auto on_render(Screen& s) -> void override {
    if (m_frame++ == dialog_on_frame) push_overlay(m_dialog);
    if (sprite_only) {
      s.write_text(0, 4, "app", Rgb{0xE0, 0xE0, 0xF0}, Rgb{0x10, 0x10, 0x18});
    } else {
      PixelApp::on_render(s);
    }
    if (m_where == Window::OnRender) place(driver());
  }
  auto on_pixels(TerminalDriver& d) -> void override {
    ++pixel_calls;
    if (m_where == Window::OnPixels) place(d);
  }

  // Pinned at the first draw rather than in on_start: test_run_frames drives
  // frame_step() only, so setup()/on_start() never run -- and the driver does
  // not exist before the harness wires it.
  [[nodiscard]] auto pin_id() const -> std::uint32_t { return m_pin.id; }
  [[nodiscard]] auto refusals() const -> int { return m_refusals; }

  // Pushed from inside the run rather than between runs: test_run_frames
  // builds a FRESH driver per call, so a pin taken in one call does not exist
  // in the next -- the overlay has to arrive mid-run or there is nothing left
  // for it to retire.
  int dialog_on_frame{-1};

 private:
  auto place(TerminalDriver& d) -> void {
    if (!m_pin) {
      // Counted, not REQUIREd: this runs inside frame_step, and a Catch2 macro
      // firing mid-frame aborts the case before its own assertion gets to say
      // what went wrong. A tier that cannot pin refuses honestly, and the case
      // that cares reads refusals().
      auto pinned =
          d.pin_image(tfsupport::solid(4, 4, Pixel{20, 220, 90, 255}));
      if (!pinned) {
        ++m_refusals;
        return;
      }
      m_pin = *pinned;
    }
    if (!d.draw_pinned(Rect{10, 1, 2, 2}, m_pin).has_value()) ++m_refusals;
  }

  Window m_where;
  int m_frame{0};
  PinnedImage m_pin{};
  int m_refusals{0};
  PlateWidget m_dialog; // a modal that draws nothing but occupies the stack
};

// #196's production shape: one logical framebuffer, one resident handle, new
// opaque bytes every frame. The frame data is intentionally tiny while its
// declared extent is the real 320x180 contract; PNG bytes are opaque to the
// library and borrowed only for replace_pinned's call.
class MutableSpriteApp final : public PixelApp {
 public:
  int pixel_calls{0};
  int refusals{0};
  [[nodiscard]] auto pin_id() const -> std::uint32_t { return m_pin.id; }

  auto on_pixels(TerminalDriver& d) -> void override {
    ++pixel_calls;
    const std::array<std::byte, 6> bytes{
        std::byte{0x89},
        std::byte{'P'},
        std::byte{'N'},
        std::byte{'G'},
        static_cast<std::byte>(m_frame & 0xFF),
        static_cast<std::byte>((m_frame >> 8) & 0xFF)};
    const EncodedImage frame{ImageFormat::Png, bytes, Extent{320, 180}};

    if (!m_pin) {
      const auto pinned = d.pin_image(frame);
      if (!pinned) {
        ++refusals;
        return;
      }
      m_pin = *pinned;
      m_reply_due = true;
      ++m_frame;
      return;
    } else if (!d.replace_pinned(m_pin, frame)) {
      ++refusals;
      return;
    }
    m_reply_due = true;
    if (!d.draw_pinned(Rect{1, 1, 18, 6}, m_pin)) ++refusals;
    ++m_frame;
  }

 protected:
  auto read_available(char* out, int max) -> int override {
    constexpr std::string_view reply{"\033_Gi=272;OK\033\\"};
    if (!m_reply_due || max < static_cast<int>(reply.size())) return 0;
    std::copy(reply.begin(), reply.end(), out);
    m_reply_due = false;
    return static_cast<int>(reply.size());
  }

 private:
  int m_frame{0};
  PinnedImage m_pin{};
  bool m_reply_due{false};
};

// The #108 counterpart to SpriteApp. Pinning is intentionally Kitty-only, so
// this app uses the raw Image overload every tier already implements and asks
// the question #108 owns: does App expose the AFTER-DIFF hook on ANSI too?
class AnsiHookApp final : public PixelApp {
 public:
  int pixel_calls{0};
  int refusals{0};

  auto on_pixels(TerminalDriver& d) -> void override {
    ++pixel_calls;
    if (!d.draw_image(Rect{6, 0, 2, 1}, m_image).has_value()) ++refusals;
  }

 private:
  Image m_image = tfsupport::checker(2, 2, Pixel{20, 220, 90, 255},
                                     Pixel{220, 90, 20, 255});
};

// Counts WRITES, which is the thing "a frame is two writes" is a claim about.
// A sink rather than a driver override: KittyDriver is final, and byte_sink.hpp
// states that emit_frame calls the sink exactly once per flush -- including for
// a frame that produced no bytes -- so a sink is the boundary instrument the
// library already documents. It still accumulates, so the wire is readable.
class WriteCounter final : public ByteSink {
 public:
  int writes{0};
  std::vector<std::string> segments; // one entry per write, in order
  std::string bytes;

  auto write(std::span<const char> b)
      -> std::expected<void, ErrorEvent> override {
    ++writes;
    segments.emplace_back(b.data(), b.size());
    bytes.append(b.data(), b.size());
    return {};
  }
};

class FailingMapSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++writes;
    if (writes == 1) {
      return std::unexpected{
          ErrorEvent{Severity::Warning, "sink", "map frame refused"}};
    }
    accepted.append(bytes.data(), bytes.size());
    return {};
  }

  int writes{0};
  std::string accepted;
};

// #167's production shape: an authored cell Baseline plus a pre-encoded asset
// whose storage stays owned by the widget. Encoded pixels take precedence over
// the raw hook; the latter is a sentinel that must never be reached while an
// encoded payload is present.
class EncodedPlateWidget final : public Widget {
 public:
  EncodedPlateWidget() { set_payload(ImageFormat::Png, Extent{4, 4}, 1); }

  Rect region{0, 0, 4, 2};
  PixelRegionState state{.mode = PixelRegionMode::Persistent};
  ImagePlacementOptions placement{};
  int encoded_calls{0};
  int raw_calls{0};
  int submissions{0};
  bool present{true};

  auto set_payload(ImageFormat format, Extent extent, std::uint8_t marker)
      -> void {
    const std::size_t size = format == ImageFormat::Rgba32
                                 ? static_cast<std::size_t>(extent.w) *
                                       static_cast<std::size_t>(extent.h) * 4
                                 : 7;
    m_bytes.assign(size, static_cast<std::byte>(marker));
    if (format == ImageFormat::Rgba32) {
      for (std::size_t i = 3; i < m_bytes.size(); i += 4)
        m_bytes[i] = std::byte{0xFF};
    }
    m_encoded = EncodedImage{format, m_bytes, extent};
    state.content_dirty = true;
    ++state.content_revision;
  }

  auto truncate_payload() -> void {
    if (!m_bytes.empty()) m_bytes.pop_back();
    m_encoded.bytes = m_bytes;
    state.content_dirty = true;
    ++state.content_revision;
  }

  auto draw(Screen& screen) -> void override {
    screen.write_text(region.x, region.y, "QZJV", Rgb{220, 220, 220},
                      Rgb{10, 10, 10});
  }
  auto pixel_regions() -> std::vector<Rect> override { return {region}; }
  [[nodiscard]] auto pixel_region_state(Rect) const noexcept
      -> PixelRegionState override {
    return state;
  }
  [[nodiscard]] auto pixel_placement(Rect) const noexcept
      -> ImagePlacementOptions override {
    return placement;
  }
  auto draw_encoded_pixels(Rect) -> const EncodedImage* override {
    ++encoded_calls;
    return present ? &m_encoded : nullptr;
  }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    ++raw_calls;
    return &m_raw;
  }
  auto pixel_region_submitted(Rect, std::uint64_t revision) noexcept
      -> void override {
    ++submissions;
    if (revision == state.content_revision) state.content_dirty = false;
  }

 private:
  std::vector<std::byte> m_bytes;
  EncodedImage m_encoded{};
  Image m_raw = tfsupport::solid(4, 4, Pixel{20, 220, 90, 255});
};

class EncodedPixelApp final : public App {
 public:
  EncodedPlateWidget plate;
  std::vector<ErrorEvent> errors;
  std::vector<std::pair<int, std::string>> replies;
  int mutate_on_frame{-1};
  int mutate_after_collect_on_frame{-1};
  int enable_encoded_on_frame{-1};
  ImageFormat mutated_format{ImageFormat::Png};
  Extent mutated_extent{4, 4};

  auto on_render(Screen& screen) -> void override {
    if (m_frame == enable_encoded_on_frame) {
      plate.present = true;
      plate.state.content_dirty = true;
      ++plate.state.content_revision;
    }
    if (m_frame == mutate_on_frame)
      plate.set_payload(mutated_format, mutated_extent, 2);
    plate.draw(screen);
    render_pixel_regions(plate);
    if (m_frame == mutate_after_collect_on_frame)
      plate.set_payload(mutated_format, mutated_extent, 2);
    ++m_frame;
  }

  auto run_with(std::unique_ptr<TerminalDriver> selected, int frames) -> void {
    test_run_frames(frames, 20, 8, &wire, std::move(selected));
  }

  std::string wire;

 protected:
  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      errors.push_back(*error);
      return;
    }
    App::on_event(event);
  }
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return std::ranges::any_of(
        replies, [&](const auto& reply) { return reply.first == m_frame; });
  }
  auto read_available(char* out, int max) -> int override {
    const auto reply =
        std::ranges::find_if(replies, [&](const auto& candidate) {
          return candidate.first == m_frame;
        });
    if (reply == replies.end() || max < static_cast<int>(reply->second.size()))
      return 0;
    std::copy(reply->second.begin(), reply->second.end(), out);
    const int size = static_cast<int>(reply->second.size());
    replies.erase(reply);
    return size;
  }

 private:
  int m_frame{0};
  std::chrono::steady_clock::time_point m_now{};
};

// A sink that counts the frame's writes. The redirect happens
// in the first on_render because that is the earliest App hook after the
// harness has wired a driver and before anything has been flushed.
class CountedApp final : public App {
 public:
  explicit CountedApp(bool graphics, bool image_frame = false)
      : m_graphics(graphics), m_image_frame(image_frame) {}
  WriteCounter sink;

  auto on_render(Screen& s) -> void override {
    driver().set_output(&sink);
    s.write_text(0, 0, "cells only", Rgb{0xE0, 0xE0, 0xF0},
                 Rgb{0x10, 0x10, 0x18});
    if (m_image_frame) render_pixel_regions(m_plate);
  }
  auto go(int frames) -> void {
    if (m_graphics) {
      test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
    } else {
      test_run_frames(frames, 20, 8, nullptr,
                      std::make_unique<FallbackDriver>());
    }
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
  bool m_graphics;
  bool m_image_frame;
  PlateWidget m_plate;
  std::chrono::steady_clock::time_point m_now{};
};

// #64's production shape: a layered map composites one atlas-backed viewport,
// retains it through clean frames, then changes content and placement on
// separate frames. This belongs here rather than test/29mapwidget because the
// assertion is about App's actual collect/flush/acknowledge cadence.
class MapSpriteApp final : public App {
 public:
  MapSpriteApp() {
    std::vector<Pixel> pixels(8, Pixel{220, 30, 30, 255});
    for (int y = 0; y < 2; ++y)
      for (int x = 2; x < 4; ++x)
        pixels[static_cast<std::size_t>(y) * 4 + x] = Pixel{30, 60, 220, 255};

    TileSet tiles;
    tiles.set_atlas(Image{4, 2, std::move(pixels)}, Extent{2, 2});
    tiles.define(1, TileDef{"RR", Rgb{220, 30, 30}, {}, Rect{0, 0, 2, 2}});
    tiles.define(2, TileDef{"BB", Rgb{30, 60, 220}, {}, Rect{2, 0, 2, 2}});
    map.set_tileset(std::move(tiles));
    map.set_map_size(2, 1);
    map.set_tile_size(2, 1);
    map.set_tile(0, 0, 0, 1);
    map.set_tile(0, 1, 0, 2);
  }

  MapWidget map;
  int mutate_on_frame{-1};
  int move_on_frame{-1};
  ByteSink* output_override{nullptr};
  bool content_dirty_on_second_frame{false};

  auto on_render(Screen& screen) -> void override {
    if (output_override != nullptr) driver().set_output(output_override);
    if (m_frame == 1)
      content_dirty_on_second_frame =
          map.pixel_region_state(map.rect()).content_dirty;
    if (m_frame == mutate_on_frame) map.set_tile(0, 0, 0, 2);
    if (m_frame == move_on_frame) m_x = 3;
    map.set_geometry({m_x, 1, 4, 1});
    map.draw(screen);
    render_pixel_regions(map);
    ++m_frame;
  }

  auto run_with(std::unique_ptr<TerminalDriver> selected, int frames) -> void {
    test_run_frames(frames, 12, 5, &wire, std::move(selected));
  }

  std::string wire;

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
  int m_x{0};
  std::chrono::steady_clock::time_point m_now{};
};

} // namespace

TEST_CASE("app pixels: the harness can put a graphics driver in App's loop",
          "[apppixels][kitty]") {
  // #189 itself. Before the seam this assertion could not be written at all:
  // test_wire_headless built a FallbackDriver, capabilities().kitty_graphics
  // was false, and collect_pixel_regions returned before the pixel path -- so
  // App's frame loop never reached a single image escape in any test.
  PixelApp app;
  app.run(1);

  REQUIRE(app.caps().kitty_graphics);
  CHECK(app.plate.pixel_calls == 1);
  CHECK(app.plate.last_extent == Extent{32, 32});
  CHECK(total_transmits(app.wire()) == 1);
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1});
}

TEST_CASE("app pixels: an encoded PNG takes precedence on Kitty",
          "[apppixels][encoded][kitty][issue167]") {
  EncodedPixelApp app;
  app.plate.state.mode = PixelRegionMode::Immediate;
  app.run_with(std::make_unique<KittyDriver>(), 1);

  CHECK(app.plate.encoded_calls == 1);
  CHECK(app.plate.raw_calls == 0);
  CHECK(total_data_transmits(app.wire) == 1);
  const auto commands = apcs(app.wire);
  const auto transmission = std::ranges::find_if(
      commands, [](const auto& c) { return key_value(c, "a") == "t"; });
  REQUIRE(transmission != commands.end());
  CHECK(key_value(*transmission, "f") == "100");
}

TEST_CASE("app pixels: raw encoded bytes reach ANSI without the Image hook",
          "[apppixels][encoded][ansi][issue167]") {
  EncodedPixelApp app;
  app.plate.state.mode = PixelRegionMode::Immediate;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 7);
  app.run_with(std::make_unique<AnsiRgbDriver>(), 1);

  CHECK(app.plate.encoded_calls == 1);
  CHECK(app.plate.raw_calls == 0);
  CHECK(app.wire.find("\xE2\x96\x80") != std::string::npos);
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: unsupported encoded formats retain the Baseline once",
          "[apppixels][encoded][ansi][fallback][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32Zlib, Extent{4, 4}, 7);
  app.run_with(std::make_unique<AnsiRgbDriver>(), 3);

  CHECK(app.plate.raw_calls == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Info);
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire);
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
}

TEST_CASE("app pixels: malformed raw encoded bytes retain the Baseline once",
          "[apppixels][encoded][failure][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 7);
  app.plate.truncate_payload();
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.raw_calls == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Warning);
  CHECK(total_data_transmits(app.wire) == 0);
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire);
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
}

TEST_CASE("app pixels: an opaque persistent root waits for terminal OK",
          "[apppixels][encoded][persistent][reply][issue167]") {
  EncodedPixelApp app;
  app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.encoded_calls == 1);
  CHECK(app.plate.raw_calls == 0);
  CHECK(app.plate.submissions == 1);
  CHECK(total_data_transmits(app.wire) == 1);
  CHECK(placements_of(app.wire, 272) == 1);
  CHECK(app.wire.find("QZJV") != std::string::npos);
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: a rejected opaque root retries before acknowledgement",
          "[apppixels][encoded][persistent][reply][failure][issue167]") {
  EncodedPixelApp app;
  app.replies.emplace_back(1, "\033_Gi=272;EINVAL\033\\");
  app.replies.emplace_back(2, "\033_Gi=272;OK\033\\");
  app.run_with(std::make_unique<KittyDriver>(), 4);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 1);
  CHECK(total_data_transmits(app.wire) == 2);
  CHECK(placements_of(app.wire, 272) == 1);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Warning);
}

TEST_CASE("app pixels: accepted opaque replacement acknowledges once",
          "[apppixels][encoded][persistent][replace][issue167]") {
  EncodedPixelApp app;
  app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
  app.replies.emplace_back(3, "\033_Gi=272;OK\033\\");
  app.mutate_after_collect_on_frame = 1;
  app.run_with(std::make_unique<KittyDriver>(), 5);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 2);
  CHECK(transmits_of(app.wire, 272) == 1);
  CHECK(frame_updates_of(app.wire, 272) == 1);
  CHECK(placements_of(app.wire, 272) == 1);
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: rejected opaque replacement keeps and retries its root",
          "[apppixels][encoded][persistent][replace][failure][issue167]") {
  EncodedPixelApp app;
  app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
  app.replies.emplace_back(3, "\033_Gi=272;EINVAL\033\\");
  app.replies.emplace_back(4, "\033_Gi=272;OK\033\\");
  app.mutate_after_collect_on_frame = 1;
  app.run_with(std::make_unique<KittyDriver>(), 5);

  CHECK(app.plate.encoded_calls == 3);
  CHECK(app.plate.submissions == 2);
  CHECK(transmits_of(app.wire, 272) == 1);
  CHECK(frame_updates_of(app.wire, 272) == 2);
  CHECK(data_deletes_of(app.wire, 272) == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Warning);
}

TEST_CASE("app pixels: a late OK cannot clear a newer widget generation",
          "[apppixels][encoded][persistent][reply][generation][issue167]") {
  EncodedPixelApp app;
  app.replies.emplace_back(1, "\033_Gi=272;OK\033\\");
  app.replies.emplace_back(2, "\033_Gi=272;OK\033\\");
  app.mutate_on_frame = 1;
  app.run_with(std::make_unique<KittyDriver>(), 4);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 1);
  CHECK_FALSE(app.plate.state.content_dirty);
  CHECK(transmits_of(app.wire, 272) == 1);
  CHECK(frame_updates_of(app.wire, 272) == 1);
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: same-format raw updates reuse their resident identity",
          "[apppixels][encoded][persistent][replace][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 1);
  app.mutate_on_frame = 1;
  app.mutated_format = ImageFormat::Rgba32;
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 2);
  CHECK(transmits_of(app.wire, 272) == 1);
  CHECK(frame_updates_of(app.wire, 272) == 1);
  CHECK(data_deletes_of(app.wire, 272) == 0);
}

TEST_CASE("app pixels: encoded extent changes recreate resident identity",
          "[apppixels][encoded][persistent][identity][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 1);
  app.mutate_on_frame = 1;
  app.mutated_format = ImageFormat::Rgba32;
  app.mutated_extent = Extent{2, 8};
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 2);
  CHECK(total_data_transmits(app.wire) == 2);
  CHECK(frame_updates_of(app.wire, 272) == 0);
  CHECK(data_deletes_of(app.wire, 272) == 1);
}

TEST_CASE("app pixels: raw-to-encoded changes recreate resident identity",
          "[apppixels][encoded][persistent][identity][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 1);
  app.plate.present = false;
  app.enable_encoded_on_frame = 1;
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.raw_calls == 1);
  CHECK(app.plate.submissions == 2);
  CHECK(transmits_of(app.wire, 272) == 2);
  CHECK(frame_updates_of(app.wire, 272) == 0);
  CHECK(data_deletes_of(app.wire, 272) == 1);
}

TEST_CASE("app pixels: encoded format changes recreate resident identity",
          "[apppixels][encoded][persistent][identity][issue167]") {
  EncodedPixelApp app;
  app.plate.set_payload(ImageFormat::Rgba32, Extent{4, 4}, 1);
  app.mutate_on_frame = 1;
  app.mutated_format = ImageFormat::Png;
  app.replies.emplace_back(2, "\033_Gi=272;OK\033\\");
  app.run_with(std::make_unique<KittyDriver>(), 3);

  CHECK(app.plate.encoded_calls == 2);
  CHECK(app.plate.submissions == 2);
  CHECK(transmits_of(app.wire, 272) == 2);
  CHECK(frame_updates_of(app.wire, 272) == 0);
  CHECK(data_deletes_of(app.wire, 272) == 1);
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: a widget layer reaches Kitty's production image pass",
          "[apppixels][kitty][layers][issue114]") {
  PixelApp app;
  app.plate.placement.layer = ImageLayer::below_text();
  app.run(1);

  CHECK(app.plate.pixel_calls == 1);
  const auto placed = placements(app.wire());
  REQUIRE(placed.size() == 1);
  CHECK(key_value(placed[0], "z") == "-1");
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: unsupported layers keep Baseline and report once",
          "[apppixels][ansi][layers][fallback][issue114]") {
  PixelApp app;
  app.plate.placement.layer = ImageLayer::below_text();
  app.run_ansi(4);

  CHECK(app.plate.pixel_calls == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Info);
  CHECK(app.errors[0].source == "app");
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(app.wire().find("\xE2\x96\x80") == std::string::npos);
}

TEST_CASE("app pixels: placement geometry reaches Kitty's image pass",
          "[apppixels][kitty][geometry][issue115]") {
  PixelApp app;
  app.plate.placement.pixel_offset = PixelPoint{3, 4};
  app.plate.placement.source = PixelRect{1, 0, 3, 4};
  app.run(1);

  CHECK(app.plate.pixel_calls == 1);
  const auto placed = placements(app.wire());
  REQUIRE(placed.size() == 1);
  CHECK(key_value(placed[0], "X") == "3");
  CHECK(key_value(placed[0], "Y") == "4");
  CHECK(key_value(placed[0], "x") == "1");
  CHECK(key_value(placed[0], "y") == "0");
  CHECK(key_value(placed[0], "w") == "3");
  CHECK(key_value(placed[0], "h") == "4");
  CHECK(app.errors.empty());
}

TEST_CASE("app pixels: invalid geometry keeps Baseline and warns once",
          "[apppixels][kitty][geometry][fallback][issue115]") {
  PixelApp app;
  app.plate.placement.source = PixelRect{3, 3, 2, 2};
  app.run(4);

  CHECK(app.plate.pixel_calls == 4);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Warning);
  CHECK(app.errors[0].source == "kitty");
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(total_data_transmits(app.wire()) == 0);
}

TEST_CASE("app pixels: unsupported geometry keeps ANSI Baseline",
          "[apppixels][ansi][geometry][fallback][issue115]") {
  PixelApp app;
  app.plate.placement.pixel_offset = PixelPoint{1, 0};
  app.run_ansi(3);

  CHECK(app.plate.pixel_calls == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Info);
  CHECK(app.errors[0].source == "app");
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(app.wire().find("\xE2\x96\x80") == std::string::npos);
}

TEST_CASE("app pixels: Unicode geometry keeps Baseline instead of full atlas",
          "[apppixels][kitty][placeholders][geometry][fallback][issue115]") {
  PixelApp app;
  app.plate.placement.pixel_offset = PixelPoint{2, 3};
  app.plate.placement.source = PixelRect{2, 0, 2, 4};
  app.run_unicode(3);

  CHECK(app.plate.pixel_calls == 0);
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].severity == Severity::Info);
  CHECK(app.errors[0].source == "app");
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(total_data_transmits(app.wire()) == 0);
  CHECK(app.wire().find("\xF4\x8E\xBB\xAE") == std::string::npos);
}

TEST_CASE("app pixels: a persistent layer move does not rebuild content",
          "[apppixels][kitty][persistent][layers][issue114]") {
  class LayerMovingApp final : public PixelApp {
   public:
    auto on_render(Screen& screen) -> void override {
      plate.state.mode = PixelRegionMode::Persistent;
      plate.placement.layer =
          m_frame == 0 ? ImageLayer{} : ImageLayer::below_text();
      PixelApp::on_render(screen);
      ++m_frame;
    }

   private:
    int m_frame{0};
  } app;

  app.run(3);

  CHECK(app.plate.pixel_calls == 1);
  CHECK(total_data_transmits(app.wire()) == 1);
  const auto placed = placements(app.wire());
  REQUIRE(placed.size() == 2);
  const auto image_id =
      static_cast<std::uint32_t>(std::stoul(key_value(placed[0], "i")));
  CHECK(placements_of(app.wire(), image_id) == 2);
  CHECK(placement_deletes_of(app.wire(), image_id) == 1);
  CHECK_FALSE(tfsupport::has_key(placed[0], "z"));
  CHECK(key_value(placed[1], "z") == "-1");
}

TEST_CASE("app pixels: ANSI receives the raster and blanks its cell fallback",
          "[apppixels][ansi][issue108]") {
  // The gate #108 opens. AnsiRgbDriver is deliberately NOT kitty, so an APC
  // count cannot prove this arm; the widget call, the driver-requested extent,
  // and the half-block bytes close the path from App to the real driver.
  PixelApp app;
  app.run_ansi(1);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  REQUIRE(app.caps().truecolor);
  CHECK(app.plate.pixel_calls == 1);
  CHECK(app.plate.last_extent == Extent{4, 4});
  CHECK(app.wire().find('Q') == std::string::npos);
  CHECK(app.wire().find('Z') == std::string::npos);
  CHECK(app.wire().find('J') == std::string::npos);
  CHECK(app.wire().find('V') == std::string::npos);
  CHECK(app.wire().find("\xE2\x96\x80") != std::string::npos);
  CHECK(app.wire().find("38;2;200;30;30") != std::string::npos);
  CHECK(app.wire().find("48;2;30;30;200") != std::string::npos);
}

TEST_CASE("app pixels: ANSI restores cells for a temporarily absent raster",
          "[apppixels][ansi][issue108]") {
  // The reason ANSI joins the existing cell-blanking path, observed across
  // production frames rather than inferred from one frame's bytes. Frame 0
  // paints the raster over blank Screen cells; frame 1 gets nullptr from the
  // widget and must repaint its authored QZJV fallback; frame 2 blanks those
  // cells again before repainting the raster. Omitting the blanking step makes
  // frame 0 leak QZJV beneath its raster, while blanking on the gap frame makes
  // that frame's four positive assertions fail.
  class AnsiBlinker final : public PixelApp {
   public:
    WriteCounter sink;

    auto on_render(Screen& s) -> void override {
      driver().set_output(&sink);
      plate.present = (m_frame++ != 1);
      PixelApp::on_render(s);
    }
    auto go() -> void {
      test_run_frames(3, 20, 8, nullptr, std::make_unique<AnsiRgbDriver>());
    }

   private:
    int m_frame{0};
  } app;
  app.go();

  REQUIRE(app.sink.segments.size() == 3);
  const auto& first = app.sink.segments[0];
  const auto& gap = app.sink.segments[1];
  const auto& third = app.sink.segments[2];

  CHECK(first.find("\xE2\x96\x80") != std::string::npos);
  CHECK(first.find('Q') == std::string::npos);
  // The first draw_text resets the colors left by the preceding image, so its
  // SGR bytes legitimately sit between CUP and Q. Assert destination and
  // content independently instead of coupling this test to that cache state.
  CHECK(gap.find("\033[1;1H") != std::string::npos);
  CHECK(gap.find('Q') != std::string::npos);
  CHECK(gap.find('Z') != std::string::npos);
  CHECK(gap.find('J') != std::string::npos);
  CHECK(gap.find('V') != std::string::npos);
  CHECK(gap.find("\xE2\x96\x80") == std::string::npos);
  CHECK(third.find("\xE2\x96\x80") != std::string::npos);
  CHECK(third.find('Q') == std::string::npos);
  CHECK(third.find('Z') == std::string::npos);
  CHECK(third.find('J') == std::string::npos);
  CHECK(third.find('V') == std::string::npos);
}

TEST_CASE("app pixels: the DEFAULT tier keeps cells and reaches no pixel",
          "[apppixels]") {
  // The Baseline control for #108. FallbackDriver CAN turn Image pixels into a
  // luminance ramp for a direct caller; App deliberately does not ask it to,
  // because the widget's authored cells carry information a ramp cannot infer.
  // Adding `|| true` or treating every draw_image implementation as enhanced
  // makes both the call count and the visible QZJV sentinel fail.
  PixelApp app;
  app.run_default(4);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  REQUIRE_FALSE(app.caps().truecolor);
  CHECK(app.plate.pixel_calls == 0);
  // Observe the terminal grid, not one particular cursor spelling: #89 may
  // omit redundant CUPs inside this adjacent run without changing a cell.
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(app.wire().find("\xE2\x96\x80") == std::string::npos);
  CHECK(total_transmits(app.wire()) == 0);
  CHECK(ids_named(app.wire()).empty());
  CHECK(app.wire().find("\033_G") == std::string::npos);
}

TEST_CASE("app pixels: an unchanged region transmits ONCE across many frames",
          "[apppixels][kitty]") {
  // #187's headline property, OBSERVED through App's own frame_step rather
  // than replayed by hand. test/47frameshape asserts the same number against a
  // transcribed cadence; this one asserts it against the cadence itself, so a
  // change to App's frame shape fails here rather than in a consumer's byte
  // budget.
  //
  // 24 frames because the pre-#187 defect was one transmit and one fresh id
  // PER FRAME: at 24 the two hypotheses are 1 vs 24, not 1 vs 2.
  PixelApp app;
  app.run(24);

  CHECK(total_transmits(app.wire()) == 1);
  CHECK(data_deletes_of(app.wire(), 1) == 0);
  CHECK(placements_of(app.wire(), 1) == 1);
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1});
}

TEST_CASE("app pixels: moving placeholders clear the terminal cell grid",
          "[apppixels][kitty][placeholders]") {
  // #201's production-cadence acceptance. APC counts cannot see this bug: the
  // old image is deleted correctly while its U+10EEEE cells remain outside
  // Screen, and #190 later reuses their id. Read the terminal grid itself.
  class MovingRegionApp final : public PixelApp {
   public:
    WriteCounter sink;

    auto on_render(Screen&) -> void override {
      driver().set_output(&sink);
      if (!m_mode_set) {
        static_cast<KittyDriver&>(driver()).set_placement_mode(
            KittyDriver::PlacementMode::UnicodePlaceholders);
        m_mode_set = true;
      }
      plate.region = Rect{m_frame * 3, 1, 2, 1};
      render_pixel_regions(plate);
      ++m_frame;
    }
    auto go() -> void {
      test_run_frames(3, 12, 4, nullptr, std::make_unique<KittyDriver>());
    }

   private:
    int m_frame{0};
    bool m_mode_set{false};
  } app;
  app.go();

  REQUIRE(app.sink.segments.size() == 4); // 3 frames + shutdown
  tfsupport::TerminalGrid grid{12, 4};
  for (int frame = 0; frame < 3; ++frame)
    grid.feed(app.sink.segments[frame]);

  // Id 1 was reused at x=6. Without the cleanup, its old x=0 grid resolves to
  // the new image and this first assertion sees two placeholders, not blanks.
  CHECK_FALSE(grid.at(0, 1).placeholder());
  CHECK_FALSE(grid.at(1, 1).placeholder());
  CHECK_FALSE(grid.at(3, 1).placeholder());
  CHECK_FALSE(grid.at(4, 1).placeholder());
  CHECK(grid.at(6, 1).placeholder());
  CHECK(grid.at(7, 1).placeholder());
}

TEST_CASE(
    "app pixels: App's frame is ONE write carrying the whole frame (#148)",
    "[apppixels][kitty]") {
  // Before #148 a frame was two writes -- the cell diff, then the images --
  // and collection had to infer frame boundaries. Since #148 the frame is ONE
  // write: images queue
  // after the cell diff in the driver's buffer and Renderer::flush() emits
  // them together, so the collection's frame boundary is exact by construction.
  //
  // Read off the meter rather than counted by hand: emit_frame is the write
  // AND meter boundary, so last_frame_bytes() after a frame_step now reports
  // the WHOLE frame -- image traffic AND the cell diff together -- where it
  // used to report only the second (image) write. A frame carrying a new
  // plate has a nonzero image_transmit AND nonzero cells in its one frame.
  PixelApp app;
  app.run(1);

  const FrameBytes last = app.meter();
  CHECK(last.image_transmit > 0);
  CHECK(last.cells > 0); // the cell diff is in the SAME single write now
  // One frame, one write: the whole frame's cost is the last frame, and the
  // cumulative total equals everything on the wire (which also carries the
  // trailing d=A the shutdown emits through the same sink).
  CHECK(app.cumulative().total() == app.wire().size());
}

// ── #191: the application image window ─────────────────────────────────────

TEST_CASE("app pixels: a sprite drawn from on_pixels is placed ONCE (#191)",
          "[apppixels][kitty][pinned]") {
  // The acceptance test for #191, and the first proof that #109 has a correct
  // App call site at all.
  //
  // One resident image and one pixel region, both drawn every frame after the
  // cell diff. The frame has one write and therefore one exact collection
  // boundary: the plate uploads once, the placement is created once and never
  // retired, and two ids exist for all time.
  SpriteApp app{Window::OnPixels};
  app.run(10);

  REQUIRE(app.pixel_calls == 10);
  REQUIRE(app.refusals() == 0);
  // Two uploads for all ten frames, and both are first-and-only: the plate
  // once, and the pinned payload once at pin time (which is what pinning IS).
  CHECK(transmits_of(app.wire(), 1) == 1);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(total_transmits(app.wire()) == 2);
  CHECK(ids_named(app.wire()).size() == 2); // the plate and the pin
  CHECK(data_deletes_of(app.wire(), 1) == 0);
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
}

TEST_CASE(
    "app pixels: mutable resident frames keep one id and placement (#196)",
    "[apppixels][kitty][pinned][replacement]") {
  MutableSpriteApp app;
  app.run(60);

  REQUIRE(app.pixel_calls == 60);
  REQUIRE(app.refusals == 0);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(frame_updates_of(app.wire(), app.pin_id()) == 59);
  // The widget plate is the other ordinary transmission in this production
  // run; every mutable-frame payload is still under the one pinned id.
  CHECK(total_data_transmits(app.wire()) == 61);
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);
  CHECK(data_deletes_of(app.wire(), app.pin_id()) == 0);
}

TEST_CASE("app pixels: an app whose ONLY images come from on_pixels is written",
          "[apppixels][kitty][pinned]") {
  // The shape that makes the trailing flush unconditional rather than
  // conditional on there being pixel regions, and the one a downstream
  // compositor actually has: every image is the app's own, so
  // m_pixel_regions is empty for the whole run.
  //
  // Under the old `if (!m_pixel_regions.empty())` gate these bytes are written
  // by the NEXT frame's present() instead -- a placement a frame late, and a
  // first flush that is no longer drawless, so the collection stops landing on
  // a frame boundary. Nothing here is about how much is drawn; it is about
  // whether the frame that drew it also wrote it.
  SpriteApp app{Window::OnPixels};
  app.sprite_only = true;
  app.run(6);

  REQUIRE(app.pixel_calls == 6);
  CHECK(total_transmits(app.wire()) == 1);             // the pin, once
  CHECK(placements_of(app.wire(), app.pin_id()) == 1); // and placed once
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);
  // The placement is in the frame that drew it, which is the whole claim.
  SpriteApp one{Window::OnPixels};
  one.sprite_only = true;
  one.run(1);
  CHECK(placements_of(one.wire(), one.pin_id()) == 1);
}

TEST_CASE(
    "app pixels: the same sprite drawn from on_render no longer blinks (#148)",
    "[apppixels][kitty][pinned]") {
  // The pre-#148 negative control, now OBSOLETE by construction -- and pinning
  // that is the point. Before #148 a frame was two writes split by present()'s
  // flush: draw_pinned from on_render landed in the first, App's regions in the
  // second, BOTH flushes had drawn something, #187's guard never fired, and
  // each collection destroyed what the other window drew -- ten uploads and
  // nine deletes for ten frames of one unchanged plate, the sprite blinking
  // off. #191's on_pixels let an app dodge that.
  //
  // #148 merged the frame into ONE image window and ONE write, so the split
  // that made on_render's draw fight the regions' cannot occur: every draw of
  // the frame goes out in the same flush and the collection reads the frame
  // boundary exactly. Drawing from on_render now behaves like on_pixels: the
  // plate uploads once, its placement is created once and never retired. Kept
  // (not deleted) so the change is asserted -- a regression that reintroduces
  // the split frame turns these numbers back to the #191 ones (transmits 11,
  // deleted 9, placement_deletes 10, placements 10).
  SpriteApp app{Window::OnRender};
  app.run(10);

  REQUIRE(app.pixel_calls == 10); // called, and deliberately does nothing
  // ONE upload of the plate plus the pin's at pin time -- not eleven.
  CHECK(total_transmits(app.wire()) == 2);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(transmits_of(app.wire(), 1) == 1); // the plate, once
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1, app.pin_id()});
  int deleted = 0;
  for (const std::uint32_t id : ids_named(app.wire()))
    deleted += data_deletes_of(app.wire(), id);
  CHECK(deleted == 0); // no per-frame d=I (was 9)
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0); // (was 10)
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);        // (was 10)
}

TEST_CASE("app pixels: on_pixels draws AFTER App's own regions",
          "[apppixels][kitty][pinned]") {
  // The EMISSION order, as a byte fact. Deliberately not a claim about what
  // the terminal composites: two placements at the same z are ordered by the
  // terminal and termforge does not specify that tie-break (#114 is where a
  // named layer would go). What this order really decides is who loses a
  // same-rect collision under UnicodePlaceholders -- App's region stamps the
  // rect first, so the draw_pinned is the one refused -- and that is a
  // behaviour a caller can observe, so the order has to be pinned or it
  // drifts.
  SpriteApp app{Window::OnPixels};
  app.run(1);

  const auto cmds = tfsupport::apcs(app.wire());
  std::size_t region_at = cmds.size();
  std::size_t sprite_at = cmds.size();
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const std::string id = tfsupport::key_value(cmds[i], "i");
    if (id == "1" && region_at == cmds.size()) region_at = i;
    if (id == std::to_string(app.pin_id())) sprite_at = i;
  }
  REQUIRE(region_at < cmds.size());
  REQUIRE(sprite_at < cmds.size());
  CHECK(region_at < sprite_at);
}

TEST_CASE("app pixels: a graphics frame is exactly ONE write, always (#148)",
          "[apppixels][kitty]") {
  // The cadence itself, counted rather than assumed. Before #148 a graphics
  // frame was TWO writes (the cell diff, then the images). Since #148 the frame
  // is ONE write on every tier -- images queue after the cell diff in the
  // driver buffer -- so the count is the
  // same whether or not the frame drew anything, making collection exact.
  //
  // Both arms below run frames with NO image at all -- precisely the shape
  // that used to expose the conditional second flush.
  SECTION("a graphics tier writes once per frame") {
    CountedApp app{true};
    app.go(6);
    // Six frames, one write each. No image was transmitted, so shutdown owes
    // no d=A and emits no seventh write.
    CHECK(app.sink.writes == 6); // was 12
  }

  SECTION("a non-graphics tier also writes once") {
    // The other half: a fallback tier has no image window, so nothing can draw
    // a second write there on either tier model -- and now no tier writes
    // twice, so the two arms agree at the frame level. The graphics arm's
    // teardown d=A has no counterpart here, so this arm stays at 6.
    // test/37bytes' MeterProbe is the case that notices if this changes.
    CountedApp app{false};
    app.go(6);
    CHECK(app.sink.writes == 6);
  }

  SECTION("an image-transmit frame is one write, plus explicit shutdown") {
    CountedApp app{true, true};
    app.go(1);
    REQUIRE(app.sink.segments.size() == 2);
    CHECK(app.sink.segments[0].find("a=t") != std::string::npos);
    CHECK(app.sink.segments[0].find("a=d,d=A") == std::string::npos);
    CHECK(app.sink.segments[1].find("a=d,d=A") != std::string::npos);
  }
}

TEST_CASE("app pixels: on_pixels runs in ANSI's after-diff image window",
          "[apppixels][ansi][issue108]") {
  // The second half of #108: application-owned images and widget regions share
  // one capability scope. The direct draw uses colors absent from PlateWidget,
  // so finding them proves on_pixels reached AnsiRgbDriver rather than merely
  // proving the widget region above rendered.
  AnsiHookApp app;
  app.run_ansi(1);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  REQUIRE(app.caps().truecolor);
  CHECK(app.pixel_calls == 1);
  CHECK(app.refusals == 0);
  CHECK(app.wire().find("38;2;20;220;90") != std::string::npos);
  CHECK(app.wire().find("48;2;220;90;20") != std::string::npos);
}

TEST_CASE("app pixels: on_pixels stays outside the Baseline capability scope",
          "[apppixels][issue108]") {
  // The unchanged control. FallbackDriver can render an image as ASCII for a
  // direct caller, but App preserves the authored cell path and never opens
  // either enhanced image hook on that tier.
  SpriteApp app{Window::OnPixels};
  app.run_default(5);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  REQUIRE_FALSE(app.caps().truecolor);
  CHECK(app.pixel_calls == 0);
  CHECK(app.plate.pixel_calls == 0);
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(app.wire());
  CHECK(grid.row_text(0).substr(0, 4) == "QZJV");
  CHECK(app.wire().find("\033_G") == std::string::npos);
}

TEST_CASE("app pixels: an overlay suppresses on_pixels, and retires the sprite",
          "[apppixels][kitty][pinned]") {
  // Same answer as render_pixel_regions for a different reason. There are no
  // Screen cells to blank here, but images are emitted after the cell diff, so
  // a sprite drawn while a dialog is up paints straight through it -- and an
  // app that draws through both paths would otherwise keep half its images
  // above the dialog and lose the other half. Only the topmost thing puts
  // pixels on screen.
  //
  // The retirement is the part worth having as a number: the frame that opens
  // the dialog draws no image at all, so its single flush is the frame's own
  // boundary and the placement is retired there -- with the dialog, not one
  // frame behind it.
  SpriteApp app{Window::OnPixels};
  app.dialog_on_frame = 3; // frames 0-2 draw the sprite, 3 and 4 do not
  app.run(5);

  CHECK(app.pixel_calls == 3); // three frames' worth, then suppressed
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
  // Retired in the frame the dialog opened on, not one behind it: that frame
  // draws no image, so its (single, since #148) flush is the frame's own
  // boundary and the collection runs there.
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 1);
}

TEST_CASE("a frame that draws no region collects on ITS OWN boundary (#148)",
          "[apppixels][kitty]") {
  // A widget whose draw_pixels returns nullptr for one frame -- WaveformWidget
  // does this whenever its sample buffer is empty -- makes that frame drawless.
  // Before #148 a frame was two writes, and a drawless frame's collection could
  // land on the NEXT frame's boundary instead of its own. Since #148 the frame
  // is ONE write, so the drawless frame collects on its own flush: the removed
  // region's delete goes out in that same frame's single write, not the next
  // one's. The cost is unchanged and stated: a full re-upload, which is why
  // pin_image and not this is the answer for content an app keeps. Per-WRITE,
  // not per-run: only the segment the delete lands in distinguishes the
  // boundary, which is why this case needs a sink.
  class Blinker final : public PixelApp {
   public:
    WriteCounter sink;
    int gap_frame{-1};

    auto on_render(Screen& s) -> void override {
      driver().set_output(&sink);
      plate.present = (m_frame++ != gap_frame);
      PixelApp::on_render(s);
    }
    auto go(int frames) -> void {
      test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
    }

   private:
    int m_frame{0};
  } app;
  app.gap_frame = 2;
  app.go(5);

  // One write per frame (#148), so frame N owns segment N; the 6th is the
  // teardown d=A, which is byte-identical to a frame's collection but is not
  // one -- segments 1..4 are gap-adjacent, and 5 is where it lands.
  REQUIRE(app.sink.writes == 6);
  const auto& seg = app.sink.segments;
  CHECK(data_deletes_of(seg[2], 1) == 1); // the gap frame's OWN single write
  CHECK(data_deletes_of(seg[3], 1) == 0); // and not the next frame's
  // The trailing d=A is shutdown cleanup, not a per-id region deletion.
  CHECK(seg[5].find("a=d,d=A") != std::string::npos);
  // The cost, unchanged and stated: a full re-upload. The id is no longer part
  // of it (#190) -- the gap frame's collection emptied the map, so the region
  // comes back under the id it just gave up, and this whole run names exactly
  // one. The two transmits are the damage; the single id is what #190 removed.
  CHECK(data_deletes_of(app.sink.bytes, 1) == 1);
  CHECK(ids_named(app.sink.bytes) == std::set<std::uint32_t>{1});
  CHECK(total_transmits(app.sink.bytes) == 2);
}

TEST_CASE("app pixels: the meter reads the whole frame, not a partial write",
          "[apppixels][kitty]") {
  // The property #148 was after, asserted at the App layer. Before #148 a
  // frame was two writes and last_frame_bytes() reported only the SECOND --
  // zero on a frame that changed nothing, because emit_frame is the write AND
  // meter boundary and the cell diff went out in the first write. A bandwidth
  // budget reading the meter on the graphics tier saw partial frames and had
  // to difference total_bytes() instead.
  //
  // Since #148 the frame is ONE write, so last_frame_bytes() reports the
  // whole frame -- cells and images together -- and a per-frame budget reads
  // it directly. The unambiguous claim below: over a run, the cumulative
  // meter equals everything on the wire, so no frame's cost is hidden in a
  // write the meter didn't count. (One run only: a second test_run_frames
  // would build a fresh driver and reset the cumulative counter.)
  PixelApp app;
  app.run(2);

  // Frame 0 uploads the plate; frame 1 changes nothing. Both are whole single
  // writes. cumulative is the full run's cost, and it equals the wire's size
  // (the frames plus the trailing d=A the shutdown emits through the sink).
  CHECK(app.cumulative().total() > 0);
  CHECK(app.cumulative().total() == app.wire().size());
  // The second frame is unchanged, so its whole single write is empty. This is
  // a meaningful zero, captured before the separate shutdown write.
  CHECK(app.meter().total() == 0);
}

TEST_CASE("app pixels: MapWidget retains one raster through 300 clean frames",
          "[apppixels][mapwidget][kitty][persistent]") {
  MapSpriteApp app;
  app.mutate_on_frame = 300;
  app.move_on_frame = 301;
  app.run_with(std::make_unique<KittyDriver>(), 303);

  CHECK(transmits_of(app.wire, 272) == 1);
  CHECK(frame_updates_of(app.wire, 272) == 1);
  CHECK(placements_of(app.wire, 272) == 2);
  CHECK(placement_deletes_of(app.wire, 272) == 1);
  CHECK(data_deletes_of(app.wire, 272) == 0);
  CHECK(app.map.rasterization_count() == 2);
  CHECK(app.map.submission_count() == 2);
  CHECK_FALSE(app.map.pixel_region_state(app.map.rect()).content_dirty);
}

TEST_CASE("app pixels: MapWidget sprites reach ANSI and glyphs remain Baseline",
          "[apppixels][mapwidget][ansi][fallback]") {
  MapSpriteApp ansi;
  ansi.run_with(std::make_unique<AnsiRgbDriver>(), 2);
  CHECK(ansi.wire.find("38;2;220;30;30") != std::string::npos);
  CHECK(ansi.wire.find("38;2;30;60;220") != std::string::npos);
  CHECK(ansi.map.rasterization_count() == 1);
  CHECK(ansi.map.submission_count() == 1);

  MapSpriteApp baseline;
  baseline.run_with(std::make_unique<FallbackDriver>(), 2);
  tfsupport::TerminalGrid grid{20, 8};
  grid.feed(baseline.wire);
  CHECK(grid.at(0, 1).text == "R");
  CHECK(grid.at(2, 1).text == "B");
  CHECK(baseline.map.rasterization_count() == 0);
  CHECK(baseline.map.submission_count() == 0);
}

TEST_CASE("app pixels: MapWidget retries an unacknowledged raster without "
          "rebuilding it",
          "[apppixels][mapwidget][kitty][persistent][sink]") {
  MapSpriteApp app;
  FailingMapSink sink;
  app.output_override = &sink;
  app.run_with(std::make_unique<KittyDriver>(), 2);

  REQUIRE(sink.writes == 3); // two frames plus explicit shutdown
  CHECK(app.content_dirty_on_second_frame);
  CHECK(app.map.rasterization_count() == 1);
  CHECK(app.map.submission_count() == 1);
  CHECK_FALSE(app.map.pixel_region_state(app.map.rect()).content_dirty);
  CHECK(total_data_transmits(sink.accepted) == 1);
}
