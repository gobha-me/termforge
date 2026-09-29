// Generated palettes versus authored content; production App, not wire replay.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/apc.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/map_widget.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using namespace termforge;
namespace {
auto image_roles() -> Theme {
  return {.content_fg = {101, 2, 3},
          .content_bg = {4, 5, 6},
          .accent = {90, 91, 92},
          .glyphs = BorderStyle::Ascii};
}
auto cells_equal(const Screen& a, const Screen& b) -> void {
  for (int y = 0; y < a.rows(); ++y)
    for (int x = 0; x < a.cols(); ++x) {
      CHECK(a.text_at(x, y) == b.text_at(x, y));
      CHECK(a.at(x, y).fg == b.at(x, y).fg);
      CHECK(a.at(x, y).bg == b.at(x, y).bg);
      CHECK(a.at(x, y).attrs == b.at(x, y).attrs);
    }
}
auto pixels_equal(const Image& a, const Image& b) -> bool {
  return a.width() == b.width() && a.height() == b.height() &&
         std::ranges::equal(a.pixels(), b.pixels());
}
auto configure(MapWidget& map) -> void {
  TileSet tiles;
  // Old defaults are still authored colors, never inferred inheritance.
  tiles.define(1, {"T", theme::kFg, theme::kBg, Rect{0, 0, 1, 1}});
  tiles.set_atlas(Image{1, 1, std::vector<Pixel>{{200, 40, 60, 128}}}, {1, 1});
  map.set_tileset(std::move(tiles));
  map.set_map_size(3, 1);
  map.set_tile_size(2, 1);
  map.set_tile(0, 0, 0, 1);
  map.set_tile(0, 1, 0, 1);
  map.set_tile(0, 2, 0, 1);
  map.set_geometry({1, 1, 5, 1}); // two tiles + one uncovered column
}
auto seed(WaveformWidget& wave) -> void {
  REQUIRE(wave.set_range(0.0F, 1.0F));
  REQUIRE(wave.push(std::array<float, 3>{0.0F, 0.5F, 1.0F}));
  wave.set_geometry({1, 1, 5, 3});
}
} // namespace

TEST_CASE("Waveform Theme invalidates only its consumed generated palette",
          "[theme][images][waveform]") {
  WaveformWidget wave{5};
  seed(wave);
  Screen before{8, 6}, themed{8, 6}, after{8, 6};
  wave.draw(before);
  const auto region = wave.pixel_regions().front();
  const auto* original = wave.draw_pixels(region, {16, 8});
  REQUIRE(original != nullptr);
  const auto old = *original;
  wave.pixel_region_submitted(region);
  wave.set_theme(image_roles());
  CHECK(wave.pixel_region_state(region).content_dirty);
  wave.draw(themed);
  CHECK(themed.at(1, 1).fg == image_roles().accent);
  CHECK(themed.at(1, 1).bg == image_roles().content_bg);
  const auto* changed = wave.draw_pixels(region, {16, 8});
  REQUIRE(changed != nullptr);
  CHECK_FALSE(pixels_equal(old, *changed));
  const auto themed_pixels = *changed;
  wave.pixel_region_submitted(region);
  auto unrelated = image_roles();
  unrelated.glyphs = BorderStyle::Double;
  unrelated.warning = {100, 101, 102};
  unrelated.content_fg = {200, 201, 202};
  wave.set_theme(unrelated);
  CHECK_FALSE(wave.pixel_region_state(region).content_dirty);
  CHECK(pixels_equal(themed_pixels, *wave.draw_pixels(region, {16, 8})));
  CHECK(wave.sample_count() == 3);
  CHECK(wave.capacity() == 5);
  CHECK(wave.pixel_regions() == std::vector<Rect>{region});
  wave.clear_theme();
  wave.draw(after);
  cells_equal(before, after);
  CHECK(pixels_equal(old, *wave.draw_pixels(region, {16, 8})));
  CHECK(wave.pixel_region_state(region).content_dirty);
}

TEST_CASE("Map Theme preserves authored tile colors and camera/hit state",
          "[theme][images][map]") {
  MapWidget map;
  configure(map);
  map.set_camera(1, 0);
  Screen before{8, 4}, themed{8, 4}, after{8, 4};
  map.draw(before);
  const auto regions = map.pixel_regions();
  REQUIRE(regions.size() == 1);
  const auto* initial = map.draw_pixels(regions.front(), {16, 8});
  REQUIRE(initial != nullptr);
  const auto original = *initial;
  const auto hit = map.tile_at(2, 1);
  const auto camera = map.camera();
  map.pixel_region_submitted(regions.front());
  const auto rasters = map.rasterization_count();
  map.set_theme(image_roles());
  map.draw(themed);
  CHECK(themed.at(1, 1).fg == theme::kFg);
  CHECK(themed.at(1, 1).bg == theme::kBg);
  CHECK(themed.at(5, 1).fg == image_roles().content_fg);
  CHECK(themed.at(5, 1).bg == image_roles().content_bg);
  CHECK(map.pixel_region_state(regions.front()).content_dirty);
  const auto* changed = map.draw_pixels(regions.front(), {16, 8});
  REQUIRE(changed != nullptr);
  CHECK_FALSE(pixels_equal(original, *changed));
  CHECK(map.rasterization_count() == rasters + 1);
  const auto colored = *changed;
  map.pixel_region_submitted(regions.front());
  auto unrelated = image_roles();
  unrelated.content_fg = {50, 51, 52};
  unrelated.accent = {100, 101, 102};
  unrelated.glyphs = BorderStyle::Double;
  map.set_theme(unrelated);
  CHECK_FALSE(map.pixel_region_state(regions.front()).content_dirty);
  CHECK(pixels_equal(colored, *map.draw_pixels(regions.front(), {16, 8})));
  CHECK(map.rasterization_count() == rasters + 1);
  CHECK(map.camera() == camera);
  CHECK(map.tile_at(2, 1) == hit);
  CHECK(map.tile(0, 1, 0) == 1);
  CHECK(map.pixel_regions() == regions);
  map.clear_theme();
  map.draw(after);
  cells_equal(before, after);
  CHECK(pixels_equal(original, *map.draw_pixels(regions.front(), {16, 8})));
  CHECK(map.camera() == camera);
}

TEST_CASE("PixelSurface Theme styles Baseline without mutating authored RGBA",
          "[theme][images][surface][authored]") {
  PixelSurface surface{{2, 1}};
  surface.pixels()[0] = {200, 40, 60, 255};
  surface.pixels()[1] = {10, 20, 30, 0};
  surface.set_geometry({1, 1, 4, 2});
  surface.set_fit(PlacementFit::Exact);
  const auto& authored_surface = std::as_const(surface);
  const auto original = authored_surface.image();
  const auto* borrowed = surface.draw_pixels(surface.rect(), {});
  surface.pixel_region_submitted(surface.rect());
  const auto submissions = surface.submission_count();
  Screen before{7, 5}, themed{7, 5}, after{7, 5};
  surface.draw(before);
  auto presentation = image_roles();
  presentation.content_bg = {255, 255, 255};
  surface.set_theme(presentation);
  surface.draw(themed);
  CHECK(themed.at(1, 1).fg == Rgb{200, 40, 60}); // opaque pixel is authored
  CHECK(themed.at(2, 1).fg == presentation.content_bg); // alpha=0 Baseline
  CHECK(themed.text_at(2, 1) == "@");
  CHECK(themed.at(3, 1).fg == presentation.content_fg); // uncovered Exact cells
  CHECK(themed.at(3, 1).bg == presentation.content_bg);
  CHECK(surface.draw_pixels(surface.rect(), {80, 40}) == borrowed);
  CHECK(pixels_equal(original, authored_surface.image()));
  CHECK_FALSE(surface.content_dirty());
  CHECK(surface.submission_count() == submissions);
  CHECK(surface.extent() == Extent{2, 1});
  CHECK(surface.fit() == PlacementFit::Exact);
  surface.clear_theme();
  surface.draw(after);
  cells_equal(before, after);
  CHECK_FALSE(surface.content_dirty());
  CHECK(pixels_equal(original, authored_surface.image()));
}

TEST_CASE(
    "Themed image fallbacks own tiny rectangles without touching neighbours",
    "[theme][images][clipping]") {
  WaveformWidget wave;
  seed(wave);
  MapWidget map;
  configure(map);
  PixelSurface pixels{{2, 2}, {100, 110, 120, 128}};
  PixelSurface empty{{0, 0}};
  for (Widget* widget : std::array<Widget*, 4>{&wave, &map, &pixels, &empty}) {
    widget->set_theme(image_roles());
    for (int width : {0, 1, 2, 3}) {
      const Rect rect{2, 2, width, 2};
      widget->set_geometry(rect);
      Screen screen{8, 6};
      screen.clear({200, 201, 202}, {});
      widget->draw(screen);
      for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
          if (!rect.contains(x, y))
            CHECK(screen.at(x, y).fg == Rgb{200, 201, 202});
    }
  }
}

namespace {
struct PaletteSink final : ByteSink {
  std::vector<std::string> frames;
  std::vector<bool> accepted;
  int refuse{-1};
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    const bool accept = static_cast<int>(frames.size()) != refuse;
    frames.emplace_back(bytes.begin(), bytes.end());
    accepted.push_back(accept);
    if (!accept)
      return std::unexpected{
          ErrorEvent{Severity::Warning, "test-sink", "palette frame refused"}};
    return {};
  }
};
class PaletteApp final : public App {
 public:
  WaveformWidget wave{5};
  MapWidget map;
  PixelSurface pixels{{2, 1}, {180, 40, 60, 255}};
  PaletteSink sink;
  std::vector<ImageResidency> residency;
  std::vector<bool> wave_dirty, map_dirty, source_dirty;
  int frame{0}, errors{0};
  std::vector<std::string> error_sources;
  auto run(int tier, bool refusal = false, bool authored_only = false,
           bool translucent = false) -> void {
    m_authored_only = authored_only;
    seed(wave);
    configure(map);
    if (translucent) pixels.image().fill({0, 0, 2, 1}, {180, 40, 60, 128});
    if (refusal) sink.refuse = 2;
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(5, 20, 9, nullptr, std::move(selected));
  }

 protected:
  auto read_available(char*, int) -> int override { return 0; }
  auto wait_readable(int) -> bool override { return false; }
  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      CHECK(error->severity == Severity::Warning);
      error_sources.push_back(error->source);
      ++errors;
    }
  }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    residency.push_back(driver().residency());
    wave_dirty.push_back(wave.pixel_region_state(wave.rect()).content_dirty);
    map_dirty.push_back(map.pixel_region_state(map.rect()).content_dirty);
    source_dirty.push_back(pixels.content_dirty());
    screen.clear();
    if (frame == 0 || frame == 2) {
      auto next = image_roles();
      if (frame == 2) {
        next.content_bg = {40, 41, 42};
        next.accent = {80, 81, 82};
      }
      wave.set_theme(next);
      map.set_theme(next);
      pixels.set_theme(next);
    }
    if (!m_authored_only) {
      wave.draw(screen);
      render_pixel_regions(wave);
      map.set_geometry({1, 5, 5, 1});
      map.draw(screen);
      render_pixel_regions(map);
    }
    pixels.set_geometry({8, 1, 4, 2});
    pixels.draw(screen);
    render_pixel_regions(pixels);
    ++frame;
  }

 private:
  bool m_authored_only{false};
};
auto first_ids(const PaletteApp& app) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> ids;
  for (const auto& command : tfsupport::apcs(app.sink.frames.front()))
    if (tfsupport::key_value(command, "a") == "t")
      ids.push_back(static_cast<std::uint32_t>(
          std::stoul(tfsupport::key_value(command, "i"))));
  return ids;
}
} // namespace

TEST_CASE("Palette writes preserve source acknowledgement through acceptance "
          "and refusal",
          "[theme][images][app][kitty]") {
  for (bool refusal : {false, true}) {
    PaletteApp app;
    app.run(2, refusal, false, true);
    REQUIRE(app.sink.frames.size() >= 5);
    const auto ids = first_ids(app);
    REQUIRE(ids.size() == 3);
    CHECK(app.sink.frames[1].empty());
    CHECK(tfsupport::total_data_transmits(app.sink.frames[2]) == 2);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], ids[0]) == 1);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], ids[1]) == 1);
    CHECK(tfsupport::frame_updates_of(app.sink.frames[2], ids[2]) == 0);
    CHECK(app.sink.accepted[2] == !refusal);
    CHECK(app.errors == (refusal ? 1 : 0));
    if (refusal)
      CHECK(app.error_sources == std::vector<std::string>{"test-sink"});
    CHECK(app.wave_dirty[3] == refusal);
    CHECK(app.map_dirty[3] == refusal);
    CHECK_FALSE(app.source_dirty[3]);
    CHECK(tfsupport::total_data_transmits(app.sink.frames[3]) ==
          (refusal ? 3 : 0));
    CHECK_FALSE(app.wave_dirty[4]);
    CHECK_FALSE(app.map_dirty[4]);
    CHECK(app.sink.frames[4].empty());
    // Current App recovery conservatively recreates every resident producer
    // in a refused mixed image frame. This is not a source-dirty Theme change.
    CHECK(app.pixels.submission_count() == (refusal ? 2 : 1));
    CHECK(app.map.submission_count() == 2);
    CHECK(app.wave.sample_count() == 3);
    CHECK(app.map.tile(0, 1, 0) == 1);
    CHECK(app.residency[1].pinned_images == 3);
    CHECK(app.residency[4].pinned_images == 3);
    for (std::size_t i : {1U, 2U, 3U, 4U}) {
      const bool repair = refusal && i == 3;
      CHECK(tfsupport::total_transmits(app.sink.frames[i]) == (repair ? 3 : 0));
      for (const auto id : ids) {
        CHECK(tfsupport::data_deletes_of(app.sink.frames[i], id) ==
              (repair ? 1 : 0));
        CHECK(tfsupport::placement_deletes_of(app.sink.frames[i], id) == 0);
      }
    }
    // PixelSurface's original source bytes and alpha remain authored.
    CHECK(std::as_const(app.pixels).image().at(0, 0) ==
          Pixel{180, 40, 60, 128});
  }
}

TEST_CASE(
    "Palette changes reach three rendering tiers with quiet stable frames",
    "[theme][images][app][tiers]") {
  for (int tier : {0, 1, 2}) {
    PaletteApp app;
    app.run(tier);
    REQUIRE(app.sink.frames.size() >= 5);
    CHECK(app.sink.frames[1].empty());
    CHECK(app.sink.frames[3].empty());
    CHECK(app.sink.frames[4].empty());
    CHECK(app.errors == 0);
    if (tier < 2) {
      CHECK(tfsupport::total_data_transmits(app.sink.frames[0]) == 0);
      CHECK(tfsupport::total_data_transmits(app.sink.frames[2]) == 0);
    }
    tfsupport::TerminalGrid terminal{20, 9};
    terminal.feed(app.sink.frames[0]);
    terminal.feed(app.sink.frames[2]);
    if (tier == 0) {
      CHECK(terminal.at(5, 5).bg == -1);
      CHECK(app.pixels.submission_count() == 0);
    } else {
      CHECK(terminal.at(5, 5).bg == ((40 << 16) | (41 << 8) | 42));
      CHECK(app.pixels.submission_count() == 1);
    }
    CHECK(app.map.camera() == std::pair{0, 0});
  }
}

TEST_CASE("Authored PixelSurface Theme changes do not trigger enhanced redraws",
          "[theme][images][app][surface]") {
  for (int tier : {1, 2}) {
    PaletteApp app;
    app.run(tier, false, true);
    REQUIRE(app.sink.frames.size() >= 5);
    CHECK(app.sink.frames[2].empty());
    CHECK(app.pixels.submission_count() == 1);
    CHECK_FALSE(app.pixels.content_dirty());
    CHECK(std::as_const(app.pixels).image().at(0, 0) ==
          Pixel{180, 40, 60, 255});
  }
}

TEST_CASE("Translucent authored source keeps ANSI's honest refusal contract",
          "[theme][images][app][alpha][refusal]") {
  PaletteApp app;
  app.run(1, false, true, true);
  CHECK(app.errors > 0);
  for (const auto& source : app.error_sources)
    CHECK(source == "ansi_rgb");
  CHECK(app.pixels.submission_count() == 0);
  CHECK(app.pixels.content_dirty());
  CHECK(std::as_const(app.pixels).image().at(0, 0) == Pixel{180, 40, 60, 128});
}
