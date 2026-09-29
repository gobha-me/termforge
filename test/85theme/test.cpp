// Production App loop below, no tty. test_run_frames intentionally skips
// terminal setup/startup requirements; this tests authored cells and emitted
// frames, not emulator/font behavior or terminal startup.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "support/screen.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/composer.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/numeric_input.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/slider.hpp"

using namespace termforge;
namespace {
auto palette() -> Theme {
  return {.content_fg = {1, 2, 3},
          .content_bg = {4, 5, 6},
          .surface_fg = {7, 8, 9},
          .surface_bg = {10, 11, 12},
          .focus_fg = {13, 14, 15},
          .focus_bg = {16, 17, 18},
          .selection_fg = {19, 20, 21},
          .selection_bg = {22, 23, 24},
          .muted = {25, 26, 27},
          .accent = {28, 29, 30},
          .info = {31, 32, 33},
          .warning = {34, 35, 36},
          .error = {37, 38, 39},
          .glyphs = BorderStyle::Ascii};
}
auto assert_cells(const Screen& a, const Screen& b) -> void {
  REQUIRE(a.cols() == b.cols());
  REQUIRE(a.rows() == b.rows());
  for (int y = 0; y < a.rows(); ++y)
    for (int x = 0; x < a.cols(); ++x) {
      CHECK(a.text_at(x, y) == b.text_at(x, y));
      CHECK(a.at(x, y).fg == b.at(x, y).fg);
      CHECK(a.at(x, y).bg == b.at(x, y).bg);
      CHECK(a.at(x, y).attrs == b.at(x, y).attrs);
    }
}
struct LegacyWidget final : Widget {
  auto draw(Screen&) -> void override {}
};
static_assert(!std::is_abstract_v<LegacyWidget>);
struct ObservedWidget final : Widget {
  int changes{0};
  auto draw(Screen&) -> void override {}

 private:
  auto on_theme_changed() -> void override { ++changes; }
};
struct Sink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
class ThemeApp final : public App {
 public:
  Button button{"Action"};
  Checkbox check{"Enabled"};
  NumericInput number;
  Sink sink;
  std::vector<std::string> authored;
  int frame{0};
  auto run(int tier) -> void {
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(4, 24, 6, nullptr, std::move(selected));
  }

 protected:
  auto read_available(char*, int) -> int override { return 0; }
  auto wait_readable(int) -> bool override { return false; }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (frame == 0) {
      button.set_theme(palette());
      check.set_theme(palette());
      number.set_theme(palette());
      button.set_focused(true);
      check.set_focused(true);
      check.set_checked(true);
      REQUIRE(number.set_draft("bad"));
      number.set_focused(true);
    }
    if (frame == 2) {
      auto next = palette();
      next.content_bg = {100, 101, 102};
      button.set_theme(next);
      check.set_theme(next);
      number.set_theme(next);
    }
    button.set_geometry({0, 0, 24, 1});
    check.set_geometry({0, 1, 24, 1});
    number.set_geometry({0, 2, 24, 3});
    button.draw(screen);
    check.draw(screen);
    number.draw(screen);
    CHECK(any(screen.at(0, 0).attrs & Attr::Bold));
    CHECK(any(screen.at(0, 1).attrs & Attr::Bold));
    CHECK(any(screen.at(0, 4).attrs & Attr::Bold));
    CHECK(screen.at(0, 4).fg == palette().warning);
    CHECK(number.draft() == "bad");
    CHECK(std::get<std::int64_t>(number.value()) == 0);
    std::string text;
    for (int y = 0; y < screen.rows(); ++y)
      text += tfsupport::row_text(screen, y) + '\n';
    authored.push_back(text);
    ++frame;
  }
};
} // namespace

TEST_CASE("Theme state is copied base-owned and compatible with legacy widgets",
          "[theme]") {
  LegacyWidget legacy;
  legacy.set_theme(palette());
  REQUIRE(legacy.theme_snapshot());
  ObservedWidget custom;
  Widget& widget = custom;
  CHECK_FALSE(widget.theme_snapshot());
  custom.set_geometry({1, 2, 3, 4});
  custom.set_focused(true);
  auto source = palette();
  widget.set_theme(source);
  CHECK(custom.changes == 1);
  widget.set_theme(source);
  CHECK(custom.changes == 1);
  source.content_fg = {200, 201, 202};
  CHECK(widget.theme_snapshot()->content_fg == palette().content_fg);
  CHECK(widget.rect() == Rect{1, 2, 3, 4});
  CHECK(widget.focused());
  widget.clear_theme();
  widget.clear_theme();
  CHECK(custom.changes == 2);
  CHECK_FALSE(widget.theme_snapshot());
}

TEST_CASE("Theme clearing restores exact historical primitive presentation",
          "[theme][compatibility]") {
  Label label{"Hello"};
  Button button{"Action"};
  Frame frame{"Border"};
  Checkbox check{"Check"};
  RadioGroup radio{{"First", "Second"}};
  TextInput input;
  Composer composer;
  Slider slider;
  NumericInput numeric;
  input.set_placeholder("Placeholder");
  composer.set_text("two\nlines");
  CHECK(slider.set_value(25));
  REQUIRE(numeric.set_draft("invalid"));
  const std::array<Widget*, 9> widgets{&label,    &button, &frame,
                                       &check,    &radio,  &input,
                                       &composer, &slider, &numeric};
  for (auto* widget : widgets) {
    widget->set_geometry({1, 1, 12, 3});
    for (bool focused : {false, true}) {
      widget->set_focused(focused);
      Screen before{14, 5}, after{14, 5};
      widget->draw(before);
      widget->set_theme(palette());
      widget->clear_theme();
      widget->draw(after);
      assert_cells(before, after);
    }
  }
  CHECK(composer.text() == "two\nlines");
  CHECK(numeric.draft() == "invalid");
  CHECK(slider.value() == 25);
}

TEST_CASE("Primitive roles apply without mutating content or callbacks",
          "[theme]") {
  Label label{"Label"};
  Button button{"Button"};
  Checkbox check{"Checkbox"};
  RadioGroup radio{{"Radio", "Other"}};
  TextInput input;
  Composer composer;
  Slider slider;
  int callbacks = 0;
  button.on_activate([&] { ++callbacks; });
  check.on_change([&](bool) { ++callbacks; });
  radio.on_change([&](int) { ++callbacks; });
  input.on_change([&](const std::string&) { ++callbacks; });
  composer.on_change([&](const std::string&) { ++callbacks; });
  slider.on_change([&](double) { ++callbacks; });
  input.set_text("draft");
  composer.set_text("draft");
  const std::array<Widget*, 7> widgets{&label, &button,   &check, &radio,
                                       &input, &composer, &slider};
  for (auto* widget : widgets) {
    widget->set_geometry({1, 1, 12, 3});
    widget->set_theme(palette());
    Screen screen{14, 5};
    widget->draw(screen);
    CHECK(screen.at(1, 1).fg == palette().content_fg);
    CHECK(screen.at(1, 1).bg == palette().content_bg);
    CHECK(screen.at(0, 0).fg != palette().content_fg);
    CHECK(widget->rect() == Rect{1, 1, 12, 3});
    CHECK_FALSE(widget->dirty());
  }
  CHECK(input.text() == "draft");
  CHECK(composer.text() == "draft");
  CHECK(callbacks == 0);
  input.set_text("");
  input.set_placeholder("placeholder");
  Screen screen{14, 5};
  input.draw(screen);
  CHECK(screen.at(1, 2).fg == palette().muted);
}

TEST_CASE(
    "Theme never steals explicit default colors or styles in either order",
    "[theme][overrides]") {
  for (bool first : {false, true}) {
    Label label{"L"};
    Frame frame;
    Checkbox check{"C"};
    RadioGroup radio{{"R"}};
    Slider slider;
    const std::array<Widget*, 5> widgets{&label, &frame, &check, &radio,
                                         &slider};
    if (first)
      for (auto* widget : widgets)
        widget->set_theme(palette());
    label.set_colors(theme::kFg, theme::kBg);
    frame.set_style(BorderStyle::Single);
    frame.set_border_color({0x60, 0x60, 0x80});
    check.set_style(BorderStyle::Single);
    radio.set_style(BorderStyle::Single);
    slider.set_style(BorderStyle::Single);
    slider.set_colors(theme::kFg, theme::kBg, theme::kFocusFg, theme::kFocusBg);
    auto next = palette();
    next.content_bg = {210, 211, 212};
    for (auto* widget : widgets) {
      widget->set_geometry({0, 0, 10, 3});
      widget->set_theme(next);
    }
    Screen screen{10, 3};
    label.draw(screen);
    CHECK(screen.at(0, 0).fg == theme::kFg);
    CHECK(screen.at(0, 0).bg == theme::kBg);
    frame.draw(screen);
    CHECK(screen.text_at(0, 0) == "┌");
    CHECK(screen.at(0, 0).fg == Rgb{0x60, 0x60, 0x80});
    CHECK(frame.style() == BorderStyle::Single);
    CHECK(check.style() == BorderStyle::Single);
    CHECK(radio.style() == BorderStyle::Single);
    CHECK(slider.style() == BorderStyle::Single);
    slider.draw(screen);
    CHECK(screen.at(0, 0).fg == theme::kFg);
    CHECK(screen.at(0, 0).bg == theme::kBg);
    for (auto* widget : widgets)
      widget->clear_theme();
    label.draw(screen);
    CHECK(screen.at(0, 0).fg == theme::kFg);
  }
}

TEST_CASE("Theme invalidates cached marks and preserves frame interiors",
          "[theme][glyphs]") {
  Checkbox check{"C"};
  RadioGroup radio{{"R"}};
  Frame frame;
  check.set_checked(true);
  check.set_geometry({0, 0, 10, 1});
  radio.set_geometry({0, 1, 10, 1});
  frame.set_geometry({0, 0, 10, 3});
  Screen screen{10, 3};
  check.draw(screen); // populate Unicode cache before applying ASCII
  check.set_theme(palette());
  radio.set_theme(palette());
  frame.set_theme(palette());
  check.draw(screen);
  radio.draw(screen);
  CHECK(tfsupport::row_text(screen, 0).starts_with("[x] C"));
  CHECK(tfsupport::row_text(screen, 1).starts_with("(*) R"));
  const auto interior = screen.text_at(1, 1);
  const std::string saved{interior};
  frame.draw(screen);
  CHECK(screen.text_at(0, 0) == "+");
  CHECK(screen.text_at(1, 1) == saved);
  CHECK(frame.style() == BorderStyle::Ascii);
  check.clear_theme();
  check.draw(screen);
  CHECK(check.style() == BorderStyle::Single);
}

TEST_CASE("Themed state remains legible without color", "[theme][focus]") {
  Button button{"B"};
  Checkbox check{"C"};
  RadioGroup radio{{"R"}};
  Slider slider;
  const std::array<Widget*, 4> widgets{&button, &check, &radio, &slider};
  for (auto* widget : widgets) {
    widget->set_geometry({0, 0, 10, 1});
    widget->set_theme(palette());
    widget->set_focused(true);
    Screen screen{10, 1};
    widget->draw(screen);
    CHECK(screen.at(0, 0).fg == palette().focus_fg);
    CHECK(screen.at(0, 0).bg == palette().focus_bg);
    CHECK(any(screen.at(0, 0).attrs & Attr::Bold));
    widget->set_focused(false);
    widget->draw(screen);
    CHECK_FALSE(any(screen.at(0, 0).attrs & Attr::Bold));
  }
  REQUIRE(button.on_event(KeyEvent{.key = Key::Enter}));
  Screen pressed{10, 1};
  button.draw(pressed);
  CHECK(pressed.at(0, 0).fg == palette().selection_fg);
  CHECK(pressed.at(0, 0).bg == palette().selection_bg);
  CHECK(any(pressed.at(0, 0).attrs & Attr::Reverse));
}

TEST_CASE(
    "Numeric theme propagates to owned editor without altering invalid draft",
    "[theme][compound]") {
  NumericInput input;
  int callbacks = 0;
  input.on_change([&](NumericValue) { ++callbacks; });
  input.set_geometry({1, 1, 12, 3});
  input.set_focused(true);
  REQUIRE(input.set_draft("bad"));
  REQUIRE(input.on_event(KeyEvent{.key = Key::Home}));
  const auto cursor = input.cursor_pos();
  const auto error = input.error()->message;
  input.set_theme(palette());
  Screen screen{14, 5};
  input.draw(screen);
  CHECK(screen.at(2, 2).fg == palette().focus_fg);
  CHECK(screen.at(2, 2).bg == palette().focus_bg);
  CHECK(screen.at(3, 2).fg == palette().content_fg);
  CHECK(screen.at(3, 2).bg == palette().content_bg);
  CHECK(any(screen.at(2, 2).attrs & Attr::Reverse));
  CHECK(screen.text_at(1, 2) == "!");
  CHECK(screen.at(1, 2).fg == palette().warning);
  CHECK(screen.at(1, 3).fg == palette().warning);
  CHECK(any(screen.at(1, 3).attrs & Attr::Bold));
  CHECK(input.draft() == "bad");
  CHECK(input.cursor_pos() == cursor);
  CHECK(input.error()->message == error);
  CHECK(callbacks == 0);
  input.clear_theme();
  input.draw(screen);
  CHECK(screen.at(2, 2).fg == theme::kFg);
  CHECK(screen.at(1, 3).attrs == Attr::None);
}

TEST_CASE(
    "Themed editor cursors use focus roles without resetting byte positions",
    "[theme][editor]") {
  TextInput input;
  Composer composer;
  input.set_text("ab");
  composer.set_text("ab");
  const std::array<Widget*, 2> widgets{&input, &composer};
  for (auto* widget : widgets) {
    widget->set_geometry({0, 0, 6, 1});
    widget->set_focused(true);
    REQUIRE(widget->on_event(KeyEvent{.key = Key::Home}));
    widget->set_theme(palette());
    Screen screen{6, 1};
    widget->draw(screen);
    CHECK(screen.at(0, 0).fg == palette().focus_fg);
    CHECK(screen.at(0, 0).bg == palette().focus_bg);
    CHECK(any(screen.at(0, 0).attrs & Attr::Reverse));
    CHECK(screen.at(1, 0).fg == palette().content_fg);
    CHECK(screen.at(1, 0).bg == palette().content_bg);
  }
  CHECK(input.cursor_pos() == 0);
  CHECK(composer.cursor_pos() == 0);
  input.set_text("a\xCC\x81"
                 "b");
  REQUIRE(input.on_event(KeyEvent{.key = Key::Home}));
  REQUIRE(input.on_event(KeyEvent{.key = Key::Right}));
  Screen screen{6, 1};
  input.draw(screen);
  CHECK(input.cursor_pos() == 1);
  CHECK(screen.text_at(1, 0) == "b");
  CHECK(screen.at(1, 0).fg == palette().focus_fg);
  CHECK(any(screen.at(1, 0).attrs & Attr::Reverse));
}

TEST_CASE(
    "Theme application preserves a slider drag and its cancellation origin",
    "[theme][interaction]") {
  Slider slider;
  REQUIRE(slider.configure({0, 10, 5, 1}));
  slider.set_geometry({0, 0, 11, 2});
  slider.set_focused(true);
  int callbacks = 0;
  slider.on_change([&](double) { ++callbacks; });
  REQUIRE(slider.on_event(
      MouseEvent{.x = 10, .y = 1, .button = 0, .pressed = true}));
  REQUIRE(slider.dragging());
  REQUIRE(slider.value() == 10);
  REQUIRE(callbacks == 1);
  slider.set_theme(palette());
  slider.clear_theme();
  CHECK(slider.dragging());
  CHECK(slider.value() == 10);
  CHECK(callbacks == 1);
  REQUIRE(slider.on_event(KeyEvent{.key = Key::Escape}));
  CHECK(slider.value() == 5);
  CHECK(callbacks == 2);
}

TEST_CASE("Themed paints clip without affecting neighboring cells",
          "[theme][clipping]") {
  Label label{"L"};
  Button button{"B"};
  Checkbox check{"C"};
  RadioGroup radio{{"R"}};
  TextInput input;
  Composer composer;
  Slider slider;
  NumericInput numeric;
  const std::array<Widget*, 8> widgets{&label, &button,   &check,  &radio,
                                       &input, &composer, &slider, &numeric};
  for (auto* widget : widgets) {
    widget->set_theme(palette());
    for (const Rect rect : {Rect{1, 1, 1, 1}, Rect{-1, -1, 2, 2},
                            Rect{5, 5, 1, 1}, Rect{0, 0, 0, 0}}) {
      Screen screen{3, 3};
      screen.fill_rect(0, 0, 3, 3, {200, 201, 202}, {});
      widget->set_geometry(rect);
      widget->draw(screen);
      for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
          if (!rect.contains(x, y))
            CHECK(screen.at(x, y).fg == Rgb{200, 201, 202});
    }
  }
}

TEST_CASE(
    "Theme roles reach actual three-tier App writes and idle diffs stay quiet",
    "[theme][app][wire]") {
  for (int tier : {0, 1, 2}) {
    ThemeApp app;
    app.run(tier);
    REQUIRE(app.sink.frames.size() == 4);
    CHECK(app.sink.frames[1].empty());
    CHECK(app.sink.frames[3].empty());
    CHECK(app.authored[0] == app.authored[2]);
    CHECK(app.authored[0].find("[x] Enabled") != std::string::npos);
    CHECK(app.authored[0].find("Invalid") != std::string::npos);
    tfsupport::TerminalGrid terminal{24, 6};
    terminal.feed(app.sink.frames[0]);
    CHECK(terminal.row_text(0).find("Action") != std::string::npos);
    CHECK(terminal.row_text(1).starts_with("[x] Enabled"));
    CHECK(terminal.row_text(3).starts_with("!bad"));
    CHECK(terminal.at(0, 0).bold);
    CHECK(terminal.at(0, 1).bold);
    CHECK(terminal.at(0, 4).bold);
    CHECK(app.sink.frames[0].find("\033[1m") != std::string::npos);
    if (tier == 0) {
      CHECK(app.sink.frames[0].find("38;2;") == std::string::npos);
      CHECK(terminal.at(0, 4).fg == -1);
    } else {
      CHECK(app.sink.frames[0].find("38;2;34;35;36") != std::string::npos);
      CHECK_FALSE(app.sink.frames[2].empty());
      CHECK(terminal.at(0, 4).fg == ((34 << 16) | (35 << 8) | 36));
    }
    for (std::size_t i = 1; i < app.sink.frames.size(); ++i)
      terminal.feed(app.sink.frames[i]);
    CHECK(terminal.row_text(0).find("Action") != std::string::npos);
    CHECK(terminal.at(0, 4).bold);
  }
}
