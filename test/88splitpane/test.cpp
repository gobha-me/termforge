#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/widgets/split_pane.hpp"

using namespace termforge;

namespace {

auto key(Key code, KeyAction action = KeyAction::Press) -> KeyEvent {
  return KeyEvent{.key = code, .action = action};
}

auto pointer(int x, int y, MouseAction action) -> MouseEvent {
  MouseEvent event{.x = x, .y = y, .button = 0};
  event.pressed = action != MouseAction::Release;
  event.motion = action == MouseAction::Drag;
  return event;
}

class Router final : public App {
 public:
  SplitPane pane;
  auto route(const MouseEvent& event) -> bool {
    if (pane.dragging()) return pane.on_event(event);
    return route_mouse(event, {&pane});
  }

 protected:
  auto on_render(Screen&) -> void override {}
};

} // namespace

TEST_CASE(
    "SplitPane configuration and programmatic changes are atomic and silent",
    "[splitpane]") {
  SplitPane pane;
  int callbacks = 0;
  pane.on_change([&](SplitPaneState) { ++callbacks; });
  REQUIRE(pane.configure({SplitDirection::LeftRight, 2, 3, 5}));
  pane.set_geometry({0, 0, 20, 4});
  CHECK(pane.state() == SplitPaneState{5, SplitCollapse::None});

  for (const auto config :
       {SplitPaneConfig{SplitDirection::LeftRight, -1, 3, 5},
        SplitPaneConfig{SplitDirection::LeftRight, 2, -1, 5},
        SplitPaneConfig{SplitDirection::LeftRight, 2, 3, -1},
        SplitPaneConfig{SplitDirection::LeftRight,
                        std::numeric_limits<int>::max(), 1, 5},
        SplitPaneConfig{static_cast<SplitDirection>(99), 2, 3, 5}}) {
    const auto result = pane.configure(config);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    CHECK(pane.configuration() ==
          SplitPaneConfig{SplitDirection::LeftRight, 2, 3, 5});
  }
  CHECK_FALSE(pane.set_preferred_first(-1));
  CHECK_FALSE(pane.set_collapse(static_cast<SplitCollapse>(99)));
  CHECK(pane.state() == SplitPaneState{5, SplitCollapse::None});

  REQUIRE(pane.set_preferred_first(7));
  REQUIRE(pane.set_collapse(SplitCollapse::Second));
  CHECK(pane.state() == SplitPaneState{7, SplitCollapse::Second});
  CHECK(pane.configuration().preferred_first == 7);
  CHECK(callbacks == 0);
}

TEST_CASE(
    "SplitPane layout restores preferred extent after resize and collapse",
    "[splitpane]") {
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 2, 3, 7}));
  pane.set_geometry({2, 4, 20, 5});
  auto rects = pane.layout();
  REQUIRE(rects);
  CHECK(*rects == SplitPaneRects{{2, 4, 7, 5}, {9, 4, 1, 5}, {10, 4, 12, 5}});

  pane.set_geometry({2, 4, 8, 5});
  rects = pane.layout();
  REQUIRE(rects);
  CHECK(rects->first.w == 4);
  CHECK(rects->second.w == 3);
  CHECK(pane.state().preferred_first == 7);
  pane.set_geometry({2, 4, 20, 5});
  CHECK(pane.layout()->first.w == 7);

  REQUIRE(pane.set_collapse(SplitCollapse::First));
  CHECK(pane.layout()->first.w == 0);
  CHECK(pane.layout()->divider.x == 2);
  CHECK(pane.layout()->second.w == 19);
  REQUIRE(pane.set_collapse(SplitCollapse::Second));
  CHECK(pane.layout()->first.w == 19);
  CHECK(pane.layout()->second.w == 0);
  REQUIRE(pane.set_collapse(SplitCollapse::None));
  CHECK(pane.layout()->first.w == 7);

  REQUIRE(pane.configure({SplitDirection::TopBottom, 1, 2, 3}));
  pane.set_geometry({2, 4, 9, 12});
  rects = pane.layout();
  REQUIRE(rects);
  CHECK(*rects == SplitPaneRects{{2, 4, 9, 3}, {2, 7, 9, 1}, {2, 8, 9, 8}});
}

TEST_CASE("SplitPane refuses impossible geometry without partial results",
          "[splitpane][failure]") {
  constexpr int hi = std::numeric_limits<int>::max();
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 2, 3, 4}));
  pane.set_geometry({0, 0, 5, 3});
  auto result = pane.layout();
  REQUIRE_FALSE(result);
  CHECK(result.error().severity == Severity::Warning);
  CHECK_FALSE(pane.focusable());
  CHECK_FALSE(pane.hit_test(2, 0));
  CHECK_FALSE(pane.on_event(key(Key::Right)));
  Screen screen{6, 3};
  pane.draw(screen);
  CHECK(screen.text_at(2, 0).empty());

  REQUIRE(pane.set_collapse(SplitCollapse::First));
  REQUIRE(pane.layout());
  CHECK(pane.layout()->divider.x == 0);
  CHECK(pane.on_event(key(Key::Enter))); // no fit: remains collapsed
  CHECK(pane.state().collapsed == SplitCollapse::First);

  pane.set_geometry({7, 9, 0, 5});
  result = pane.layout();
  REQUIRE(result);
  CHECK(*result == SplitPaneRects{{7, 9, 0, 0}, {7, 9, 0, 0}, {7, 9, 0, 0}});
  pane.set_geometry({hi, 0, 2, 2});
  result = pane.layout();
  REQUIRE_FALSE(result);
  CHECK(result.error().severity == Severity::Warning);

  REQUIRE(pane.configure({SplitDirection::TopBottom, 0, 0, 1}));
  REQUIRE(pane.set_collapse(SplitCollapse::None));
  pane.set_geometry({-hi + 2, 0, hi, 3});
  REQUIRE(pane.layout());
  pane.draw(screen); // loops over physical Screen, not the huge divider
  CHECK(screen.text_at(0, 1) == "─");
  CHECK(screen.text_at(1, 1) == "─");
  CHECK(screen.text_at(2, 1).empty());
}

TEST_CASE("SplitPane keys adjust, collapse, restore and skip no-op callbacks",
          "[splitpane]") {
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 2, 3, 5}));
  pane.set_geometry({0, 0, 12, 3});
  std::vector<SplitPaneState> changes;
  pane.on_change([&](SplitPaneState state) { changes.push_back(state); });
  CHECK(pane.on_event(key(Key::Right)));
  CHECK(pane.state() == SplitPaneState{6, SplitCollapse::None});
  CHECK(pane.on_event(key(Key::Left, KeyAction::Repeat)));
  CHECK(pane.state().preferred_first == 5);
  CHECK(pane.on_event(key(Key::Home)));
  CHECK(pane.state().collapsed == SplitCollapse::First);
  CHECK(pane.on_event(key(Key::Home)));
  CHECK(pane.on_event(key(Key::Enter)));
  CHECK(pane.state().collapsed == SplitCollapse::None);
  CHECK(pane.on_event(key(Key::End)));
  CHECK(pane.state().collapsed == SplitCollapse::Second);
  CHECK(pane.on_event(key(Key::Left)));
  CHECK(pane.state() == SplitPaneState{4, SplitCollapse::None});
  CHECK(changes.size() == 6);
  CHECK_FALSE(pane.on_event(key(Key::Right, KeyAction::Release)));
  CHECK_FALSE(pane.on_event(KeyEvent{.key = Key::Right, .ctrl = true}));
  CHECK_FALSE(pane.on_event(key(Key::Up)));

  REQUIRE(pane.configure({SplitDirection::TopBottom, 1, 1, 3}));
  pane.set_geometry({0, 0, 4, 10});
  CHECK(pane.on_event(key(Key::Down)));
  CHECK(pane.state().preferred_first == 4);
  CHECK_FALSE(pane.on_event(key(Key::Right)));
}

TEST_CASE("App forwarding keeps an off-divider drag local and Escape restores",
          "[splitpane][mouse]") {
  Router app;
  REQUIRE(app.pane.configure({SplitDirection::LeftRight, 2, 3, 5}));
  app.pane.set_geometry({0, 0, 20, 3});
  std::vector<SplitPaneState> changes;
  app.pane.on_change([&](SplitPaneState state) { changes.push_back(state); });
  CHECK_FALSE(app.route(pointer(10, 1, MouseAction::Press)));
  CHECK(app.route(pointer(5, 1, MouseAction::Press)));
  CHECK(app.pane.dragging());
  CHECK_FALSE(app.pane.hit_test(30, 1));
  CHECK(app.route(pointer(30, 1, MouseAction::Drag)));
  CHECK(app.pane.state().preferred_first == 16);
  CHECK(app.route(pointer(-10, 1, MouseAction::Release)));
  CHECK(app.pane.state().preferred_first == 2);
  CHECK_FALSE(app.pane.dragging());
  CHECK(changes.size() == 2);

  CHECK(app.route(pointer(2, 1, MouseAction::Press)));
  CHECK(app.route(pointer(8, 1, MouseAction::Drag)));
  CHECK(app.pane.on_event(key(Key::Escape)));
  CHECK(app.pane.state().preferred_first == 2);
  CHECK_FALSE(app.pane.dragging());
  CHECK(changes.size() == 4);

  CHECK(app.route(pointer(2, 1, MouseAction::Press)));
  CHECK(app.route(pointer(9, 1, MouseAction::Drag)));
  app.pane.set_focused(false);
  CHECK_FALSE(app.pane.dragging());
  CHECK(app.pane.state().preferred_first == 9);
  CHECK(changes.size() == 5);
}

TEST_CASE("SplitPane capture invalidates on geometry, reset and resize",
          "[splitpane][mouse]") {
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 0, 0, 3}));
  pane.set_geometry({0, 0, 10, 2});
  CHECK(pane.on_event(pointer(3, 0, MouseAction::Press)));
  pane.set_geometry({0, 0, 11, 2});
  CHECK_FALSE(pane.dragging());
  CHECK_FALSE(pane.on_event(pointer(7, 0, MouseAction::Drag)));
  CHECK(pane.on_event(pointer(3, 0, MouseAction::Press)));
  pane.reset_transient();
  CHECK_FALSE(pane.dragging());
  CHECK(pane.on_event(pointer(3, 0, MouseAction::Press)));
  CHECK_FALSE(pane.on_event(ResizeEvent{}));
  CHECK_FALSE(pane.dragging());
  CHECK(pane.on_event(pointer(3, 0, MouseAction::Press)));
  REQUIRE(pane.set_preferred_first(4));
  CHECK_FALSE(pane.dragging());
}

TEST_CASE("SplitPane callbacks may reconfigure or delete the divider",
          "[splitpane]") {
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 0, 0, 3}));
  pane.set_geometry({0, 0, 10, 2});
  int calls = 0;
  pane.on_change([&](SplitPaneState) {
    ++calls;
    REQUIRE(pane.configure({SplitDirection::TopBottom, 0, 0, 2}));
    pane.on_change({});
  });
  CHECK(pane.on_event(key(Key::Right)));
  CHECK(calls == 1);
  CHECK(pane.configuration().direction == SplitDirection::TopBottom);

  auto owned = std::make_unique<SplitPane>();
  REQUIRE(owned->configure({SplitDirection::LeftRight, 0, 0, 3}));
  owned->set_geometry({0, 0, 10, 2});
  owned->on_change([&](SplitPaneState) { owned.reset(); });
  CHECK(owned->on_event(key(Key::Right)));
  CHECK_FALSE(owned);
}

TEST_CASE("SplitPane Theme and colorless focus leave pane cells untouched",
          "[splitpane]") {
  SplitPane pane;
  REQUIRE(pane.configure({SplitDirection::LeftRight, 0, 0, 2}));
  pane.set_geometry({0, 0, 5, 2});
  Theme theme;
  theme.glyphs = BorderStyle::Ascii;
  theme.muted = {1, 2, 3};
  theme.focus_bg = {4, 5, 6};
  theme.content_bg = {7, 8, 9};
  pane.set_theme(theme);
  Screen screen{5, 2};
  screen.write_text(0, 0, "A", theme::kFg, theme::kBg);
  screen.write_text(4, 0, "B", theme::kFg, theme::kBg);
  pane.draw(screen);
  CHECK(screen.text_at(0, 0) == "A");
  CHECK(screen.text_at(2, 0) == "|");
  CHECK(screen.text_at(4, 0) == "B");
  CHECK(screen.at(2, 0).fg == theme.muted);
  CHECK(screen.at(2, 0).bg == theme.content_bg);
  CHECK_FALSE(any(screen.at(2, 0).attrs & Attr::Bold));
  pane.set_focused(true);
  pane.draw(screen);
  CHECK(screen.at(2, 0).fg == theme.focus_bg);
  CHECK(any(screen.at(2, 0).attrs & Attr::Bold));

  pane.set_style(BorderStyle::Double);
  pane.set_colors({10, 10, 10}, {11, 11, 11}, {12, 12, 12});
  pane.clear_theme();
  pane.set_theme(theme);
  pane.draw(screen);
  CHECK(screen.text_at(2, 0) == "║");
  CHECK(screen.at(2, 0).fg == Rgb{11, 11, 11});
  CHECK(screen.at(2, 0).bg == Rgb{12, 12, 12});

  SplitPane before_theme;
  REQUIRE(before_theme.configure({SplitDirection::TopBottom, 0, 0, 1}));
  before_theme.set_geometry({0, 0, 5, 3});
  before_theme.set_style(BorderStyle::Ascii);
  before_theme.set_colors({13, 13, 13}, {14, 14, 14}, {15, 15, 15});
  before_theme.set_theme(theme);
  before_theme.draw(screen);
  CHECK(screen.text_at(1, 1) == "-");
  CHECK(screen.at(1, 1).fg == Rgb{13, 13, 13});
  CHECK(screen.at(1, 1).bg == Rgb{15, 15, 15});
}
