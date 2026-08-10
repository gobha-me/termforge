// ChoiceDialog is a Layer-3 composition, so this suite tests the caller-visible
// form contract: result identity, cancellation, validation, dynamic rebuilding,
// small-screen clipping and the exact production input order. It does not
// reach into the owned RadioGroup/Checkboxes; observations come through the
// public result and painted Screen.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/choice_dialog.hpp"

using namespace termforge;
using namespace tfsupport;

namespace {

auto painted_text(const Screen& screen) -> std::string {
  std::string out;
  for (int y = 0; y < screen.rows(); ++y) {
    out += row_text(screen, y);
    out += '\n';
  }
  return out;
}

}  // namespace

static_assert(!std::is_copy_constructible_v<ChoiceDialog>);
static_assert(!std::is_move_constructible_v<ChoiceDialog>);

TEST_CASE("ChoiceDialog: single mode reports the selected presentation index",
          "[choice-dialog]") {
  ChoiceDialog dialog{"Choose", "Pick one"};
  dialog.set_choices({{"Alpha", "first"}, {"Beta", "second"}});
  std::optional<ChoiceResult> result;
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult> value) {
    result = std::move(value);
    ++calls;
  });

  REQUIRE(dialog.selected_indices() == std::vector<std::size_t>{0});
  REQUIRE(dialog.on_event(key(Key::Down)));
  REQUIRE(dialog.selected_indices() == std::vector<std::size_t>{1});
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(result.has_value());
  REQUIRE(result->selected_indices == std::vector<std::size_t>{1});
  REQUIRE_FALSE(result->other.has_value());
}

TEST_CASE("ChoiceDialog: cancel is distinct from a valid empty multiple result",
          "[choice-dialog][failure]") {
  std::optional<ChoiceResult> cancelled{ChoiceResult{}};
  ChoiceDialog cancel{"Choose", "", ChoiceMode::Multiple};
  cancel.on_result(
      [&](std::optional<ChoiceResult> value) { cancelled = std::move(value); });
  REQUIRE(cancel.on_event(key(Key::Escape)));
  REQUIRE_FALSE(cancelled.has_value());

  std::optional<ChoiceResult> submitted;
  ChoiceDialog empty{"Choose", "", ChoiceMode::Multiple};
  empty.on_result(
      [&](std::optional<ChoiceResult> value) { submitted = std::move(value); });
  REQUIRE(empty.on_event(key(Key::Enter)));
  REQUIRE(submitted.has_value());
  REQUIRE(submitted->selected_indices.empty());
  REQUIRE_FALSE(submitted->other.has_value());
}

TEST_CASE("ChoiceDialog: an empty single question rejects submit visibly",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", ""};
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) { ++calls; });
  Screen screen{40, 10};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Select one option.") != std::string::npos);
}

TEST_CASE("ChoiceDialog: multiple mode tabs between independent duplicate rows",
          "[choice-dialog]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"same", "first"}, {"same", "second"}, {"last", ""}});
  std::optional<ChoiceResult> result;
  dialog.on_result(
      [&](std::optional<ChoiceResult> value) { result = std::move(value); });

  REQUIRE(dialog.on_event(ch(U' ')));      // index 0
  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(ch(U' ')));      // index 1, despite duplicate label
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->selected_indices == std::vector<std::size_t>{0, 1});
}

TEST_CASE("ChoiceDialog: minimum and maximum reject visibly without closing",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"a", ""}, {"b", ""}, {"c", ""}});
  REQUIRE(dialog.set_selection_limits(2, 2));
  REQUIRE_FALSE(dialog.set_selection_limits(3, 2));
  REQUIRE(dialog.minimum_selected() == 2);
  REQUIRE(dialog.maximum_selected() == 2);

  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) { ++calls; });
  Screen screen{50, 16};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(ch(U' ')));       // one: below minimum
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Select at least 2") !=
          std::string::npos);

  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(ch(U' ')));       // two: valid
  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(ch(U' ')));       // three: above maximum
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Select at most 2") !=
          std::string::npos);
}

TEST_CASE("ChoiceDialog: selecting Other reveals focus and requires text",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", ""};
  dialog.set_choices({{"Known", ""}});
  dialog.set_other_enabled(true);
  dialog.set_other_placeholder("explain");
  std::optional<ChoiceResult> result;
  dialog.on_result(
      [&](std::optional<ChoiceResult> value) { result = std::move(value); });

  Screen screen{50, 16};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(Key::Down)));  // select Other, focus its input
  REQUIRE(dialog.other_selected());
  REQUIRE(dialog.on_event(key(Key::Enter)));  // empty Other is invalid
  REQUIRE_FALSE(result.has_value());

  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Enter an Other response.") !=
          std::string::npos);
  REQUIRE(dialog.on_event(ch(U'n')));
  REQUIRE(dialog.on_event(ch(U'e')));
  REQUIRE(dialog.on_event(ch(U'w')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->selected_indices.empty());
  REQUIRE(result->other == "new");
}

TEST_CASE("ChoiceDialog: multiple Other coexists with ordinary selections",
          "[choice-dialog]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"A", ""}, {"B", ""}});
  dialog.set_other_enabled(true);
  dialog.set_other_text("custom");
  dialog.set_other_selected(true);
  dialog.set_selected_indices({1});
  // Standard selection setters do not silently discard the separate Other
  // state, which is the multiple-mode composition requirement.
  REQUIRE(dialog.other_selected());

  std::optional<ChoiceResult> result;
  dialog.on_result(
      [&](std::optional<ChoiceResult> value) { result = std::move(value); });
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->selected_indices == std::vector<std::size_t>{1});
  REQUIRE(result->other == "custom");
}

TEST_CASE("ChoiceDialog: reconfiguration drops stale indices and keeps valid ones",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"a", ""}, {"b", ""}, {"c", ""}});
  dialog.set_selected_indices({0, 2});
  dialog.set_choices({{"new-a", ""}, {"new-b", ""}});
  REQUIRE(dialog.selected_indices() == std::vector<std::size_t>{0});
}

TEST_CASE("ChoiceDialog: callback may reconfigure the dialog it just closed",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", ""};
  dialog.set_choices({{"old", ""}});
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) {
    ++calls;
    dialog.set_mode(ChoiceMode::Multiple);
    dialog.set_choices({{"replacement", ""}, {"second", ""}});
  });

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(dialog.mode() == ChoiceMode::Multiple);
  REQUIRE(dialog.choices().size() == 2);
}

TEST_CASE("ChoiceDialog: on_close may destroy the dialog",
          "[choice-dialog][failure]") {
  auto dialog = std::make_unique<ChoiceDialog>("Choose", "");
  dialog->set_choices({{"a", ""}});
  ChoiceDialog* raw = dialog.get();
  int results = 0;
  raw->on_result([&](std::optional<ChoiceResult>) { ++results; });
  raw->on_close([&] { dialog.reset(); });

  REQUIRE(raw->on_event(key(Key::Enter)));
  REQUIRE(dialog == nullptr);
  REQUIRE(results == 1);  // callback was snapshotted before destruction
}

TEST_CASE("ChoiceDialog: reopen preserves values but reports once per showing",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"a", ""}, {"b", ""}});
  dialog.set_other_enabled(true);
  dialog.set_selected_indices({1});
  dialog.set_other_selected(true);
  dialog.set_other_text("kept");
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) { ++calls; });

  Screen screen{50, 16};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(dialog.on_event(key(Key::Escape)));
  REQUIRE(calls == 1);  // same showing remains latched

  dialog.draw(screen);  // new showing
  REQUIRE(dialog.selected_indices() == std::vector<std::size_t>{1});
  REQUIRE(dialog.other_selected());
  REQUIRE(dialog.other_text() == "kept");
  REQUIRE(dialog.on_event(key(Key::Escape)));
  REQUIRE(calls == 2);
}

TEST_CASE("ChoiceDialog: labels and descriptions sanitize before measurement",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", ""};
  dialog.set_choices({{"evil\033[2Jlabel\xC0\x9B", "desc\twith\033[31mcontrols"},
                      {"日本語", "wide"}});
  REQUIRE(dialog.choices()[0].label == "evillabel");
  REQUIRE(dialog.choices()[0].description == "desc withcontrols");

  Screen screen{24, 10};
  dialog.draw(screen);
  const std::string text = painted_text(screen);
  REQUIRE(text.find('\033') == std::string::npos);
  REQUIRE(text.find("desc withcontrols") != std::string::npos);
  REQUIRE(text.find("日本語") != std::string::npos);
}

TEST_CASE("ChoiceDialog: a visible multiple row toggles by mouse",
          "[choice-dialog][mouse]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  dialog.set_choices({{"first", ""}, {"second", ""}});
  Screen screen{40, 12};
  dialog.draw(screen);
  const Rect rect = dialog.rect();

  // No body text: the second checkbox is the second interior row.
  REQUIRE(dialog.on_event(press(rect.x + 2, rect.y + 2)));
  REQUIRE(dialog.selected_indices() == std::vector<std::size_t>{1});
}

TEST_CASE("ChoiceDialog: overflow follows Tab focus and never paints outside",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", "", ChoiceMode::Multiple};
  std::vector<ChoiceOption> choices;
  for (int i = 0; i < 14; ++i)
    choices.push_back({"choice-" + std::to_string(i), "description"});
  dialog.set_choices(std::move(choices));

  Screen screen{32, 10};
  dialog.draw(screen);
  for (int i = 0; i < 12; ++i) REQUIRE(dialog.on_event(key(Key::Tab)));
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("choice-12") != std::string::npos);

  // The degenerate geometry is still a keyboard-operable modal and never
  // writes out of bounds or divides by a negative content height.
  Screen zero{0, 0};
  dialog.draw(zero);
  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Escape)));
}

TEST_CASE("ChoiceDialog: mouse activation and a following key report only once",
          "[choice-dialog][failure][mouse]") {
  ChoiceDialog dialog{"Choose", ""};
  dialog.set_choices({{"a", ""}});
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) { ++calls; });
  Screen screen{40, 12};
  dialog.draw(screen);
  const Rect rect = dialog.rect();

  // Submit is the left button on the last interior row.
  REQUIRE(dialog.on_event(press(rect.x + rect.w - 19, rect.y + rect.h - 2)));
  dialog.on_event(key(Key::Enter));
  REQUIRE(calls == 1);
}

TEST_CASE("ChoiceDialog: enhanced key releases never activate a result",
          "[choice-dialog][failure]") {
  ChoiceDialog dialog{"Choose", ""};
  dialog.set_choices({{"a", ""}});
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceResult>) { ++calls; });

  REQUIRE_FALSE(dialog.on_event(release(Key::Enter)));
  REQUIRE_FALSE(dialog.on_event(release(Key::Escape)));
  REQUIRE(calls == 0);
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
}
