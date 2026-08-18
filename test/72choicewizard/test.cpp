// ChoiceWizardDialog is a Layer-3 composition. Exercise its public page/result
// contract and production event order without reaching into owned controls.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/choice_wizard_dialog.hpp"

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

auto single_page(std::string title, std::string choice) -> ChoiceWizardPage {
  ChoiceWizardPage page;
  page.title = std::move(title);
  page.choices = {{std::move(choice), {}}};
  return page;
}

auto empty_multiple(std::string title) -> ChoiceWizardPage {
  ChoiceWizardPage page;
  page.title = std::move(title);
  page.mode = ChoiceMode::Multiple;
  return page;
}

} // namespace

static_assert(!std::is_copy_constructible_v<ChoiceWizardDialog>);
static_assert(!std::is_move_constructible_v<ChoiceWizardDialog>);

TEST_CASE("ChoiceWizardDialog: pages advance and submit ordered results",
          "[choice-wizard]") {
  ChoiceWizardDialog dialog;
  auto second = empty_multiple("Second");
  second.choices = {{"Beta", {}}, {"Gamma", {}}};
  REQUIRE(dialog.set_pages({single_page("First", "Alpha"), second}));
  std::optional<ChoiceWizardResult> result;
  dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
    result = std::move(value);
  });

  REQUIRE(dialog.current_page() == 0);
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Enter)));

  REQUIRE(result.has_value());
  REQUIRE(result->pages.size() == 2);
  REQUIRE(result->pages[0].selected_indices == std::vector<std::size_t>{0});
  REQUIRE(result->pages[1].selected_indices == std::vector<std::size_t>{0});
}

TEST_CASE("ChoiceWizardDialog: invalid replacement is atomic",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  REQUIRE(dialog.set_pages(
      {single_page("First", "a"), single_page("Second", "b")}, 1));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(dialog.page_count() == 2);

  REQUIRE_FALSE(dialog.set_pages({}));
  REQUIRE_FALSE(dialog.set_pages({single_page("Only", "x")}, 1));
  auto invalid = empty_multiple("Invalid");
  invalid.minimum_selected = 2;
  invalid.maximum_selected = 1;
  REQUIRE_FALSE(dialog.set_pages({invalid}));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(dialog.page_count() == 2);
}

TEST_CASE("ChoiceWizardDialog: configured selections are normalized",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto single = single_page("Single", "first");
  single.choices.push_back({"second", {}});
  single.selected_indices = {7, 1, 1, 0};
  auto multiple = empty_multiple("Multiple");
  multiple.choices = {{"a", {}}, {"b", {}}, {"c", {}}};
  multiple.selected_indices = {2, 9, 0, 2};
  REQUIRE(dialog.set_pages({single, multiple}));
  std::optional<ChoiceWizardResult> result;
  dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
    result = std::move(value);
  });

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->pages[0].selected_indices == std::vector<std::size_t>{0});
  REQUIRE(result->pages[1].selected_indices == std::vector<std::size_t>{0, 2});
}

TEST_CASE("ChoiceWizardDialog: empty default reports validation and stays open",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceWizardResult>) { ++calls; });
  Screen screen{40, 10};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Add at least one page.") !=
          std::string::npos);
}

TEST_CASE("ChoiceWizardDialog: Next validates before advancing",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto required = empty_multiple("Required");
  required.choices = {{"a", {}}, {"b", {}}};
  required.minimum_selected = 1;
  REQUIRE(dialog.set_pages({required, empty_multiple("Done")}));
  Screen screen{40, 12};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Select at least 1 option") !=
          std::string::npos);

  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 1);
}

TEST_CASE("ChoiceWizardDialog: maximum selection blocks navigation",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto limited = empty_multiple("Limited");
  limited.choices = {{"a", {}}, {"b", {}}};
  limited.maximum_selected = 1;
  REQUIRE(dialog.set_pages({limited, empty_multiple("Done")}));
  Screen screen{40, 12};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 0);
  dialog.draw(screen);
  REQUIRE(painted_text(screen).find("Select at most 1 option") !=
          std::string::npos);
}

TEST_CASE("ChoiceWizardDialog: Back preserves selections without validation",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto first = empty_multiple("First");
  first.choices = {{"a", {}}, {"b", {}}};
  auto second = empty_multiple("Second");
  second.choices = {{"required", {}}};
  second.minimum_selected = 1;
  REQUIRE(dialog.set_pages({first, second}));
  std::optional<ChoiceWizardResult> result;
  dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
    result = std::move(value);
  });

  REQUIRE(dialog.on_event(ch(U' '))); // retain first-page index 0
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(dialog.on_event(key(Key::Tab)));   // checkbox -> Back
  REQUIRE(dialog.on_event(key(Key::Enter))); // invalid page still goes back
  REQUIRE(dialog.current_page() == 0);
  REQUIRE(dialog.on_event(key(Key::Enter))); // preserved selection validates
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(dialog.on_event(key(Key::Tab)));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 0);

  // Toggle the preserved row off, then on. If navigation lost it, this would
  // instead select it once and the final result would still expose the bug.
  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(ch(U' ')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.on_event(ch(U' '))); // satisfy page 2
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->pages[0].selected_indices == std::vector<std::size_t>{0});
}

TEST_CASE("ChoiceWizardDialog: Other text survives repeated navigation",
          "[choice-wizard]") {
  ChoiceWizardDialog dialog;
  auto first = single_page("First", "Known");
  first.other_enabled = true;
  first.other_placeholder = "explain";
  REQUIRE(dialog.set_pages({first, empty_multiple("Second")}));
  std::optional<ChoiceWizardResult> result;
  dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
    result = std::move(value);
  });

  REQUIRE(dialog.on_event(key(Key::Down)));
  REQUIRE(dialog.on_event(ch(U'n')));
  REQUIRE(dialog.on_event(ch(U'e')));
  REQUIRE(dialog.on_event(ch(U'w')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.on_event(key(Key::Tab, 0, true)));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.current_page() == 0);
  REQUIRE(dialog.on_event(ch(U'!')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(dialog.on_event(key(Key::Enter)));

  REQUIRE(result.has_value());
  REQUIRE(result->pages[0].selected_indices.empty());
  REQUIRE(result->pages[0].other == "new!");
}

TEST_CASE("ChoiceWizardDialog: navigation controls match the page boundary",
          "[choice-wizard]") {
  ChoiceWizardDialog dialog;
  dialog.set_labels("Previous", "Forward", "Finish", "Abort");
  REQUIRE(dialog.set_pages(
      {single_page("First", "a"), single_page("Second", "b")}));
  Screen screen{60, 14};
  dialog.draw(screen);
  std::string text = painted_text(screen);
  REQUIRE(text.find("Previous") == std::string::npos);
  REQUIRE(text.find("Forward") != std::string::npos);
  REQUIRE(text.find("Finish") == std::string::npos);
  REQUIRE(text.find("Abort") != std::string::npos);

  REQUIRE(dialog.on_event(key(Key::Enter)));
  screen.clear();
  dialog.draw(screen);
  text = painted_text(screen);
  REQUIRE(text.find("Previous") != std::string::npos);
  REQUIRE(text.find("Forward") == std::string::npos);
  REQUIRE(text.find("Finish") != std::string::npos);
}

TEST_CASE("ChoiceWizardDialog: cancellation is distinct on every page",
          "[choice-wizard][failure]") {
  for (std::size_t initial = 0; initial < 3; ++initial) {
    ChoiceWizardDialog dialog;
    REQUIRE(dialog.set_pages(
        {empty_multiple("a"), empty_multiple("b"), empty_multiple("c")},
        initial));
    std::optional<ChoiceWizardResult> result{ChoiceWizardResult{}};
    int closes = 0;
    int calls = 0;
    dialog.on_close([&] { ++closes; });
    dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
      result = std::move(value);
      ++calls;
    });

    REQUIRE(dialog.on_event(key(Key::Escape)));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(calls == 1);
    REQUIRE(closes == 1);
    REQUIRE(dialog.on_event(key(Key::Escape)));
    REQUIRE(calls == 1);
    REQUIRE(closes == 1);
  }
}

TEST_CASE("ChoiceWizardDialog: resize while editing Other preserves the draft",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto page = single_page("Resize", "Known");
  page.other_enabled = true;
  REQUIRE(dialog.set_pages({page}));
  std::optional<ChoiceWizardResult> result;
  dialog.on_result([&](std::optional<ChoiceWizardResult> value) {
    result = std::move(value);
  });
  Screen normal{50, 16};
  dialog.draw(normal);
  REQUIRE(dialog.on_event(key(Key::Down)));
  REQUIRE(dialog.on_event(ch(U'x')));

  Screen tiny{7, 3};
  dialog.draw(tiny);
  Screen zero{0, 0};
  dialog.draw(zero);
  REQUIRE(dialog.on_event(ch(U'y')));
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(result.has_value());
  REQUIRE(result->pages[0].other == "xy");
}

TEST_CASE("ChoiceWizardDialog: presentation text is sanitized before drawing",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  auto page = single_page("bad\033[2Jtitle", "choice\033[31m");
  page.text = "body\ttext\nsecond line";
  page.choices[0].description = "desc\ttext";
  page.other_enabled = true;
  page.other_label = "other\033[0m";
  page.other_placeholder = "place\tmark";
  REQUIRE(dialog.set_pages({page}));
  Screen screen{50, 16};
  dialog.draw(screen);
  const std::string text = painted_text(screen);
  REQUIRE(text.find('\033') == std::string::npos);
  REQUIRE(text.find("body text") != std::string::npos);
  REQUIRE(text.find("second line") != std::string::npos);
  REQUIRE(text.find("desc text") != std::string::npos);
}

TEST_CASE("ChoiceWizardDialog: final callback may reconfigure the dialog",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  REQUIRE(dialog.set_pages({single_page("Old", "a")}));
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceWizardResult>) {
    ++calls;
    REQUIRE(dialog.set_pages(
        {single_page("Replacement", "x"), empty_multiple("Second")}));
  });

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(dialog.page_count() == 2);
  REQUIRE(dialog.current_page() == 0);
}

TEST_CASE("ChoiceWizardDialog: on_close may destroy the dialog",
          "[choice-wizard][failure]") {
  auto dialog = std::make_unique<ChoiceWizardDialog>();
  REQUIRE(dialog->set_pages({single_page("Only", "a")}));
  ChoiceWizardDialog* raw = dialog.get();
  int results = 0;
  raw->on_result([&](std::optional<ChoiceWizardResult>) { ++results; });
  raw->on_close([&] { dialog.reset(); });

  REQUIRE(raw->on_event(key(Key::Enter)));
  REQUIRE(dialog == nullptr);
  REQUIRE(results == 1);
}

TEST_CASE("ChoiceWizardDialog: mouse then key reports only once",
          "[choice-wizard][failure][mouse]") {
  ChoiceWizardDialog dialog;
  REQUIRE(dialog.set_pages({single_page("Only", "a")}));
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceWizardResult>) { ++calls; });
  Screen screen{50, 14};
  dialog.draw(screen);

  const int y = dialog.rect().y + dialog.rect().h - 2;
  const std::string row = row_text(screen, y);
  const std::size_t submit = row.find("[ Submit ]");
  REQUIRE(submit != std::string::npos);
  REQUIRE(dialog.on_event(press(static_cast<int>(submit) + 1, y)));
  dialog.on_event(key(Key::Enter));
  REQUIRE(calls == 1);
}

TEST_CASE("ChoiceWizardDialog: a new showing reports once again",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  REQUIRE(dialog.set_pages({single_page("Only", "a")}));
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceWizardResult> result) {
    REQUIRE(result.has_value());
    ++calls;
  });
  Screen screen{40, 12};
  dialog.draw(screen);

  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);

  dialog.draw(screen); // the next draw starts the next showing
  REQUIRE(dialog.on_event(key(Key::Enter)));
  REQUIRE(calls == 2);
}

TEST_CASE("ChoiceWizardDialog: enhanced releases never navigate or finish",
          "[choice-wizard][failure]") {
  ChoiceWizardDialog dialog;
  REQUIRE(dialog.set_pages(
      {single_page("First", "a"), single_page("Second", "b")}));
  int calls = 0;
  dialog.on_result([&](std::optional<ChoiceWizardResult>) { ++calls; });

  REQUIRE_FALSE(dialog.on_event(release(Key::Enter)));
  REQUIRE_FALSE(dialog.on_event(release(Key::Escape)));
  REQUIRE(dialog.current_page() == 0);
  REQUIRE(calls == 0);
}
