// Public dialog contracts and production App frames, not a live font probe.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/choice_dialog.hpp"
#include "termforge/widgets/choice_wizard_dialog.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/file_picker_dialog.hpp"
#include "termforge/widgets/label.hpp"

using namespace termforge;
using namespace tfsupport;
namespace {
auto palette() -> Theme {
  return {.content_fg = {101, 2, 3},
          .content_bg = {4, 5, 6},
          .surface_fg = {7, 8, 9},
          .surface_bg = {10, 11, 12},
          .focus_fg = {13, 14, 15},
          .focus_bg = {16, 17, 18},
          .selection_fg = {19, 20, 21},
          .selection_bg = {22, 23, 24},
          .muted = {25, 26, 27},
          .warning = {28, 29, 30},
          .glyphs = BorderStyle::Ascii};
}
auto locate(const Screen& screen, const std::string& text)
    -> std::pair<int, int> {
  for (int y = 0; y < screen.rows(); ++y)
    for (int x = 0; x < screen.cols(); ++x)
      if (row_text(screen, y, x, screen.cols() - x).starts_with(text))
        return {x, y};
  FAIL("Missing painted text: " << text);
  return {};
}
auto identical(const Screen& a, const Screen& b) -> void {
  for (int y = 0; y < a.rows(); ++y)
    for (int x = 0; x < a.cols(); ++x) {
      CHECK(a.text_at(x, y) == b.text_at(x, y));
      CHECK(a.at(x, y).fg == b.at(x, y).fg);
      CHECK(a.at(x, y).bg == b.at(x, y).bg);
      CHECK(a.at(x, y).attrs == b.at(x, y).attrs);
    }
}
struct Directory {
  std::filesystem::path root;
  Directory() {
    std::array<char, 40> name{};
    const std::string pattern = "/tmp/termforge-theme-dialogs.XXXXXX";
    std::copy(pattern.begin(), pattern.end(), name.begin());
    const auto* made = ::mkdtemp(name.data());
    REQUIRE(made != nullptr);
    root = made;
    std::ofstream{root / "Alpha.txt"} << "fixture";
    std::ofstream{root / "Beta.txt"} << "fixture";
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  Directory(const Directory&) = delete;
  auto operator=(const Directory&) -> Directory& = delete;
};
auto page(std::string title) -> ChoiceWizardPage {
  ChoiceWizardPage result;
  result.title = std::move(title);
  result.mode = ChoiceMode::Multiple;
  result.choices = {{"Alpha", "Description"}, {"Beta", "Details"}};
  return result;
}
} // namespace

TEST_CASE("Every built-in dialog exactly restores its opt-out presentation",
          "[theme][dialogs][compatibility]") {
  Directory directory;
  Dialog base{"Base"};
  base.set_text("Body");
  MessageDialog message{"Message", "Body"};
  ConfirmDialog confirm{"Confirm", "Body"};
  PromptDialog prompt{"Prompt", "Body"};
  prompt.set_value("Draft");
  ChoiceDialog choice{"Choice", "Body", ChoiceMode::Multiple};
  choice.set_choices({{"Alpha", "Description"}, {"Beta", "Details"}});
  choice.set_other_enabled(true);
  ChoiceWizardDialog wizard;
  REQUIRE(wizard.set_pages({page("First"), page("Second")}));
  FilePickerDialog picker{"Picker"};
  picker.set_start_dir(directory.root);
  int callbacks = 0;
  for (Dialog* dialog : std::array<Dialog*, 7>{
           &base, &message, &confirm, &prompt, &choice, &wizard, &picker}) {
    dialog->on_close([&] { ++callbacks; });
    for (bool explicit_style : {false, true}) {
      if (explicit_style) dialog->set_border_style(BorderStyle::Single);
      Screen before{40, 18}, after{40, 18};
      dialog->draw(before);
      const auto rect = dialog->rect();
      dialog->set_theme(palette());
      dialog->draw(after); // exercise layout before clearing, not just setters
      CHECK(dialog->rect() == rect);
      dialog->clear_theme();
      after.clear();
      dialog->draw(after);
      identical(before, after);
    }
  }
  CHECK(prompt.value() == "Draft");
  CHECK(picker.current_dir() == directory.root);
  CHECK(callbacks == 0);
}

TEST_CASE("Modal surface and owned focus roles preserve prompt edit position",
          "[theme][dialogs][roles]") {
  PromptDialog prompt{"Prompt", "Body"};
  prompt.set_value("Draft");
  Screen screen{40, 12};
  prompt.draw(screen);
  REQUIRE(prompt.on_event(key(Key::Left)));
  prompt.set_theme(palette());
  prompt.draw(screen);
  const auto r = prompt.rect();
  CHECK(screen.text_at(r.x, r.y) == "+");
  CHECK(screen.at(r.x, r.y).fg == palette().muted);
  CHECK(screen.at(r.x, r.y).bg == palette().surface_bg);
  const auto [body_x, body_y] = locate(screen, "Body");
  CHECK(screen.at(body_x, body_y).fg == palette().surface_fg);
  CHECK(screen.at(body_x, body_y).bg == palette().surface_bg);
  const auto [input_x, input_y] = locate(screen, "Draft");
  CHECK(screen.at(input_x, input_y).fg == palette().content_fg);
  CHECK(screen.at(input_x + 4, input_y).fg == palette().focus_fg);
  CHECK(screen.at(input_x + 4, input_y).bg == palette().focus_bg);
  CHECK(any(screen.at(input_x + 4, input_y).attrs & Attr::Reverse));
  REQUIRE(prompt.on_event(ch(U'X')));
  CHECK(prompt.value() == "DrafXt");
  REQUIRE(prompt.on_event(key(Key::Tab)));
  auto next = palette();
  next.focus_bg = {80, 81, 82};
  prompt.set_theme(next);
  prompt.draw(screen);
  const auto [button_x, button_y] = locate(screen, "[ OK ]");
  CHECK(screen.at(button_x, button_y).bg == next.focus_bg);
  CHECK(any(screen.at(button_x, button_y).attrs & Attr::Bold));
  int submitted = 0;
  prompt.on_submit([&](const std::string& value) {
    CHECK(value == "DrafXt");
    ++submitted;
  });
  REQUIRE(prompt.on_event(key(Key::Enter)));
  prompt.clear_theme();
  REQUIRE(prompt.on_event(key(Key::Enter)));
  CHECK(submitted == 1); // snapshot clearing did not re-arm the result latch
}

TEST_CASE(
    "Choice controls inherit new snapshots without losing authored intent",
    "[theme][dialogs][glyphs][dynamic]") {
  for (ChoiceMode mode : {ChoiceMode::Single, ChoiceMode::Multiple}) {
    for (bool override_first : {false, true}) {
      ChoiceDialog dialog{"Choice", "", mode};
      Dialog& base = dialog;
      if (override_first) base.set_border_style(BorderStyle::Single);
      dialog.set_theme(palette());
      // Controls allocated after the snapshot inherit it immediately.
      dialog.set_choices({{"Alpha", "Description"}, {"Beta", "Details"}});
      dialog.set_selected_indices({1});
      dialog.set_other_enabled(true);
      dialog.set_other_text("Draft");
      Screen screen{40, 15};
      dialog.draw(screen);
      const auto [label_x, label_y] = locate(screen, "Alpha");
      CHECK(screen.at(label_x, label_y).fg == (mode == ChoiceMode::Single
                                                   ? palette().content_fg
                                                   : palette().focus_fg));
      CHECK(screen.text_at(dialog.rect().x, dialog.rect().y) ==
            (override_first ? "┌" : "+"));
      if (mode == ChoiceMode::Single) {
        const auto [selected_x, selected_y] = locate(screen, "Beta");
        CHECK(screen.text_at(selected_x - 3, selected_y) ==
              (override_first ? "•" : "*"));
      }
      if (!override_first) base.set_border_style(BorderStyle::Single);
      dialog.draw(screen);
      CHECK(screen.text_at(dialog.rect().x, dialog.rect().y) == "┌");
      auto next = palette();
      next.glyphs = BorderStyle::Double;
      dialog.set_theme(next);
      CHECK(dialog.border_style() == BorderStyle::Single);
      dialog.clear_theme();
      dialog.draw(screen);
      CHECK(dialog.selected_indices() == std::vector<std::size_t>{1});
      CHECK(dialog.other_text() == "Draft");
      CHECK(screen.text_at(dialog.rect().x, dialog.rect().y) == "┌");
    }
    ChoiceDialog reversible{"Choice", "", mode};
    reversible.set_theme(palette());
    reversible.set_choices({{"Alpha", ""}});
    Screen screen{40, 12};
    reversible.draw(screen);
    const auto [x, y] = locate(screen, "Alpha");
    CHECK(screen.text_at(reversible.rect().x, reversible.rect().y) == "+");
    if (mode == ChoiceMode::Single) CHECK(screen.text_at(x - 3, y) == "*");
    auto next = palette();
    next.glyphs = BorderStyle::Double;
    reversible.set_theme(next);
    reversible.draw(screen);
    CHECK(screen.text_at(reversible.rect().x, reversible.rect().y) == "╔");
    if (mode == ChoiceMode::Single) CHECK(screen.text_at(x - 3, y) == "•");
    reversible.clear_theme();
    reversible.draw(screen);
    CHECK(screen.at(x, y).bg == theme::kFocusBg);
    CHECK(reversible.border_style() == BorderStyle::Single);
  }
}

TEST_CASE("Choice diagnostic roles preserve validation and Other drafts",
          "[theme][dialogs][diagnostics]") {
  ChoiceDialog dialog{"Choice", "Room for the validation message.",
                      ChoiceMode::Multiple};
  dialog.set_choices({{"Alpha", "Description"}, {"Beta", "Details"}});
  REQUIRE(dialog.set_selection_limits(2));
  dialog.set_other_enabled(true);
  dialog.set_other_text("Draft");
  int results = 0;
  dialog.on_result([&](const auto&) { ++results; });
  Screen screen{40, 16};
  dialog.draw(screen);
  REQUIRE(dialog.on_event(key(Key::Enter)));
  dialog.set_theme(palette());
  dialog.draw(screen);
  const auto [x, y] = locate(screen, "Select at least 2 options.");
  CHECK(screen.at(x, y).fg == palette().warning);
  CHECK(screen.at(x, y).bg == palette().surface_bg);
  CHECK(any(screen.at(x, y).attrs & Attr::Bold));
  const auto [dx, dy] = locate(screen, "Description");
  CHECK(screen.at(dx, dy).fg == palette().muted);
  CHECK(dialog.other_text() == "Draft");
  CHECK(dialog.selected_indices().empty());
  dialog.clear_theme();
  dialog.draw(screen);
  CHECK(screen.at(x, y).fg == Rgb{0xFF, 0xA0, 0x60});
  CHECK_FALSE(any(screen.at(x, y).attrs & Attr::Bold));
  CHECK(results == 0);
}

TEST_CASE("Wizard page allocation retains Theme and page-local results",
          "[theme][dialogs][wizard]") {
  ChoiceWizardDialog wizard;
  wizard.set_theme(palette());
  auto first = page("First");
  first.selected_indices = {1};
  auto second = page("Second");
  second.other_enabled = true;
  second.other_selected = true;
  second.other_text = "Draft";
  REQUIRE(wizard.set_pages({first, second}));
  Screen screen{40, 16};
  wizard.draw(screen);
  REQUIRE(wizard.on_event(key(Key::Enter)));
  REQUIRE(wizard.current_page() == 1);
  wizard.draw(screen);
  auto next = palette();
  next.content_bg = {70, 71, 72};
  next.glyphs = BorderStyle::Double;
  wizard.set_theme(next);
  wizard.draw(screen);
  CHECK(wizard.current_page() == 1);
  const auto [x, y] = locate(screen, "Beta");
  CHECK(screen.at(x, y).bg == next.content_bg);
  CHECK(screen.text_at(wizard.rect().x, wizard.rect().y) == "╔");
  CHECK(locate(screen, "Draft").first > 0);
  Dialog& base = wizard;
  base.set_border_style(BorderStyle::Ascii);
  wizard.clear_theme();
  wizard.draw(screen);
  CHECK(row_text(screen, y).find("[ ]") != std::string::npos);
  std::optional<ChoiceWizardResult> result;
  wizard.on_result([&](auto value) { result = std::move(value); });
  REQUIRE(wizard.on_event(key(Key::Enter)));
  REQUIRE(result);
  CHECK(result->pages[0].selected_indices == std::vector<std::size_t>{1});
  CHECK(result->pages[1].other == "Draft");
}

TEST_CASE("Wizard diagnostic snapshots preserve blocked page transitions",
          "[theme][dialogs][wizard][diagnostics]") {
  ChoiceWizardDialog wizard;
  auto first = page("First");
  first.text = "Room for the validation message.";
  first.minimum_selected = 2;
  REQUIRE(wizard.set_pages({first, page("Second")}));
  int results = 0;
  wizard.on_result([&](const auto&) { ++results; });
  Screen screen{40, 16};
  wizard.draw(screen);
  REQUIRE(wizard.on_event(key(Key::Enter)));
  CHECK(wizard.current_page() == 0);
  wizard.set_theme(palette());
  wizard.draw(screen);
  const auto [x, y] = locate(screen, "Select at least 2 options.");
  CHECK(screen.at(x, y).fg == palette().warning);
  CHECK(screen.at(x, y).bg == palette().surface_bg);
  CHECK(any(screen.at(x, y).attrs & Attr::Bold));
  const auto [dx, dy] = locate(screen, "Description");
  CHECK(screen.at(dx, dy).fg == palette().muted);
  auto next = palette();
  next.warning = {60, 61, 62};
  wizard.set_theme(next);
  wizard.draw(screen);
  CHECK(screen.at(x, y).fg == next.warning);
  CHECK(wizard.current_page() == 0);
  wizard.clear_theme();
  wizard.draw(screen);
  CHECK(screen.at(x, y).fg == Rgb{0xFF, 0xA0, 0x60});
  CHECK_FALSE(any(screen.at(x, y).attrs & Attr::Bold));
  CHECK(wizard.current_page() == 0);
  CHECK(results == 0);
}

TEST_CASE("Picker Themes preserve selection and its owned error overlay",
          "[theme][dialogs][picker]") {
  Directory directory;
  FilePickerDialog picker{"Picker"};
  picker.set_start_dir(directory.root);
  Screen screen{60, 20};
  picker.draw(screen);
  REQUIRE(picker.on_event(key(Key::Down)));
  picker.set_theme(palette());
  picker.draw(screen);
  const auto [x, y] = locate(screen, "Alpha.txt");
  CHECK(screen.at(x, y).bg == palette().selection_bg);
  CHECK(any(screen.at(x, y).attrs & Attr::Bold));
  CHECK(picker.current_dir() == directory.root);
  picker.clear_theme();
  picker.draw(screen);
  CHECK(screen.at(x, y).bg == theme::kFocusBg);
  std::optional<std::filesystem::path> picked;
  picker.on_result([&](auto value) { picked = std::move(value); });
  REQUIRE(picker.on_event(key(Key::Enter)));
  CHECK(picked == directory.root / "Alpha.txt");

  FilePickerDialog failing{"Picker"};
  failing.set_start_dir(directory.root / "absent");
  failing.set_theme(palette());
  Dialog* error = nullptr;
  int raises = 0;
  failing.on_error_overlay([&](Dialog& owned) {
    error = &owned;
    ++raises;
  });
  failing.draw(screen);
  REQUIRE(error != nullptr);
  CHECK(error->theme_snapshot() == palette());
  CHECK(error->border_style() == BorderStyle::Ascii);
  failing.clear_theme();
  CHECK_FALSE(error->theme_snapshot());
  CHECK(error->border_style() == BorderStyle::Single);
  failing.set_border_style(BorderStyle::Single);
  failing.set_theme(palette());
  CHECK(error->border_style() == BorderStyle::Single);
  failing.draw(screen);
  CHECK(raises == 1); // Theme did not refresh or start another showing
}

namespace {
// A legacy-style custom dialog with a BORROWED child and no new hook override.
class BorrowingDialog final : public Dialog {
 public:
  explicit BorrowingDialog(Label& label) : m_label(label) { add_child(&label); }

 protected:
  auto content_rows() const -> int override { return 1; }
  auto content_cols() const -> int override { return 8; }
  auto layout_content(Rect area) -> void override {
    m_label.set_geometry(area);
  }
  auto draw_content(Screen& screen) -> void override { m_label.draw(screen); }

 private:
  Label& m_label;
};
} // namespace

TEST_CASE("Dialog child registration is not an ownership or Theme traversal",
          "[theme][dialogs][borrowed]") {
  Label independent{"Borrowed"};
  auto authored = palette();
  authored.content_fg = {50, 51, 52};
  independent.set_theme(authored);
  BorrowingDialog dialog{independent};
  dialog.set_theme(palette());
  dialog.set_border_style(BorderStyle::Ascii);
  Screen screen{30, 10};
  dialog.draw(screen);
  CHECK(independent.theme_snapshot() == authored);
  dialog.clear_theme();
  CHECK(independent.theme_snapshot() == authored);
  const auto [x, y] = locate(screen, "Borrowed");
  CHECK(screen.at(x, y).fg == authored.content_fg);
}

TEST_CASE("Dialog Theme updates retain focus and self-deleting result handlers",
          "[theme][dialogs][callbacks]") {
  ConfirmDialog confirm{"Confirm", "Body"};
  confirm.set_default(false);
  int closes = 0, results = 0;
  bool accepted = true;
  confirm.on_close([&] { ++closes; });
  confirm.on_result([&](bool value) {
    accepted = value;
    ++results;
  });
  Screen screen{40, 12};
  confirm.draw(screen);
  confirm.set_theme(palette());
  confirm.draw(screen);
  const auto [x, y] = locate(screen, "[ No ]");
  CHECK(screen.at(x, y).bg == palette().focus_bg);
  CHECK(any(screen.at(x, y).attrs & Attr::Bold));
  confirm.clear_theme();
  REQUIRE(confirm.on_event(key(Key::Enter)));
  CHECK_FALSE(accepted);
  CHECK(closes == 1);
  CHECK(results == 1);

  auto message = std::make_unique<MessageDialog>("Message", "Body");
  message->set_theme(palette());
  message->on_close([&] { message.reset(); });
  message->on_ok([&] { ++results; });
  message->draw(screen);
  REQUIRE(message->on_event(key(Key::Enter)));
  CHECK_FALSE(message);
  CHECK(results == 2);
}

TEST_CASE("Themed dialogs clip wide content on tiny screens",
          "[theme][dialogs][clipping]") {
  MessageDialog message{"界 Title", "Body 界"};
  PromptDialog prompt{"界 Title", "Body 界"};
  prompt.set_value("Draft 界");
  ChoiceDialog choice{"界 Title", "Body 界", ChoiceMode::Multiple};
  choice.set_choices({{"Wide 界", "Description 界"}});
  ChoiceWizardDialog wizard;
  REQUIRE(wizard.set_pages({page("界 Title")}));
  for (Dialog* dialog :
       std::array<Dialog*, 4>{&message, &prompt, &choice, &wizard}) {
    dialog->set_theme(palette());
    for (int width : {0, 1, 2, 3, 8})
      for (int height : {0, 1, 2, 3, 6}) {
        Screen screen{width, height};
        dialog->draw(screen);
        CHECK(dialog->rect().w <= width);
        CHECK(dialog->rect().h <= height);
      }
  }
}

namespace {
struct DialogSink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
class DialogApp final : public App {
 public:
  PromptDialog prompt{"Prompt", "Body"};
  DialogSink sink;
  int frame{0}, escaped{0};
  std::string submitted;
  auto run(int tier) -> void {
    prompt.set_value("Draft");
    prompt.on_close([this] { pop_overlay(); });
    prompt.on_submit(
        [this](std::string value) { submitted = std::move(value); });
    push_overlay(prompt);
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(5, 40, 12, nullptr, std::move(selected));
  }

 protected:
  auto read_available(char*, int) -> int override { return 0; }
  auto wait_readable(int) -> bool override { return false; }
  auto on_event(const Event&) -> void override { ++escaped; }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    screen.clear();
    if (frame == 0) prompt.set_theme(palette());
    if (frame == 2) {
      auto next = palette();
      next.focus_bg = {80, 81, 82};
      prompt.set_theme(next);
      test_pump({"\033[D", "X", "\t"});
    }
    if (frame == 4) test_pump({"\r"});
    ++frame;
  }
};
} // namespace

TEST_CASE(
    "Dialog snapshots compose with real modal input and three-tier writes",
    "[theme][dialogs][app][wire]") {
  for (int tier : {0, 1, 2}) {
    DialogApp app;
    app.run(tier);
    REQUIRE(app.sink.frames.size() == 5);
    CHECK(app.sink.frames[1].empty());
    CHECK(app.sink.frames[3].empty());
    CHECK(app.escaped == 0);
    CHECK(app.submitted == "DrafXt");
    CHECK(app.top_overlay() == nullptr);
    TerminalGrid terminal{40, 12};
    terminal.feed(app.sink.frames[0]);
    int input_y = -1, input_x = -1;
    for (int y = 0; y < 12; ++y) {
      const auto x = terminal.row_text(y).find("Draft");
      if (x != std::string::npos) {
        input_y = y;
        input_x = static_cast<int>(x);
      }
    }
    REQUIRE(input_y >= 0);
    REQUIRE(input_x >= 0);
    CHECK_FALSE(terminal.at(input_x, input_y).bold);
    terminal.feed(app.sink.frames[2]);
    CHECK(terminal.row_text(input_y).find("DrafXt") != std::string::npos);
    int button_x = -1, button_y = -1;
    for (int y = 0; y < 12; ++y) {
      const auto x = terminal.row_text(y).find("[ OK ]");
      if (x != std::string::npos) {
        button_x = static_cast<int>(x);
        button_y = y;
      }
    }
    REQUIRE(button_x >= 0);
    REQUIRE(button_y >= 0);
    CHECK(terminal.at(button_x, button_y).bold);
    if (tier == 0)
      CHECK(terminal.at(button_x, button_y).bg == -1);
    else
      CHECK(terminal.at(button_x, button_y).bg ==
            ((80 << 16) | (81 << 8) | 82));
  }
}
