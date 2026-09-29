#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "slider_app.hpp"
#include "support/apc.hpp"
#include "support/screen.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/slider.hpp"

using namespace termforge;
using Catch::Approx;

namespace {
auto key(Key code, KeyAction action = KeyAction::Press) -> KeyEvent {
  return KeyEvent{.key = code, .action = action};
}
auto press(int x, int y) -> MouseEvent {
  return MouseEvent{.x = x, .y = y, .button = 0, .pressed = true};
}
auto drag(int x, int y) -> MouseEvent {
  return MouseEvent{.x = x, .y = y, .button = 0, .motion = true};
}
auto release(int x, int y) -> MouseEvent {
  return MouseEvent{.x = x, .y = y, .button = 0};
}
auto text(const Screen& screen) -> std::string {
  std::string result;
  for (int y = 0; y < screen.rows(); ++y)
    result += tfsupport::row_text(screen, y) + '\n';
  return result;
}
auto prepared() -> Slider {
  Slider slider;
  REQUIRE(slider.configure({0, 10, 5, 3}));
  slider.set_geometry({2, 2, 11, 2});
  return slider;
}

struct RecordingSink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
class DemoHarness final : public examples::SliderDemo {
 public:
  ~DemoHarness() override { set_clock(nullptr); }
  RecordingSink output;
  std::vector<std::string> input, screens;
  std::vector<double> values, previews;
  std::vector<bool> captured;
  auto drive(int tier, int frames) -> void {
    set_clock(&clock);
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(frames, 40, 14, nullptr, std::move(selected));
    set_clock(nullptr);
  }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&output);
    // test_run_frames selects a driver but intentionally skips terminal
    // setup/on_start. Exercise the real example's startup against that driver.
    if (frame == 0) examples::SliderDemo::on_start();
    examples::SliderDemo::on_render(screen);
    screens.push_back(text(screen));
    values.push_back(slider().value());
    previews.push_back(brightness());
    captured.push_back(slider().dragging());
    ++frame;
  }
  auto read_available(char* out, int capacity) -> int override {
    if (input_frame != frame) {
      input_frame = frame;
      pending = frame < static_cast<int>(input.size())
                    ? input[static_cast<std::size_t>(frame)]
                    : "";
    }
    const int count = std::min(capacity, static_cast<int>(pending.size()));
    std::copy_n(pending.begin(), count, out);
    pending.erase(0, static_cast<std::size_t>(count));
    return count;
  }

 private:
  SyntheticClock clock;
  int frame{0}, input_frame{-1};
  std::string pending;
};
auto mouse_wire(int button, int x, int y, char final = 'M') -> std::string {
  return "\033[<" + std::to_string(button) + ";" + std::to_string(x + 1) + ";" +
         std::to_string(y + 1) + final;
}
auto actions(std::string_view wire, std::string_view action) -> int {
  int count = 0;
  for (const auto& record : tfsupport::apcs(wire))
    if (tfsupport::key_value(record, "a") == action &&
        tfsupport::has_key(record, "i"))
      ++count;
  return count;
}
} // namespace

TEST_CASE("Slider rejects invalid configurations atomically including capture",
          "[slider][failure]") {
  auto slider = prepared();
  int notifications = 0;
  slider.on_change([&](double) { ++notifications; });
  REQUIRE(slider.on_event(press(2, 3)));
  const auto previous = slider.configuration();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const double huge = std::numeric_limits<double>::max();
  for (const auto config :
       {SliderConfig{nan, 10, 0, 1}, SliderConfig{0, inf, 0, 1},
        SliderConfig{0, 10, nan, 1}, SliderConfig{0, 10, 0, inf},
        SliderConfig{10, 0, 5, 1}, SliderConfig{0, 10, 0, 0},
        SliderConfig{0, 10, 0, -1}, SliderConfig{0, 10, -1, 1},
        SliderConfig{0, 10, 11, 1}, SliderConfig{-huge, huge, 0, 1},
        SliderConfig{0, huge, 0, std::numeric_limits<double>::denorm_min()}}) {
    const auto result = slider.configure(config);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    CHECK(slider.configuration() == previous);
    CHECK(slider.dragging());
  }
  for (double value : {nan, inf, -inf, -1.0, 11.0}) {
    REQUIRE_FALSE(slider.set_value(value));
    CHECK(slider.configuration() == previous);
    CHECK(slider.dragging());
  }
  CHECK(notifications == 1);
  REQUIRE(slider.set_value(1.25));
  CHECK_FALSE(slider.dragging());
  CHECK(notifications == 1);
  REQUIRE(slider.configure({2, 2, 2, 1}));
  CHECK(slider.on_event(key(Key::End)));
  CHECK(slider.value() == 2);
  CHECK(notifications == 1);
}

TEST_CASE("Slider keyboard endpoints odd steps and releases do not lie",
          "[slider][input]") {
  auto slider = prepared();
  std::vector<double> changes;
  slider.on_change([&](double value) { changes.push_back(value); });
  REQUIRE(slider.on_event(key(Key::Right)));
  REQUIRE(slider.on_event(key(Key::Up, KeyAction::Repeat)));
  REQUIRE(slider.on_event(key(Key::Right)));
  REQUIRE(slider.on_event(key(Key::Left)));
  REQUIRE(slider.on_event(key(Key::Home)));
  REQUIRE(slider.on_event(key(Key::Left)));
  REQUIRE(slider.on_event(key(Key::End)));
  CHECK(changes == std::vector<double>{8, 10, 7, 0, 10});
  CHECK_FALSE(slider.on_event(key(Key::Right, KeyAction::Release)));
  CHECK_FALSE(slider.on_event(KeyEvent{.key = Key::Left, .ctrl = true}));
  CHECK_FALSE(slider.on_event(key(Key::Tab)));
  CHECK_FALSE(slider.on_event(key(Key::Enter)));
  CHECK(slider.value() == 10);
  Screen screen{20, 5};
  slider.draw(screen);
  CHECK_FALSE(slider.dirty());
  REQUIRE(slider.set_value(10));
  CHECK_FALSE(slider.dirty());
  REQUIRE(slider.on_event(key(Key::End)));
  CHECK_FALSE(slider.dirty());
  const double huge = std::numeric_limits<double>::max();
  REQUIRE(slider.configure({huge / 2, huge, huge / 2, huge}));
  REQUIRE(slider.on_event(key(Key::Right)));
  CHECK(slider.value() == huge);
  REQUIRE(slider.on_event(key(Key::Left)));
  CHECK(slider.value() == huge / 2);
}

TEST_CASE("Slider pointer snaps and captures beyond bounds without treating "
          "motion as release",
          "[slider][mouse]") {
  auto slider = prepared();
  std::vector<double> changes;
  slider.on_change([&](double value) { changes.push_back(value); });
  REQUIRE(slider.on_event(press(7, 3)));
  CHECK(slider.value() == 6);
  REQUIRE(slider.dragging());
  REQUIRE(slider.on_event(drag(200, 200)));
  CHECK(slider.value() == 10);
  CHECK(slider.dragging());
  REQUIRE(slider.on_event(drag(-200, -200)));
  CHECK(slider.value() == 0);
  CHECK(slider.dragging());
  CHECK_FALSE(
      slider.on_event(MouseEvent{.x = 7, .y = 3, .button = 3, .motion = true}));
  CHECK(slider.dragging());
  REQUIRE(slider.on_event(release(7, 50)));
  CHECK_FALSE(slider.dragging());
  CHECK(slider.value() == 6);
  CHECK(changes == std::vector<double>{6, 10, 0, 6});
  CHECK_FALSE(slider.on_event(drag(2, 3)));
  CHECK_FALSE(slider.on_event(release(2, 3)));
  CHECK_FALSE(slider.on_event(press(7, 2))); // label is not the track
  CHECK_FALSE(slider.on_event(
      MouseEvent{.x = 2, .y = 3, .button = 2, .pressed = true}));
  CHECK_FALSE(slider.on_event(
      MouseEvent{.x = 2, .y = 3, .button = -1, .scroll_right = true}));
}

TEST_CASE("Slider cancellation restores only on a user boundary",
          "[slider][failure][mouse]") {
  auto slider = prepared();
  std::vector<double> changes;
  slider.on_change([&](double value) { changes.push_back(value); });
  REQUIRE(slider.on_event(press(2, 3)));
  REQUIRE(slider.on_event(key(Key::Escape)));
  CHECK(slider.value() == 5);
  CHECK(changes == std::vector<double>{0, 5});
  slider.cancel_drag();
  CHECK(changes.size() == 2);
  REQUIRE(slider.on_event(press(12, 3)));
  slider.set_focused(false);
  CHECK_FALSE(slider.dragging());
  CHECK(slider.value() == 10);
  REQUIRE(slider.on_event(press(2, 3)));
  slider.reset_transient();
  slider.reset_transient();
  CHECK(slider.value() == 0);
  CHECK(changes == std::vector<double>{0, 5, 10, 0});
  REQUIRE(slider.on_event(press(12, 3)));
  slider.set_geometry({0, 0, 1, 1});
  CHECK_FALSE(slider.dragging());
  slider.cancel_drag();
  CHECK(slider.value() == 10);
  Screen screen{20, 5};
  slider.draw(screen);
  CHECK(changes.size() == 5);
  REQUIRE(slider.on_event(press(0, 0)));
  CHECK_FALSE(slider.on_event(ResizeEvent{10, 10}));
  CHECK_FALSE(slider.dragging());
  CHECK(slider.value() == 10);
}

TEST_CASE("Slider pointer arithmetic includes negative offsets fractional and "
          "oversized steps",
          "[slider][numeric]") {
  auto slider = prepared();
  REQUIRE(slider.configure({-2, 2, 0, 0.75}));
  REQUIRE(slider.on_event(press(7, 3)));
  CHECK(slider.value() == Approx(0.25));
  slider.stop_drag();
  REQUIRE(slider.on_event(press(12, 3)));
  CHECK(slider.value() == 2);
  slider.stop_drag();
  REQUIRE(slider.configure({0, std::numeric_limits<double>::denorm_min(), 0,
                            std::numeric_limits<double>::max()}));
  REQUIRE(slider.on_event(press(7, 3)));
  CHECK(slider.value() == 0);
  REQUIRE(slider.on_event(release(12, 3)));
  CHECK(slider.value() == std::numeric_limits<double>::denorm_min());
  REQUIRE(slider.configure({1e300, 1e300 + 1e285, 1e300, 1}));
  REQUIRE(slider.on_event(key(Key::Right)));
  CHECK(slider.value() == 1e300); // below this double's representable spacing
  const double huge = std::numeric_limits<double>::max();
  REQUIRE(slider.configure({-huge / 2, huge / 2, 0, huge / 8}));
  const int edge = std::numeric_limits<int>::max();
  slider.set_geometry({-edge, 2, edge, 2});
  REQUIRE(slider.on_event(press(-edge, 3)));
  CHECK(slider.value() == -huge / 2);
  REQUIRE(slider.on_event(drag(edge, 3)));
  CHECK(slider.value() == huge / 2);
  REQUIRE(slider.on_event(drag(std::numeric_limits<int>::min(), 3)));
  CHECK(slider.value() == -huge / 2);
  REQUIRE(slider.on_event(release(-edge / 2, 3)));
  CHECK(slider.value() == 0);
  CHECK_FALSE(slider.dragging());
}

TEST_CASE("Slider callbacks may replace configure reset or destroy the control",
          "[slider][callback][failure]") {
  auto slider = prepared();
  std::vector<double> notifications;
  slider.on_change([&](double value) {
    notifications.push_back(value);
    slider.on_change(
        [&](double other) { notifications.push_back(other * 10); });
    REQUIRE(slider.configure({0, 100, 50, 10}));
  });
  REQUIRE(slider.on_event(press(2, 3)));
  CHECK_FALSE(slider.dragging());
  CHECK(slider.value() == 50);
  REQUIRE(slider.on_event(key(Key::Right)));
  CHECK(notifications == std::vector<double>{0, 600});
  for (const Event& event :
       {Event{key(Key::Right)}, Event{press(2, 3)}, Event{drag(12, 3)},
        Event{release(12, 3)}, Event{key(Key::Escape)}}) {
    auto owned = std::make_unique<Slider>(prepared());
    if (std::holds_alternative<MouseEvent>(event) &&
        std::get<MouseEvent>(event).action() != MouseAction::Press)
      REQUIRE(owned->on_event(press(2, 3)));
    if (std::holds_alternative<KeyEvent>(event) &&
        std::get<KeyEvent>(event).key == Key::Escape)
      REQUIRE(owned->on_event(press(2, 3)));
    owned->on_change([&](double) { owned.reset(); });
    auto* raw = owned.get();
    CHECK(raw->on_event(event));
    CHECK_FALSE(owned);
  }
}

TEST_CASE("Slider owns clipped tiny geometry and ASCII focus cues",
          "[slider][draw][failure]") {
  for (const Rect area :
       {Rect{2, 1, 11, 2}, Rect{-3, 1, 11, 3}, Rect{6, 3, 8, 4},
        Rect{0, 0, 1, 1}, Rect{0, 0, 0, 1}, Rect{0, 0, 1, 0},
        Rect{std::numeric_limits<int>::max(), 0, 8, 1},
        Rect{0, std::numeric_limits<int>::max(), 8, 4}}) {
    auto slider = prepared();
    slider.set_style(BorderStyle::Ascii);
    slider.set_geometry(area);
    slider.set_label("Load界\033[2J");
    Screen screen{12, 5};
    for (int y = 0; y < 5; ++y)
      screen.write_text(0, y, "!!!!!!!!!!!!", theme::kFg, theme::kBg);
    slider.draw(screen);
    for (int y = 0; y < 5; ++y)
      for (int x = 0; x < 12; ++x)
        if (!area.contains(x, y)) CHECK(screen.text_at(x, y) == "!");
    CHECK(text(screen).find('\033') == std::string::npos);
    CHECK_FALSE(slider.dirty());
    slider.draw(screen);
    CHECK_FALSE(slider.dirty());
  }
  auto slider = prepared();
  slider.set_style(BorderStyle::Ascii);
  slider.set_geometry({0, 0, 11, 2});
  Screen screen{11, 2};
  slider.draw(screen);
  CHECK(screen.text_at(5, 1) == "#");
  slider.set_focused(true);
  slider.draw(screen);
  CHECK(screen.text_at(0, 0) == ">");
  CHECK(screen.text_at(5, 1) == "*");
  CHECK(text(screen).find('5') != std::string::npos);
  CHECK(std::ranges::all_of(text(screen),
                            [](unsigned char c) { return c < 128; }));
  slider.set_label("A caption too long for the control");
  slider.set_geometry({0, 0, 4, 2});
  Screen narrow{4, 2};
  slider.draw(narrow);
  CHECK(narrow.text_at(2, 0) == "5");
  REQUIRE(slider.set_value(7));
  slider.draw(narrow);
  CHECK(narrow.text_at(2, 0) == "7");
  slider.set_geometry({0, 0, 4, 1});
  slider.draw(narrow);
  CHECK(narrow.text_at(0, 0) == "7");
  slider.set_geometry({0, 0, 1, 2});
  slider.draw(narrow);
  CHECK(narrow.text_at(0, 0) == "7");
  REQUIRE(slider.set_value(5));
  slider.set_geometry({0, 0, 1, 1});
  REQUIRE(slider.on_event(press(0, 0)));
  CHECK(slider.value() == 5);
  REQUIRE(slider.on_event(key(Key::End)));
  CHECK(slider.value() == 10);
  slider.set_geometry({0, 0, 0, 0});
  CHECK_FALSE(slider.on_event(key(Key::Home)));
}

TEST_CASE("Slider functional example drives captured decoded mouse and real "
          "preview on all tiers",
          "[slider][app][drivers]") {
  for (int tier = 0; tier < 3; ++tier) {
    DemoHarness app;
    app.input = {"",
                 "\033[C",
                 mouse_wire(0, 1, 3),
                 mouse_wire(32, 100, 50),
                 mouse_wire(0, 1, 3, 'm'),
                 mouse_wire(0, 38, 3),
                 "\033[27u",
                 "\033[F",
                 ""};
    app.drive(tier, 9);
    CHECK(app.values ==
          std::vector<double>{50, 55, 0, 100, 0, 100, 0, 100, 100});
    CHECK(app.previews == app.values);
    CHECK(app.captured == std::vector<bool>{false, false, true, true, false,
                                            true, false, false, false});
    CHECK(app.screens[3].find("100") != std::string::npos);
    if (tier == 0) {
      CHECK(app.slider().style() == BorderStyle::Ascii);
      CHECK(std::ranges::all_of(app.output.frames[0],
                                [](unsigned char c) { return c < 128; }));
      CHECK(app.output.frames[0].find("\033_G") == std::string::npos);
    }
    if (tier == 2) {
      CHECK(actions(app.output.frames[0], "t") == 1);
      CHECK(actions(app.output.frames[1], "f") == 1);
      CHECK(actions(app.output.frames[8], "t") == 0);
      CHECK(actions(app.output.frames[8], "f") == 0);
    }
  }
}
