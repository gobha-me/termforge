#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "numeric_settings_app.hpp"
#include "support/apc.hpp"
#include "support/screen.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/numeric_input.hpp"

using namespace termforge;
namespace {
auto key(Key code, KeyAction action = KeyAction::Press) -> KeyEvent {
  return {.key = code, .action = action};
}
auto character(char32_t ch) -> KeyEvent {
  return {.key = Key::Char, .ch = ch};
}
auto integer(const NumericInput& input) -> std::int64_t {
  return std::get<std::int64_t>(input.value());
}
auto decimal(const NumericInput& input) -> double {
  return std::get<double>(input.value());
}
auto prepared() -> NumericInput {
  NumericInput input;
  REQUIRE(input.configure(IntegerInputConfig{-10, 10, 5, 3}));
  input.set_geometry({2, 2, 20, 3});
  input.set_focused(true);
  return input;
}
auto screen_text(const Screen& screen) -> std::string {
  std::string result;
  for (int y = 0; y < screen.rows(); ++y)
    result += tfsupport::row_text(screen, y) + '\n';
  return result;
}
} // namespace

TEST_CASE("NumericInput configuration is typed atomic and silent",
          "[numeric][failure]") {
  auto input = prepared();
  int changes = 0;
  input.on_change([&](NumericValue) { ++changes; });
  REQUIRE(input.set_draft("invalid"));
  const auto before = input.configuration();
  const auto error = input.error()->message;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const NumericInputConfig& cfg :
       {NumericInputConfig{IntegerInputConfig{10, -10, 0, 1}},
        NumericInputConfig{IntegerInputConfig{0, 10, 11, 1}},
        NumericInputConfig{IntegerInputConfig{0, 10, 0, 0}},
        NumericInputConfig{IntegerInputConfig{0, 10, 0, -1}},
        NumericInputConfig{DecimalInputConfig{nan, 10, 0, 1}},
        NumericInputConfig{DecimalInputConfig{0, inf, 0, 1}},
        NumericInputConfig{DecimalInputConfig{0, 10, nan, 1}},
        NumericInputConfig{DecimalInputConfig{0, 10, 0, inf}},
        NumericInputConfig{DecimalInputConfig{0, 10, 0, 0}}}) {
    auto result = input.configure(cfg);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    CHECK(input.configuration() == before);
    CHECK(input.draft() == "invalid");
    CHECK(input.error()->message == error);
  }
  REQUIRE_FALSE(input.set_value(5.0)); // no implicit decimal -> integer
  REQUIRE_FALSE(input.set_value(std::int64_t{11}));
  CHECK(input.draft() == "invalid");
  REQUIRE(input.set_value(std::int64_t{5})); // unchanged model repairs draft
  CHECK(input.draft() == "5");
  CHECK_FALSE(input.error());
  REQUIRE(input.configure(DecimalInputConfig{-1, 1, 0.25, 0.1}));
  REQUIRE_FALSE(input.set_value(std::int64_t{0}));
  REQUIRE_FALSE(input.set_value(inf));
  REQUIRE_FALSE(input.set_value(nan));
  CHECK(decimal(input) == 0.25);
  CHECK(changes == 0);
}

TEST_CASE("NumericInput integer parsing and stepping never round or overflow",
          "[numeric][integer]") {
  NumericInput input;
  const auto low = std::numeric_limits<std::int64_t>::min();
  const auto high = std::numeric_limits<std::int64_t>::max();
  REQUIRE(input.configure(IntegerInputConfig{low, high, 0, 1}));
  std::vector<NumericValue> changes;
  input.on_change([&](NumericValue value) { changes.push_back(value); });
  for (const auto& draft :
       {"", "+", "-", "1.0", "1e0", "0x10", "nan", "inf", "+-1", "++1", " 1",
        "1 ", "1x", "9223372036854775808", "-9223372036854775809"}) {
    REQUIRE(input.set_draft(draft));
    REQUIRE_FALSE(input.commit());
    CHECK(input.draft() == draft);
    CHECK(integer(input) == 0);
    REQUIRE(input.error());
    CHECK(input.error()->severity == Severity::Warning);
  }
  CHECK(changes.empty());
  REQUIRE(input.set_draft("-9223372036854775808"));
  REQUIRE(input.commit());
  CHECK(integer(input) == low);
  REQUIRE_FALSE(input.step_down());
  CHECK(integer(input) == low);
  REQUIRE(input.step_up());
  CHECK(integer(input) == low + 1);
  REQUIRE(input.set_draft("+9223372036854775807"));
  REQUIRE(input.commit());
  CHECK(integer(input) == high);
  REQUIRE_FALSE(input.step_up());
  CHECK(integer(input) == high);
  REQUIRE(input.step_down());
  CHECK(integer(input) == high - 1);
  REQUIRE(input.configure(IntegerInputConfig{low, high, low, high}));
  REQUIRE(input.step_up());
  CHECK(integer(input) == -1);
  REQUIRE(input.step_up());
  CHECK(integer(input) == high - 1);
  REQUIRE_FALSE(input.step_up());
}

TEST_CASE("NumericInput decimal dot exponent roundtrip and finite boundaries",
          "[numeric][decimal]") {
  NumericInput input;
  const double high = std::numeric_limits<double>::max();
  REQUIRE(input.configure(DecimalInputConfig{-high, high, 0, 0.25}));
  for (const auto& draft :
       {"", ".", "+", "-.", "1e", "1e+", "+-1", "0x1p2", "nan", "NaN(1)", "inf",
        "-infinity", "1e999", "1e-999", "1,5", "1.5x", " 1", "1 "}) {
    REQUIRE(input.set_draft(draft));
    REQUIRE_FALSE(input.commit());
    CHECK(decimal(input) == 0);
    CHECK(input.draft() == draft);
  }
  REQUIRE(input.set_draft("+.5"));
  REQUIRE(input.commit());
  CHECK(decimal(input) == 0.5);
  REQUIRE(input.set_draft("1."));
  REQUIRE(input.commit());
  CHECK(decimal(input) == 1);
  REQUIRE(input.set_draft("-2.5E-1"));
  REQUIRE(input.commit());
  CHECK(decimal(input) == -0.25);
  for (double number :
       {high, -high, std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(), -0.0, 1e300, 0.1}) {
    REQUIRE(input.set_value(number));
    const auto parsed = input.draft_value();
    REQUIRE(parsed);
    CHECK(std::get<double>(*parsed) == number);
  }
  REQUIRE(input.configure(DecimalInputConfig{-high, high, 0, high}));
  REQUIRE(input.step_up());
  CHECK(decimal(input) == high);
  REQUIRE_FALSE(input.step_up());
  CHECK(std::isfinite(decimal(input)));
  REQUIRE(input.step_down());
  REQUIRE(input.step_down());
  CHECK(decimal(input) == -high);
  REQUIRE_FALSE(input.step_down());
  REQUIRE(input.configure(DecimalInputConfig{1e300, 1e300 + 1e285, 1e300, 1}));
  int changes = 0;
  input.on_change([&](NumericValue) { ++changes; });
  REQUIRE(input.step_up()); // below representable spacing: no invented change
  CHECK(changes == 0);
}

TEST_CASE("NumericInput drafts commit once refuse rather than clamp and cancel "
          "silently",
          "[numeric][input]") {
  auto input = prepared();
  std::vector<std::int64_t> changes;
  input.on_change([&](NumericValue value) {
    changes.push_back(std::get<std::int64_t>(value));
  });
  REQUIRE(input.on_event(key(Key::Up)));
  CHECK(integer(input) == 8);
  REQUIRE(input.on_event(key(Key::Up, KeyAction::Repeat)));
  CHECK(integer(input) == 8);
  REQUIRE(input.error());
  CHECK(input.draft() == "8");
  REQUIRE(input.set_draft("4"));
  REQUIRE(input.on_event(key(Key::Down)));
  CHECK(integer(input) == 1);
  CHECK(changes == std::vector<std::int64_t>{8, 1});
  REQUIRE(input.set_draft("+0001"));
  REQUIRE(input.on_event(key(Key::Enter)));
  CHECK(input.draft() == "1");
  CHECK(changes.size() == 2);
  REQUIRE(input.set_draft("11"));
  REQUIRE(input.on_event(key(Key::Enter)));
  CHECK(input.draft() == "11");
  CHECK(integer(input) == 1);
  input.set_focused(false);
  input.reset_transient();
  CHECK(input.draft() == "11");
  input.set_focused(true);
  REQUIRE(input.on_event(key(Key::Escape)));
  CHECK(input.draft() == "1");
  CHECK_FALSE(input.error());
  CHECK_FALSE(input.on_event(key(Key::Escape)));
  CHECK_FALSE(input.on_event(key(Key::Tab)));
  CHECK_FALSE(input.on_event(key(Key::Up, KeyAction::Release)));
  CHECK_FALSE(input.on_event(KeyEvent{.key = Key::Up, .ctrl = true}));
  CHECK(changes.size() == 2);
}

TEST_CASE("NumericInput paste splices at the cursor without committing",
          "[numeric][paste]") {
  auto input = prepared();
  int changes = 0;
  input.on_change([&](NumericValue) { ++changes; });
  REQUIRE(input.set_draft("12"));
  REQUIRE(input.on_event(key(Key::Home)));
  REQUIRE(input.on_event(key(Key::Right)));
  REQUIRE(input.on_event(PasteEvent{"0"}));
  CHECK(input.draft() == "102");
  CHECK(input.cursor_pos() == 2);
  CHECK(integer(input) == 5);
  REQUIRE(input.on_event(key(Key::Delete)));
  CHECK(input.draft() == "10");
  REQUIRE(input.on_event(key(Key::Enter)));
  CHECK(integer(input) == 10);
  CHECK(changes == 1);
  REQUIRE(input.on_event(PasteEvent{"界"}));
  CHECK(input.draft() == "10界");
  REQUIRE(input.on_event(key(Key::Enter)));
  CHECK(integer(input) == 10);
  REQUIRE(input.on_event(key(Key::Backspace)));
  CHECK(input.draft() == "10");
  REQUIRE(input.on_event(PasteEvent{"\033[987J"}));
  CHECK(input.draft() == "10\033[987J");
  REQUIRE(input.error());
  Screen screen{26, 7};
  input.draw(screen);
  CHECK(screen_text(screen).find('\033') == std::string::npos);
  CHECK(integer(input) == 10);
  CHECK(changes == 1);
}

TEST_CASE(
    "TextInput atomic cursor setter preserves state on invalid boundaries",
    "[numeric][editor][failure]") {
  TextInput editor;
  editor.set_geometry({0, 0, 8, 1});
  editor.set_focused(true);
  int changes = 0;
  editor.on_change([&](const std::string&) { ++changes; });
  REQUIRE(editor.set_text("a界b", 1));
  for (int cursor : {-1, 2, 3, 6}) {
    REQUIRE_FALSE(editor.set_text("a界b", cursor));
    CHECK(editor.text() == "a界b");
    CHECK(editor.cursor_pos() == 1);
  }
  CHECK(changes == 0);
  REQUIRE(editor.on_event(character('X')));
  CHECK(editor.text() == "aX界b");
  CHECK(changes == 1);
  REQUIRE(editor.set_text("", 0));
  CHECK(editor.cursor_pos() == 0);
  CHECK(changes == 1);
}

TEST_CASE("NumericInput callbacks can reconfigure replace or destroy",
          "[numeric][callback][failure]") {
  auto input = prepared();
  std::vector<NumericValue> values;
  input.on_change([&](NumericValue value) {
    values.push_back(value);
    REQUIRE(input.configure(DecimalInputConfig{0, 1, 0.5, 0.1}));
    input.on_change([&](NumericValue next) { values.push_back(next); });
  });
  REQUIRE(input.set_draft("7"));
  REQUIRE(input.commit());
  CHECK(decimal(input) == 0.5);
  REQUIRE(input.set_draft("0.75"));
  REQUIRE(input.commit());
  CHECK(values == std::vector<NumericValue>{std::int64_t{7}, 0.75});
  for (Key code : {Key::Enter, Key::Up, Key::Down}) {
    auto owned = std::make_unique<NumericInput>(prepared());
    REQUIRE(owned->set_draft("4"));
    owned->on_change([&](NumericValue) { owned.reset(); });
    auto* raw = owned.get();
    CHECK(raw->on_event(key(code)));
    CHECK_FALSE(owned);
  }
}

TEST_CASE("NumericInput owns tiny clipped geometry and colorless error focus",
          "[numeric][draw][failure]") {
  for (const Rect area :
       {Rect{2, 1, 11, 3}, Rect{-3, 1, 11, 3}, Rect{7, 3, 10, 4},
        Rect{0, 0, 1, 1}, Rect{0, 0, 2, 2}, Rect{0, 0, 0, 1}, Rect{0, 0, 1, 0},
        Rect{std::numeric_limits<int>::max() - 1, 1, 8, 3},
        Rect{0, std::numeric_limits<int>::max(), 8, 3}}) {
    auto input = prepared();
    input.set_geometry(area);
    input.set_label("Gain界\033[987J");
    REQUIRE(input.set_draft("bad"));
    Screen screen{12, 5};
    for (int y = 0; y < 5; ++y)
      screen.write_text(0, y, "!!!!!!!!!!!!", theme::kFg, theme::kBg);
    input.draw(screen);
    for (int y = 0; y < 5; ++y)
      for (int x = 0; x < 12; ++x)
        if (!area.contains(x, y)) CHECK(screen.text_at(x, y) == "!");
    CHECK(screen_text(screen).find('\033') == std::string::npos);
    CHECK(input.draft() == "bad");
    CHECK(integer(input) == 5);
    CHECK_FALSE(input.dirty());
  }
  auto input = prepared();
  input.set_geometry({0, 0, 12, 3});
  Screen screen{12, 3};
  input.draw(screen);
  CHECK(screen.text_at(0, 1) == ">");
  REQUIRE(input.set_draft("?"));
  input.draw(screen);
  CHECK(screen.text_at(0, 1) == "!");
  CHECK_FALSE(tfsupport::row_text(screen, 2).empty());
  input.set_geometry({0, 0, 0, 0});
  CHECK_FALSE(input.on_event(key(Key::Enter)));
  CHECK_FALSE(input.on_event(PasteEvent{"1"}));
}

TEST_CASE("TextInput escaped display aligns raw cursor and clips tokens",
          "[numeric][textinput][render][failure]") {
  TextInput input;
  input.set_geometry({0, 0, 16, 1});
  input.set_focused(true);
  REQUIRE(input.set_display_mode(text::SanitizeMode::Escape));
  const std::string raw = "a\033[987Jb";
  REQUIRE(input.set_text(raw, static_cast<int>(raw.size())));
  Screen screen{18, 1};
  input.draw(screen);
  CHECK(tfsupport::row_text(screen, 0).starts_with("a^[[987Jb"));
  CHECK(any(screen.at(9, 0).attrs & Attr::Reverse));
  REQUIRE(
      input.on_event(MouseEvent{.x = 2, .y = 0, .button = 0, .pressed = true}));
  CHECK(input.cursor_pos() == 1);
  input.draw(screen);
  CHECK(any(screen.at(1, 0).attrs & Attr::Reverse));
  CHECK(any(screen.at(2, 0).attrs & Attr::Reverse));
  const auto refusal =
      input.set_display_mode(static_cast<text::SanitizeMode>(99));
  REQUIRE_FALSE(refusal);
  CHECK(refusal.error().severity == Severity::Warning);
  CHECK(input.text() == raw);
  input.draw(screen);
  CHECK(tfsupport::row_text(screen, 0).starts_with("a^[[987Jb"));
  input.set_geometry({0, 0, 4, 1});
  REQUIRE(input.set_text(raw, static_cast<int>(raw.size())));
  Screen scrolled{4, 1};
  input.draw(scrolled);
  CHECK(tfsupport::row_text(scrolled, 0).starts_with("7Jb"));
  CHECK(any(scrolled.at(3, 0).attrs & Attr::Reverse));
  REQUIRE(
      input.on_event(MouseEvent{.x = 0, .y = 0, .button = 0, .pressed = true}));
  CHECK(input.cursor_pos() == 5);
  for (const std::string& token :
       {std::string{"\033"}, std::string{"界"}, std::string(1, '\x9b')}) {
    REQUIRE(input.set_text(token, 0));
    input.set_geometry({0, 0, 1, 1});
    Screen tiny{2, 1};
    tiny.write_text(0, 0, "!!", theme::kFg, theme::kBg);
    input.draw(tiny);
    CHECK(tiny.text_at(1, 0) == "!");
    CHECK(any(tiny.at(0, 0).attrs & Attr::Reverse));
  }
  REQUIRE(input.set_display_mode(text::SanitizeMode::Strip));
  input.set_geometry({0, 0, 16, 1});
  REQUIRE(input.set_text(raw, static_cast<int>(raw.size())));
  input.draw(screen);
  CHECK(tfsupport::row_text(screen, 0).starts_with("ab"));
}

TEST_CASE("NumericInput invalid zero-width draft keeps its cursor",
          "[numeric][zero-cursor][failure]") {
  auto input = prepared();
  const std::string raw = "a\xcc\x81"
                          "b";
  REQUIRE(input.set_draft(raw));
  REQUIRE(input.on_event(key(Key::Home)));
  REQUIRE(input.on_event(key(Key::Right)));
  Screen screen{26, 7};
  input.draw(screen);
  const Rect field = input.editor_rect();
  CHECK(screen.text_at(field.x, field.y) == "a\xcc\x81");
  CHECK(screen.text_at(field.x + 1, field.y) == "b");
  CHECK(any(screen.at(field.x + 1, field.y).attrs & Attr::Reverse));
  CHECK(input.draft() == raw);
  CHECK(input.cursor_pos() == 1);
  CHECK(integer(input) == 5);
  REQUIRE(input.error());
}

namespace {
struct RecordingSink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
// The production App frame/decoder is exercised, but test_run_frames skips
// raw-mode/setup and startup requirements evaluation. No terminal-setup or
// physical-emulator certification is claimed by these observations.
class SettingsHarness final : public examples::NumericSettingsDemo {
 public:
  ~SettingsHarness() override { set_clock(nullptr); }
  RecordingSink output;
  std::vector<std::string> input, screens, band_drafts, brightness_drafts;
  std::vector<examples::PreviewSettings> models;
  std::vector<int> levels, pattern;
  std::vector<bool> band_errors, brightness_errors;
  auto drive(int tier, int frames) -> void {
    set_clock(&clock);
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(frames, 44, 16, nullptr, std::move(selected));
    set_clock(nullptr);
  }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&output);
    examples::NumericSettingsDemo::on_render(screen);
    screens.push_back(screen_text(screen));
    models.push_back(settings());
    band_drafts.push_back(bands_input().draft());
    brightness_drafts.push_back(brightness_input().draft());
    levels.push_back(preview_pixels()[0].r);
    pattern.push_back(preview_pixels()[7].r);
    band_errors.push_back(bands_input().error().has_value());
    brightness_errors.push_back(brightness_input().error().has_value());
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
auto actions(std::string_view wire, std::string_view action) -> int {
  int count = 0;
  for (const auto& record : tfsupport::apcs(wire))
    if (tfsupport::key_value(record, "a") == action &&
        tfsupport::has_key(record, "i"))
      ++count;
  return count;
}
} // namespace

TEST_CASE("Numeric settings example keeps invalid pasted drafts and commits "
          "real pixels on every tier",
          "[numeric][app][drivers]") {
  for (int tier = 0; tier < 3; ++tier) {
    SettingsHarness app;
    app.input = {"",
                 "\033[H\033[3~\033[200~NaN\033[201~",
                 "\r",
                 "\033[27u",
                 "\033[A",
                 "\t\033[H\033[3~\033[3~\033[3~\033[200~0.75\033[201~",
                 "\r",
                 "\033[A\033[A",
                 "\033[27u",
                 "",
                 "\033[200~\033[987J\033[201~"};
    app.drive(tier, 11);
    REQUIRE(app.models.size() == 11);
    CHECK(app.models == std::vector<examples::PreviewSettings>{{4, 0.5},
                                                               {4, 0.5},
                                                               {4, 0.5},
                                                               {4, 0.5},
                                                               {5, 0.5},
                                                               {5, 0.5},
                                                               {5, 0.75},
                                                               {5, 1},
                                                               {5, 1},
                                                               {5, 1},
                                                               {5, 1}});
    CHECK(app.band_drafts[1] == "NaN");
    CHECK(app.band_drafts[2] == "NaN");
    CHECK(app.band_drafts[3] == "4");
    CHECK(app.band_errors == std::vector<bool>{false, true, true, false, false,
                                               false, false, false, false,
                                               false, false});
    CHECK(app.brightness_errors == std::vector<bool>{false, false, false, false,
                                                     false, false, false, true,
                                                     false, false, true});
    CHECK(app.levels == std::vector<int>{128, 128, 128, 128, 128, 128, 191, 255,
                                         255, 255, 255});
    CHECK(app.pattern ==
          std::vector<int>{128, 128, 128, 128, 32, 32, 47, 63, 63, 63, 63});
    CHECK(app.brightness_drafts[10] == "1\033[987J");
    CHECK(app.screens[10].find("[987J") != std::string::npos);
    CHECK(app.output.frames[10].find("\033[987J") == std::string::npos);
    CHECK(app.screens[2].find("Invalid") != std::string::npos);
    CHECK(app.screens[5].find("0.75") != std::string::npos);
    if (tier == 0) {
      CHECK(std::ranges::all_of(app.output.frames[0],
                                [](unsigned char c) { return c < 128; }));
      CHECK(app.output.frames[0].find("\033_G") == std::string::npos);
    }
    if (tier == 2) {
      CHECK(actions(app.output.frames[0], "t") == 1);
      for (std::size_t frame : {1U, 2U, 3U, 5U, 8U, 9U, 10U}) {
        CHECK(actions(app.output.frames[frame], "t") == 0);
        CHECK(actions(app.output.frames[frame], "f") == 0);
      }
      for (std::size_t frame : {4U, 6U, 7U})
        CHECK(actions(app.output.frames[frame], "f") == 1);
    }
  }
}
