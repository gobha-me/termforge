#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/scrollbar.hpp"

using namespace termforge;

namespace {

auto key(Key code, KeyAction action = KeyAction::Press) -> KeyEvent {
  return KeyEvent{.key = code, .action = action};
}

auto press(int x, int y, int button = 0) -> MouseEvent {
  return MouseEvent{.x = x, .y = y, .button = button, .pressed = true};
}

auto wheel(int x, int y, bool up, bool horizontal = false) -> MouseEvent {
  MouseEvent event{.x = x, .y = y, .button = -1};
  if (horizontal) {
    event.scroll_left = up;
    event.scroll_right = !up;
  } else {
    event.scroll_up = up;
    event.scroll_down = !up;
  }
  return event;
}

auto strip(const Screen& screen, Rect track, ScrollOrientation orientation)
    -> std::string {
  std::string result;
  if (orientation == ScrollOrientation::Horizontal) {
    for (int x = track.x; x < track.x + track.w; ++x)
      result += screen.text_at(x, track.y);
  } else {
    for (int y = track.y; y < track.y + track.h; ++y)
      result += screen.text_at(track.x, y);
  }
  return result;
}

} // namespace

TEST_CASE("viewport setter validates atomically and stays callback-silent",
          "[scrollbar]") {
  Scrollbar bar;
  int calls = 0;
  bar.on_change([&](int) { ++calls; });
  REQUIRE(bar.set_viewport({100, 20, 10}));
  CHECK(bar.viewport() == ScrollbarViewport{100, 20, 10});
  CHECK(bar.focusable());

  for (const auto invalid :
       {ScrollbarViewport{-1, 0, 1}, ScrollbarViewport{10, 0, -1},
        ScrollbarViewport{10, -1, 1}, ScrollbarViewport{10, 10, 1},
        ScrollbarViewport{10, 1, 10}}) {
    const auto result = bar.set_viewport(invalid);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    CHECK(bar.viewport() == ScrollbarViewport{100, 20, 10});
  }
  REQUIRE(bar.set_viewport({100, 20, 10}));
  CHECK(calls == 0);
  REQUIRE(bar.set_viewport({8, 0, 10}));
  CHECK_FALSE(bar.focusable());
  REQUIRE(bar.set_viewport({8, 3, 0}));
  CHECK_FALSE(bar.focusable());
  CHECK(calls == 0);
}

TEST_CASE("vertical bar paints one strip, pages and uses discrete keys",
          "[scrollbar]") {
  Scrollbar bar;
  REQUIRE(bar.set_viewport({100, 20, 10}));
  bar.set_style(BorderStyle::Ascii);
  bar.set_geometry({3, 2, 3, 10});
  Screen screen{8, 15};
  bar.draw(screen);
  CHECK(bar.track_rect() == Rect{5, 2, 1, 10});
  CHECK(strip(screen, bar.track_rect(), ScrollOrientation::Vertical) ==
        "||#|||||||");
  CHECK(screen.text_at(3, 2).empty());
  CHECK_FALSE(bar.hit_test(4, 2));
  CHECK(bar.hit_test(5, 2));

  std::vector<int> changes;
  bar.on_change([&](int offset) { changes.push_back(offset); });
  CHECK_FALSE(bar.on_event(press(4, 2)));
  CHECK(bar.on_event(press(5, 4))); // thumb: consumed, no change
  CHECK(changes.empty());
  CHECK(bar.on_event(press(5, 2))); // page above
  CHECK(bar.offset() == 10);
  CHECK(bar.on_event(key(Key::PageDown)));
  CHECK(bar.offset() == 20);
  CHECK(bar.on_event(key(Key::Down, KeyAction::Repeat)));
  CHECK(bar.offset() == 21);
  CHECK(bar.on_event(key(Key::Home)));
  CHECK(bar.offset() == 0);
  CHECK(bar.on_event(key(Key::Home))); // no callback at boundary
  CHECK(bar.on_event(key(Key::End)));
  CHECK(bar.offset() == 90);
  CHECK(bar.on_event(key(Key::End)));
  CHECK(changes == std::vector<int>{10, 20, 21, 0, 90});

  CHECK_FALSE(bar.on_event(key(Key::Down, KeyAction::Release)));
  CHECK_FALSE(bar.on_event(KeyEvent{.key = Key::Down, .ctrl = true}));
  CHECK_FALSE(bar.on_event(key(Key::Right)));
  CHECK_FALSE(bar.on_event(press(5, 2, 2)));
  CHECK_FALSE(bar.on_event(MouseEvent{.x = 5, .y = 2, .button = 0}));
}

TEST_CASE("wheels match the axis and horizontal track accepts vertical wheel",
          "[scrollbar]") {
  Scrollbar bar;
  REQUIRE(bar.set_viewport({100, 50, 10}));
  bar.set_geometry({2, 3, 10, 2});
  bar.set_orientation(ScrollOrientation::Horizontal);
  bar.set_style(BorderStyle::Ascii);
  Screen screen{14, 7};
  bar.draw(screen);
  CHECK(bar.track_rect() == Rect{2, 4, 10, 1});
  CHECK(strip(screen, bar.track_rect(), ScrollOrientation::Horizontal) ==
        "-----#----");
  CHECK_FALSE(bar.hit_test(2, 3));
  CHECK(bar.hit_test(2, 4));
  CHECK_FALSE(bar.on_event(wheel(2, 3, true, true)));
  CHECK(bar.on_event(wheel(2, 4, true, true)));
  CHECK(bar.offset() == 47);
  CHECK(bar.on_event(wheel(2, 4, false)));
  CHECK(bar.offset() == 50);
  CHECK(bar.on_event(key(Key::Left)));
  CHECK(bar.offset() == 49);
  CHECK_FALSE(bar.on_event(key(Key::Up)));
  CHECK(bar.on_event(press(11, 4))); // page after thumb
  CHECK(bar.offset() == 59);

  bar.set_orientation(ScrollOrientation::Vertical);
  CHECK_FALSE(bar.on_event(wheel(11, 3, true, true)));
  CHECK(bar.on_event(wheel(11, 3, true)));
  CHECK(bar.offset() == 56);
}

TEST_CASE("callbacks can reconfigure or destroy their Scrollbar",
          "[scrollbar]") {
  Scrollbar bar;
  REQUIRE(bar.set_viewport({100, 0, 10}));
  bar.set_geometry({0, 0, 1, 10});
  int last = -1;
  bar.on_change([&](int offset) {
    last = offset;
    REQUIRE(bar.set_viewport({20, 0, 5}));
    bar.on_change({});
  });
  CHECK(bar.on_event(key(Key::PageDown)));
  CHECK(last == 10);
  CHECK(bar.viewport() == ScrollbarViewport{20, 0, 5});

  auto owned = std::make_unique<Scrollbar>();
  REQUIRE(owned->set_viewport({100, 0, 10}));
  owned->set_geometry({0, 0, 1, 10});
  owned->on_change([&](int) { owned.reset(); });
  CHECK(owned->on_event(key(Key::PageDown)));
  CHECK_FALSE(owned);
}

TEST_CASE("theme roles, overrides and colorless focus survive", "[scrollbar]") {
  Scrollbar bar;
  REQUIRE(bar.set_viewport({20, 0, 5}));
  bar.set_geometry({0, 0, 1, 5});
  Theme theme;
  theme.muted = {10, 20, 30};
  theme.accent = {40, 50, 60};
  theme.focus_bg = {70, 80, 90};
  theme.content_bg = {1, 2, 3};
  theme.glyphs = BorderStyle::Ascii;
  bar.set_theme(theme);
  Screen screen{1, 5};
  bar.draw(screen);
  CHECK(strip(screen, bar.track_rect(), ScrollOrientation::Vertical) ==
        "#||||");
  CHECK(screen.at(0, 0).fg == theme.accent);
  CHECK(screen.at(0, 1).fg == theme.muted);
  CHECK(screen.at(0, 1).bg == theme.content_bg);
  CHECK_FALSE(any(screen.at(0, 0).attrs & Attr::Bold));

  bar.set_focused(true);
  bar.draw(screen);
  CHECK(screen.at(0, 0).fg == theme.focus_bg);
  CHECK(any(screen.at(0, 0).attrs & Attr::Bold));
  CHECK_FALSE(any(screen.at(0, 1).attrs & Attr::Bold));

  bar.set_style(BorderStyle::Single);
  bar.set_colors({1, 1, 1}, {2, 2, 2}, {3, 3, 3}, {4, 4, 4});
  bar.clear_theme();
  bar.set_theme(theme);
  bar.draw(screen);
  CHECK(screen.text_at(0, 1) == "│");
  CHECK(screen.at(0, 0).fg == Rgb{3, 3, 3});
  CHECK(screen.at(0, 1).fg == Rgb{1, 1, 1});
  CHECK(screen.at(0, 1).bg == Rgb{4, 4, 4});
  CHECK(any(screen.at(0, 0).attrs & Attr::Bold));
}

TEST_CASE("empty, offscreen and unrepresentable tracks do no unsafe work",
          "[scrollbar]") {
  constexpr int hi = std::numeric_limits<int>::max();
  Scrollbar bar;
  REQUIRE(bar.set_viewport({100, 20, 10}));
  bar.set_geometry({0, 0, 0, 10});
  CHECK_FALSE(bar.on_event(key(Key::Down)));
  Screen screen{4, 4};
  bar.draw(screen);
  CHECK(screen.text_at(0, 0).empty());

  bar.set_geometry({hi, 0, 2, 10});
  CHECK(bar.track_rect().empty());
  CHECK_FALSE(bar.on_event(key(Key::Down)));
  bar.draw(screen);
  CHECK(screen.text_at(0, 0).empty());

  bar.set_geometry({-hi + 2, 0, hi, 4});
  CHECK(bar.track_rect() == Rect{1, 0, 1, 4});
  bar.draw(screen);
  CHECK_FALSE(screen.text_at(1, 0).empty());
}
