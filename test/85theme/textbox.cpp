// Real App frames and document model checks; no terminal setup/font probe.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/apc.hpp"
#include "support/screen.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/text_box.hpp"

using namespace termforge;
namespace {
auto presentation() -> Theme {
  return {.content_fg = {11, 12, 13},
          .content_bg = {14, 15, 16},
          .muted = {17, 18, 19},
          .accent = {20, 21, 22},
          .glyphs = BorderStyle::Ascii};
}
auto authored(std::string text) -> StyledText {
  return {{std::move(text), {theme::kFg, {}, Attr::None}}};
}
auto same(const Screen& a, const Screen& b) -> void {
  for (int y = 0; y < a.rows(); ++y)
    for (int x = 0; x < a.cols(); ++x) {
      CHECK(a.text_at(x, y) == b.text_at(x, y));
      CHECK(a.at(x, y).fg == b.at(x, y).fg);
      CHECK(a.at(x, y).bg == b.at(x, y).bg);
      CHECK(a.at(x, y).attrs == b.at(x, y).attrs);
    }
}
} // namespace

TEST_CASE("TextBox theme preserves old-default authored spans in mixed streams",
          "[theme][textbox][provenance]") {
  TextBox box;
  box.set_geometry({0, 0, 12, 3});
  const auto live = box.begin_entry("Plain");
  REQUIRE(box.append_to_entry(live, authored("Auth")));
  REQUIRE(box.append_to_entry(live, "Tail"));
  Screen before{12, 3}, after{12, 3};
  box.draw(before);
  const auto bytes = box.retained_bytes();
  const auto builds = box.wrap_build_count();
  box.set_theme(presentation());
  box.draw(after);
  CHECK(after.at(0, 0).fg == presentation().content_fg);
  CHECK(after.at(5, 0).fg == theme::kFg);
  CHECK(after.at(5, 0).bg == Rgb{});
  CHECK(after.at(9, 0).fg == presentation().content_fg);
  CHECK(after.at(11, 2).bg == presentation().content_bg);
  CHECK(box.retained_bytes() == bytes);
  CHECK(box.wrap_build_count() > builds);
  const auto refreshed = box.wrap_build_count();
  box.set_theme(presentation());
  box.draw(after);
  CHECK(box.wrap_build_count() == refreshed);
  auto glyph_only = presentation();
  glyph_only.glyphs = BorderStyle::Double;
  box.set_theme(glyph_only);
  box.draw(after);
  CHECK(box.wrap_build_count() == refreshed);
  box.clear_theme();
  box.draw(after);
  same(before, after);
  REQUIRE(box.append_to_entry(live, "!")); // no live-tail handle/reset change
  CHECK(box.retained_bytes() == bytes + 1);
}

TEST_CASE("TextBox palette changes preserve UTF-8 lead-byte provenance",
          "[theme][textbox][stream]") {
  for (bool plain_lead : {false, true}) {
    TextBox box;
    box.set_geometry({0, 0, 12, 2});
    const auto live = plain_lead ? box.begin_entry(std::string{"\xE7"})
                                 : box.begin_entry(authored("\xE7"));
    box.set_theme(presentation());
    // Complete 界 through a chunk of the OTHER provenance. The lead wins.
    if (plain_lead)
      REQUIRE(box.append_to_entry(live, authored("\x95\x8C")));
    else
      REQUIRE(box.append_to_entry(live, std::string{"\x95\x8C"}));
    REQUIRE(box.append_to_entry(live, "Plain"));
    REQUIRE(box.append_to_entry(live, authored("Auth")));
    Screen screen{12, 2};
    box.draw(screen);
    CHECK(screen.text_at(0, 0) == "界");
    CHECK(screen.at(0, 0).fg ==
          (plain_lead ? presentation().content_fg : theme::kFg));
    CHECK(screen.at(2, 0).fg == presentation().content_fg);
    CHECK(screen.at(7, 0).fg == theme::kFg);
    auto next = presentation();
    next.content_fg = {30, 31, 32};
    box.set_theme(next);
    box.draw(screen);
    CHECK(screen.at(0, 0).fg == (plain_lead ? next.content_fg : theme::kFg));
    CHECK(screen.at(2, 0).fg == next.content_fg);
    CHECK(screen.at(7, 0).fg == theme::kFg);
    REQUIRE(box.finalize_entry(live));
    CHECK_FALSE(box.append_to_entry(live, "stale"));
    CHECK(box.retained_bytes() == 12);
  }
}

TEST_CASE("TextBox scrollbar and follow chip use distinct inherited roles",
          "[theme][textbox][roles]") {
  TextBox box;
  box.set_geometry({0, 0, 14, 3});
  box.set_theme(presentation());
  for (int i = 0; i < 10; ++i)
    box.append("Row " + std::to_string(i));
  Screen screen{14, 3};
  box.draw(screen);
  CHECK(box.style() == BorderStyle::Ascii);
  CHECK(screen.at(13, 0).fg == presentation().muted);
  CHECK(screen.at(13, 2).fg == presentation().accent);
  CHECK(screen.at(13, 0).bg == presentation().content_bg);
  box.scroll(-3);
  box.draw(screen);
  CHECK(screen.text_at(7, 0) == "[");
  CHECK(screen.at(7, 0).fg == presentation().muted);
  CHECK(screen.at(7, 0).bg == presentation().content_bg);
  box.clear_theme();
  box.draw(screen);
  CHECK(box.style() == BorderStyle::Single);
  CHECK(screen.at(13, 0).fg == theme::kDim);
  CHECK(screen.at(13, 1).fg == theme::kFocusBg);
  CHECK(screen.at(13, 0).bg == Rgb{});
}

TEST_CASE(
    "TextBox published block geometry survives invalidated plain-color caches",
    "[theme][textbox][geometry]") {
  TextBox box;
  box.set_geometry({1, 1, 8, 6});
  box.append("Plain wide words");
  const auto block = box.append_block(2, authored("Block"));
  REQUIRE(block);
  Screen screen{12, 9};
  box.draw(screen);
  const auto before = box.block_geometry(*block);
  REQUIRE(before);
  REQUIRE(before->state == TextBlockLayoutState::Visible);
  const auto builds = box.wrap_build_count();
  box.set_theme(presentation());
  const auto pending = box.block_geometry(*block);
  REQUIRE(pending);
  CHECK(pending->allocation == before->allocation);
  CHECK(pending->visible == before->visible);
  CHECK(pending->source_row == before->source_row);
  CHECK(box.wrap_build_count() == builds);
  box.draw(screen);
  CHECK(box.wrap_build_count() > builds);
  const auto after = box.block_geometry(*block);
  REQUIRE(after);
  CHECK(after->allocation == before->allocation);
  CHECK(after->visible == before->visible);
  CHECK(screen.at(1, 1).fg == presentation().content_fg);
  CHECK(screen.at(after->visible.x, after->visible.y).fg == theme::kFg);
}

TEST_CASE(
    "TextBox authored-only caches and block geometry survive theme changes",
    "[theme][textbox][blocks]") {
  TextBox box;
  box.set_geometry({0, 0, 12, 5});
  box.append(authored("Authored"));
  const auto block = box.append_block(2, authored("Fallback"));
  REQUIRE(block);
  const auto live = box.begin_entry(authored("Live"));
  Screen screen{12, 5};
  box.draw(screen);
  const auto geometry = box.block_geometry(*block);
  REQUIRE(geometry);
  const auto builds = box.wrap_build_count();
  const auto bytes = box.retained_bytes();
  box.set_theme(presentation());
  const auto during = box.block_geometry(*block);
  REQUIRE(during);
  CHECK(during->state == geometry->state);
  CHECK(during->visible == geometry->visible);
  CHECK(during->allocation == geometry->allocation);
  box.draw(screen);
  CHECK(box.wrap_build_count() == builds);
  CHECK(screen.at(0, 1).fg ==
        theme::kFg); // fixed-height fallback retains authored style
  CHECK(box.retained_bytes() == bytes);
  CHECK(box.block_count() == 1);
  CHECK(box.line_count() == 3);
  REQUIRE(box.append_to_entry(live, " plain"));
  REQUIRE(box.update_block(*block, 2, authored("Updated")));
  box.draw(screen);
  CHECK(screen.at(0, 1).fg == theme::kFg);
  box.clear_theme();
  REQUIRE(box.finalize_entry(live));
  REQUIRE(box.remove_block(*block));
}

TEST_CASE("TextBox themes preserve scroll retention replacement and local "
          "chrome overrides",
          "[theme][textbox][state]") {
  for (bool theme_first : {false, true}) {
    TextBox box;
    box.set_geometry({0, 0, 14, 3});
    box.set_focused(true);
    if (theme_first) box.set_theme(presentation());
    box.set_style(BorderStyle::Single);
    box.set_scrollbar_colors(theme::kDim, theme::kFocusBg);
    for (int i = 0; i < 10; ++i)
      box.append("Row " + std::to_string(i));
    Screen screen{14, 3};
    box.draw(screen);
    box.scroll(-3);
    box.draw(screen);
    REQUIRE_FALSE(box.at_bottom());
    const auto row = tfsupport::row_text(screen, 1, 0, box.content_w());
    const auto bytes = box.retained_bytes();
    box.set_theme(presentation());
    box.draw(screen);
    CHECK(tfsupport::row_text(screen, 1, 0, box.content_w()) == row);
    CHECK_FALSE(box.at_bottom());
    CHECK(box.focused());
    CHECK(box.retained_bytes() == bytes);
    CHECK(box.style() == BorderStyle::Single);
    CHECK(screen.at(13, 0).fg == theme::kDim);
    CHECK(screen.at(13, 1).fg == theme::kFocusBg);
    box.clear_theme();
    box.draw(screen);
    CHECK_FALSE(box.at_bottom());
    CHECK(box.style() == BorderStyle::Single);
    CHECK(tfsupport::row_text(screen, 1, 0, box.content_w()) == row);
    box.scroll_to_bottom();
    box.set_theme(presentation());
    const auto live = box.begin_entry("Plain");
    REQUIRE(box.replace_entry(live, authored("Auth")));
    box.draw(screen);
    CHECK(screen.at(0, 2).fg == theme::kFg);
    REQUIRE(box.replace_entry(live, std::string{"Plain"}));
    box.draw(screen);
    CHECK(screen.at(0, 2).fg == presentation().content_fg);
    box.set_retention({.max_entries = 2});
    box.clear_theme();
    REQUIRE(box.finalize_entry(live));
    CHECK(box.line_count() == 2);
    CHECK_FALSE(box.append_to_entry(live, "stale"));
  }
}

TEST_CASE("TextBox copy and move preserve provenance and explicit chrome",
          "[theme][textbox][copy]") {
  TextBox source;
  source.set_style(BorderStyle::Single);
  source.set_scrollbar_colors({31, 32, 33}, {34, 35, 36});
  source.set_geometry({0, 0, 12, 2});
  source.append("Plain");
  source.append(authored("Auth"));
  source.set_theme(presentation());
  TextBox copy{source};
  TextBox moved{std::move(copy)};
  TextBox assigned;
  assigned = std::move(moved);
  auto next = presentation();
  next.content_fg = {40, 41, 42};
  assigned.set_theme(next);
  Screen screen{12, 2};
  assigned.draw(screen);
  CHECK(screen.at(0, 0).fg == next.content_fg);
  CHECK(screen.at(0, 1).fg == theme::kFg);
  CHECK(assigned.style() == BorderStyle::Single);
  assigned.clear_theme();
  assigned.draw(screen);
  CHECK(screen.at(0, 0).fg == theme::kFg);
  CHECK(screen.at(0, 0).bg == Rgb{});
  source.draw(screen);
  CHECK(screen.at(0, 0).fg == presentation().content_fg);
  assigned.append("Extra");
  assigned.set_theme(presentation());
  assigned.draw(screen);
  CHECK(screen.at(11, 0).fg == Rgb{31, 32, 33});
  CHECK(screen.at(11, 1).fg == Rgb{34, 35, 36});
}

namespace {
struct DocumentSink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
class DocumentApp final : public App {
 public:
  TextBox box;
  DocumentSink sink;
  int frame{0};
  TextEntryHandle live;
  PixelSurface pixels{{2, 2}, Pixel{180, 60, 30, 255}};
  std::optional<TextBlockHandle> block;
  bool images{false};
  auto run(int tier, bool with_pixels = false) -> void {
    images = with_pixels;
    box.append("Plain");
    if (images) {
      const auto added = box.append_block(3, authored("pixels"));
      REQUIRE(added);
      block = *added;
    } else {
      box.append(StyledText{{"Auth", {theme::kFg, {}, Attr::Bold}}});
      live = box.begin_entry("Live");
    }
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(4, 20, 4, nullptr, std::move(selected));
  }

 protected:
  auto read_available(char*, int) -> int override { return 0; }
  auto wait_readable(int) -> bool override { return false; }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (frame == 0) box.set_theme(presentation());
    if (frame == 2) {
      auto next = presentation();
      next.content_fg = {30, 31, 32};
      next.content_bg = {33, 34, 35};
      box.set_theme(next);
    }
    box.set_geometry({0, 0, 20, 4});
    box.draw(screen);
    if (images) {
      const auto geometry = box.block_geometry(*block);
      REQUIRE(geometry);
      REQUIRE(geometry->state == TextBlockLayoutState::Visible);
      CHECK(geometry->visible == Rect{0, 1, 20, 3});
      pixels.set_geometry(geometry->visible);
      pixels.draw(screen);
      render_pixel_regions(pixels);
      CHECK(box.retained_bytes() == 11);
      CHECK(box.line_count() == 2);
    } else {
      CHECK(box.retained_bytes() == 13);
      CHECK(box.line_count() == 3);
    }
    CHECK(box.at_bottom());
    ++frame;
  }
};
} // namespace

TEST_CASE(
    "TextBox theme changes leave persistent block producer identity intact",
    "[theme][textbox][app][pixels]") {
  DocumentApp app;
  app.run(2, true);
  CHECK(app.pixels.submission_count() == 1);
  CHECK_FALSE(app.pixels.content_dirty());
  REQUIRE(app.sink.frames.size() >= 4);
  CHECK_FALSE(app.sink.frames[2].empty()); // text colors actually change
  CHECK(tfsupport::total_data_transmits(app.sink.frames[0]) == 1);
  const auto ids = tfsupport::ids_named(app.sink.frames[0]);
  REQUIRE(ids.size() == 1);
  for (std::size_t i : {1U, 2U, 3U}) {
    CHECK(tfsupport::total_data_transmits(app.sink.frames[i]) == 0);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[i], *ids.begin()) == 0);
  }
}

TEST_CASE("TextBox roles reach real three-tier frames without idle writes or "
          "authored recoloring",
          "[theme][textbox][app][wire]") {
  for (int tier : {0, 1, 2}) {
    DocumentApp app;
    app.run(tier);
    REQUIRE(app.sink.frames.size() == 4);
    CHECK(app.sink.frames[1].empty());
    CHECK(app.sink.frames[3].empty());
    tfsupport::TerminalGrid terminal{20, 4};
    terminal.feed(app.sink.frames[0]);
    CHECK(terminal.row_text(0).starts_with("Plain"));
    CHECK(terminal.row_text(1).starts_with("Auth"));
    CHECK(terminal.at(0, 1).bold);
    CHECK_FALSE(terminal.at(0, 0).bold);
    if (tier == 0)
      CHECK(terminal.at(0, 0).fg == -1);
    else
      CHECK(terminal.at(0, 0).fg == ((11 << 16) | (12 << 8) | 13));
    const auto authored_fg = terminal.at(0, 1).fg;
    terminal.feed(app.sink.frames[2]);
    CHECK(terminal.at(0, 1).fg == authored_fg);
    CHECK(terminal.at(0, 1).bold);
    CHECK(terminal.row_text(2).starts_with("Live"));
    if (tier != 0) {
      CHECK(terminal.at(0, 0).fg == ((30 << 16) | (31 << 8) | 32));
      CHECK(terminal.at(19, 3).bg == ((33 << 16) | (34 << 8) | 35));
    }
    REQUIRE(app.box.append_to_entry(app.live, "!"));
  }
}

TEST_CASE("TextBox themed wide content clips without touching neighbours",
          "[theme][textbox][clipping]") {
  for (int width : {0, 1, 2, 3}) {
    TextBox box;
    box.set_geometry({2, 2, width, 2});
    box.set_theme(presentation());
    box.append("界 wide");
    box.append(authored("界 auth"));
    Screen screen{8, 6};
    screen.fill_rect(0, 0, 8, 6, {200, 201, 202}, {});
    box.draw(screen);
    for (int y = 0; y < 6; ++y)
      for (int x = 0; x < 8; ++x)
        if (!box.rect().contains(x, y))
          CHECK(screen.at(x, y).fg == Rgb{200, 201, 202});
  }
}
