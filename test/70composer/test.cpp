// Composer tests (#26).
//
// These cases exercise the caller's real event order: a focused widget first
// receives vertical movement, and only a movement that reaches the document
// boundary turns into history navigation. The split matters — testing history
// only on one-line drafts would miss an Up key stealing multiline navigation.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "detail/wrap.hpp"
#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/widgets/composer.hpp"
#include "termforge/widgets/focus_ring.hpp"

using namespace tfsupport;
using termforge::Composer;
using termforge::ComposerEnterMode;
using termforge::Event;
using termforge::FocusRing;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;
using termforge::PasteEvent;
using termforge::Screen;

namespace {

auto modified_enter(bool shift, bool alt, bool ctrl = false) -> Event {
  KeyEvent event;
  event.key = Key::Enter;
  event.shift = shift;
  event.alt = alt;
  event.ctrl = ctrl;
  return event;
}

auto modified_char(char32_t value, bool ctrl, bool alt) -> Event {
  KeyEvent event;
  event.key = Key::Char;
  event.ch = value;
  event.ctrl = ctrl;
  event.alt = alt;
  return event;
}

auto focused_composer(int width = 20, int height = 4) -> Composer {
  Composer composer;
  composer.set_geometry({0, 0, width, height});
  composer.set_focused(true);
  return composer;
}

} // namespace

TEST_CASE("Composer: shared wrapping reports source byte ranges",
          "[composer][wrap]") {
  const std::string source = "one two\n界x";
  const auto ranges = termforge::detail::wrap_byte_ranges(source, 4);
  const auto rows = termforge::detail::wrap_to_width(source, 4);

  REQUIRE(ranges.size() == rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i)
    CHECK(source.substr(ranges[i].begin, ranges[i].end - ranges[i].begin) ==
          rows[i]);
  CHECK(ranges[1].end + 1 == ranges[2].begin); // hard newline byte
}

TEST_CASE("Composer: height grows with wrapped content and clamps",
          "[composer][layout]") {
  Composer composer;
  composer.set_max_height(3);
  composer.set_text("abcd");

  // A cursor after an exactly full row needs a real insertion row.
  CHECK(composer.preferred_height(4) == 2);
  composer.set_text("a\nb\nc\nd");
  CHECK(composer.preferred_height(20) == 3);
  composer.set_max_height(0);
  CHECK(composer.max_height() == 1);
  CHECK(composer.preferred_height(20) == 1);
}

TEST_CASE("Composer: exact-width cursor paints on the following row",
          "[composer][cursor][failure]") {
  Screen screen{4, 2};
  Composer composer = focused_composer(4, 2);
  composer.set_text("abcd");
  composer.draw(screen);

  CHECK(row_text(screen, 0) == "abcd");
  CHECK(screen.at(0, 1).fg == termforge::theme::kFocusFg);
  CHECK(screen.at(0, 1).bg == termforge::theme::kFg);
}

TEST_CASE("Composer: UTF-8 movement and deletion keep byte boundaries",
          "[composer][utf8][failure]") {
  Composer composer = focused_composer();
  composer.set_text("a界b");

  REQUIRE(composer.on_event(key(Key::Left)));
  CHECK(composer.cursor_pos() == 4);
  REQUIRE(composer.on_event(key(Key::Left)));
  CHECK(composer.cursor_pos() == 1);
  REQUIRE(composer.on_event(key(Key::Delete)));
  CHECK(composer.text() == "ab");
  CHECK(composer.cursor_pos() == 1);
  REQUIRE(composer.on_event(key(Key::Backspace)));
  CHECK(composer.text() == "b");
  CHECK(composer.cursor_pos() == 0);
}

TEST_CASE("Composer: Home End and arrows navigate visual rows",
          "[composer][cursor][wrap]") {
  Composer composer = focused_composer(3, 4);
  composer.set_text("abcdef");

  REQUIRE(composer.on_event(key(Key::Home)));
  CHECK(composer.cursor_pos() == 6); // insertion row after exact-width "def"
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.cursor_pos() == 3);
  REQUIRE(composer.on_event(key(Key::End)));
  CHECK(composer.cursor_pos() == 6);
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.cursor_pos() == 3);
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.cursor_pos() == 0);
  REQUIRE(composer.on_event(key(Key::Right)));
  CHECK(composer.cursor_pos() == 1);
}

TEST_CASE("Composer: vertical movement preserves a display-column goal",
          "[composer][cursor][utf8]") {
  Composer composer = focused_composer(8, 3);
  composer.set_text("界a\n12345\nx");

  REQUIRE(composer.on_event(key(Key::Home)));
  REQUIRE(composer.on_event(key(Key::Up)));
  REQUIRE(composer.on_event(key(Key::Right)));
  REQUIRE(composer.on_event(key(Key::Right)));
  REQUIRE(composer.on_event(key(Key::Right)));
  CHECK(composer.cursor_pos() == 8); // column 3 in 12345
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.cursor_pos() == 4); // clamped after 界a (3 columns)
  REQUIRE(composer.on_event(key(Key::Down)));
  CHECK(composer.cursor_pos() == 8);
  REQUIRE(composer.on_event(key(Key::Down)));
  CHECK(composer.cursor_pos() == 12); // short final row clamps to its end
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.cursor_pos() == 8); // original column 3 survives the clamp
}

TEST_CASE("Composer: multiline Up moves before it recalls history",
          "[composer][history][caller-order]") {
  Composer composer = focused_composer(10, 3);
  composer.push_history("old");
  composer.set_text("top\nbottom");

  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.text() == "top\nbottom");
  CHECK(composer.cursor_pos() == 3);
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.text() == "old");
}

TEST_CASE("Composer: history preserves edited recalls and the bottom draft",
          "[composer][history][failure]") {
  Composer composer = focused_composer();
  composer.push_history("one");
  composer.push_history("two");
  composer.set_text("draft");

  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.text() == "two");
  REQUIRE(composer.on_event(ch(U'!')));
  CHECK(composer.text() == "two!");
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.text() == "one");
  REQUIRE(composer.on_event(key(Key::Down)));
  CHECK(composer.text() == "two!");
  REQUIRE(composer.on_event(key(Key::Down)));
  CHECK(composer.text() == "draft");
  CHECK(composer.cursor_pos() == 5);
}

TEST_CASE("Composer: empty and reset history do not replace the draft",
          "[composer][history][failure]") {
  Composer composer = focused_composer();
  composer.set_text("draft");
  REQUIRE(composer.on_event(key(Key::Up)));
  CHECK(composer.text() == "draft");

  composer.push_history("");
  CHECK(composer.history_size() == 1);
  composer.clear_history();
  CHECK(composer.history_size() == 0);
  CHECK(composer.text() == "draft");
}

TEST_CASE("Composer: Enter modes keep submission parent-owned",
          "[composer][input]") {
  Composer composer = focused_composer();

  CHECK_FALSE(composer.on_event(key(Key::Enter)));
  CHECK_FALSE(composer.on_event(modified_enter(false, false, true)));
  REQUIRE(composer.on_event(modified_enter(true, false)));
  CHECK(composer.text() == "\n");
  REQUIRE(composer.on_event(modified_enter(false, true)));
  CHECK(composer.text() == "\n\n");

  composer.set_enter_mode(ComposerEnterMode::Newline);
  REQUIRE(composer.on_event(key(Key::Enter)));
  CHECK(composer.text() == "\n\n\n");
  CHECK_FALSE(composer.on_event(key(Key::Escape)));
  CHECK_FALSE(composer.on_event(key(Key::Tab)));
  CHECK_FALSE(composer.on_event(modified_char(U'l', true, false)));
  CHECK_FALSE(composer.on_event(modified_char(U'x', false, true)));
}

TEST_CASE("Composer: paste normalizes lines and the renderer contains controls",
          "[composer][paste][failure]") {
  Composer composer = focused_composer();
  std::vector<std::string> changes;
  composer.on_change([&](const std::string& text) { changes.push_back(text); });

  REQUIRE(composer.on_event(PasteEvent{"a\r\nb\rc\t\033[2J"}));
  REQUIRE(changes.size() == 1);
  CHECK(composer.text() == std::string{"a\nb\nc\t\033[2J"});
  CHECK(changes.front() == composer.text());

  Screen screen{8, 3};
  composer.set_geometry({0, 0, 8, 3});
  composer.draw(screen);
  CHECK(row_text(screen, 0) == "a       ");
  CHECK(row_text(screen, 1) == "b       ");
  CHECK(row_text(screen, 2) == "c       ");
}

TEST_CASE("Composer: on_change may replace its own callback and text",
          "[composer][callback][failure]") {
  Composer composer = focused_composer();
  int first = 0;
  int second = 0;
  composer.on_change([&](const std::string&) {
    ++first;
    composer.on_change([&](const std::string&) { ++second; });
    composer.set_text("replacement");
  });

  REQUIRE(composer.on_event(ch(U'x')));
  CHECK(first == 1);
  CHECK(composer.text() == "replacement");
  REQUIRE(composer.on_event(ch(U'y')));
  CHECK(second == 1);
}

TEST_CASE("Composer: viewport follows the cursor and clears stale rows",
          "[composer][viewport][failure]") {
  Screen screen{4, 2};
  Composer composer = focused_composer(4, 2);
  composer.set_text("a\nb\nc\nd");
  composer.draw(screen);
  CHECK(row_text(screen, 0) == "c   ");
  CHECK(row_text(screen, 1) == "d   ");

  composer.clear();
  composer.draw(screen);
  CHECK(row_text(screen, 0) == "    ");
  CHECK(row_text(screen, 1) == "    ");
}

TEST_CASE("Composer: click maps a visual row and display column to bytes",
          "[composer][mouse][failure]") {
  Screen screen{4, 2};
  Composer composer;
  composer.set_geometry({0, 0, 4, 2});
  composer.set_text("abc\ndef");
  composer.draw(screen);

  REQUIRE(composer.on_event(press(1, 1)));
  CHECK(composer.focused());
  CHECK(composer.cursor_pos() == 5); // after 'd'
}

TEST_CASE("Composer: FocusRing routes paste and leaves Enter to the parent",
          "[composer][focus][caller-order]") {
  Composer composer;
  composer.set_geometry({0, 0, 20, 2});
  FocusRing ring;
  ring.add(&composer);

  REQUIRE(ring.handle_key(PasteEvent{"hello\nworld"}));
  CHECK(composer.text() == "hello\nworld");
  CHECK_FALSE(ring.handle_key(key(Key::Enter)));
}
