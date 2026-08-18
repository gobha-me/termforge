#include "detail/wrap.hpp"
#include "support/events.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/styled_text.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/theme.hpp"
#include <catch2/catch_test_macros.hpp>

using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::Rect;
using termforge::Rgb;
using termforge::Screen;
using termforge::TextBox;

namespace {
// Render the widget into a fresh screen and read back row y as a string.
auto render_row(TextBox& box, int width, int height, int y) -> std::string {
  Screen s{width, height};
  box.set_geometry({0, 0, width, height});
  box.draw(s);
  std::string row;
  for (int x = 0; x < width; ++x)
    row += s.text_at(x, y);
  return row;
}

auto plain_rows(const std::string& text, int width)
    -> std::vector<std::string> {
  std::vector<std::string> rows;
  termforge::detail::wrap_into(rows, text, width);
  return rows;
}

auto styled_text(const termforge::StyledText& row) -> std::string {
  std::string text;
  for (const termforge::TextSpan& span : row)
    text += span.text;
  return text;
}
} // namespace

TEST_CASE("TextBox: appends and shows the latest lines pinned to the bottom",
          "[textbox]") {
  TextBox box;
  box.append("one");
  box.append("two");
  box.append("three");
  REQUIRE(box.line_count() == 3);
  REQUIRE(box.at_bottom());
  // 5-row view, 3 lines -> bottom-aligned, last row is "three"
  REQUIRE(render_row(box, 20, 5, 2) == "three");
  REQUIRE(render_row(box, 20, 5, 0) == "one");
}

TEST_CASE("TextBox: long lines wrap to the widget width", "[textbox]") {
  TextBox box;
  box.append("abcdefghij"); // 10 chars into width 4 -> 3 rows: abcd efgh ij
  REQUIRE(render_row(box, 4, 5, 0) == "abcd");
  REQUIRE(render_row(box, 4, 5, 1) == "efgh");
  REQUIRE(render_row(box, 4, 5, 2) == "ij");
}

TEST_CASE("TextBox: prose wraps at the last fitting word boundary (#24)",
          "[textbox][wrap]") {
  TextBox box;
  box.append("alpha beta gamma");

  REQUIRE(render_row(box, 8, 5, 0) == "alpha ");
  REQUIRE(render_row(box, 8, 5, 1) == "beta ");
  REQUIRE(render_row(box, 8, 5, 2) == "gamma");
}

TEST_CASE("TextBox: resizing reflows prose through the same word policy (#24)",
          "[textbox][wrap][resize]") {
  TextBox box;
  box.append("alpha beta");

  REQUIRE(render_row(box, 7, 3, 0) == "alpha ");
  REQUIRE(render_row(box, 7, 3, 1) == "beta");
  REQUIRE(render_row(box, 10, 3, 0) == "alpha beta");
}

TEST_CASE("TextBox: word wrapping preserves whitespace exactly (#24)",
          "[textbox][wrap][failure]") {
  const auto leading = plain_rows("  hi", 4);
  REQUIRE(leading == std::vector<std::string>{"  hi"});

  const auto trailing = plain_rows("hi  ", 4);
  REQUIRE(trailing == std::vector<std::string>{"hi  "});

  const std::string repeated = "aa   bb";
  const auto rows = plain_rows(repeated, 4);
  REQUIRE(rows == std::vector<std::string>{"aa  ", " bb"});
  std::string rebuilt;
  for (const auto& row : rows)
    rebuilt += row;
  REQUIRE(rebuilt == repeated);
}

TEST_CASE(
    "TextBox: overlong words retain display-width-safe hard wrapping (#24)",
    "[textbox][wrap][failure]") {
  REQUIRE(plain_rows("abcde", 2) == std::vector<std::string>{"ab", "cd", "e"});
  REQUIRE(plain_rows("word", 4) == std::vector<std::string>{"word"});
  REQUIRE(plain_rows("ab cd", 1) ==
          std::vector<std::string>{"a", "b", " ", "c", "d"});
  REQUIRE(plain_rows("", 4) == std::vector<std::string>{""});
  REQUIRE(plain_rows("no wrap", 0) == std::vector<std::string>{"no wrap"});

  const std::string wide = "\xE7\x95\x8C hello"; // 界 is two columns.
  const auto rows = plain_rows(wide, 4);
  REQUIRE(rows == std::vector<std::string>{"\xE7\x95\x8C ", "hell", "o"});
  std::string rebuilt;
  for (const auto& row : rows)
    rebuilt += row;
  REQUIRE(rebuilt == wide);
}

TEST_CASE("TextBox: word breaks cross spans without losing style (#24)",
          "[textbox][styled][wrap]") {
  using termforge::Attr;
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  const TextStyle red{Rgb{0xFF, 0, 0}, Rgb{1, 2, 3}, Attr::Bold};
  const TextStyle gap{Rgb{0, 0xFF, 0}, Rgb{4, 5, 6}, Attr::Underline};
  const TextStyle blue{Rgb{0, 0, 0xFF}, Rgb{7, 8, 9}, Attr::Italic};
  const StyledText line{{"red", red}, {" ", gap}, {"blue", blue}};

  std::vector<StyledText> rows;
  termforge::detail::wrap_styled_into(rows, line, 4);
  REQUIRE(rows.size() == 2);
  REQUIRE(styled_text(rows[0]) == "red ");
  REQUIRE(rows[0].size() == 2);
  CHECK(rows[0][0].style == red);
  CHECK(rows[0][1].style == gap);
  REQUIRE(styled_text(rows[1]) == "blue");
  REQUIRE(rows[1].size() == 1);
  CHECK(rows[1][0].style == blue);
}

TEST_CASE("TextBox: a word split across spans is still one word (#24)",
          "[textbox][styled][wrap][failure]") {
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  const TextStyle a{Rgb{1, 0, 0}, {}};
  const TextStyle b{Rgb{0, 1, 0}, {}};
  const StyledText line{{"ab", a}, {"cd", b}, {" ef", a}};
  std::vector<StyledText> rows;
  termforge::detail::wrap_styled_into(rows, line, 4);

  REQUIRE(rows.size() == 2);
  CHECK(styled_text(rows[0]) == "abcd");
  CHECK(rows[0].size() == 2);
  CHECK(rows[0][0].style == a);
  CHECK(rows[0][1].style == b);
  CHECK(styled_text(rows[1]) == " ef");
}

TEST_CASE("TextBox: scrolling up pauses follow; new content does not yank view",
          "[textbox]") {
  TextBox box;
  for (int i = 0; i < 20; ++i)
    box.append("line " + std::to_string(i));
  box.scroll(-5); // scroll up 5
  REQUIRE_FALSE(box.at_bottom());
  const auto before = box.line_count();
  box.append("new line");
  REQUIRE(box.line_count() == before + 1u);
  REQUIRE_FALSE(box.at_bottom()); // still scrolled up, follow paused
}

TEST_CASE("TextBox: PageUp/PageDown scroll a page and are consumed",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 5});
  for (int i = 0; i < 20; ++i)
    box.append("line " + std::to_string(i));
  KeyEvent pgup{Key::PageUp};
  REQUIRE(box.on_event(Event{pgup}));
  REQUIRE_FALSE(box.at_bottom());
  KeyEvent pgdn{Key::PageDown};
  REQUIRE(box.on_event(Event{pgdn}));
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: scroll_to_bottom restores follow", "[textbox]") {
  TextBox box;
  for (int i = 0; i < 10; ++i)
    box.append("x");
  box.scroll(-3);
  REQUIRE_FALSE(box.at_bottom());
  box.scroll_to_bottom();
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: empty and zero-size rects are safe", "[textbox][failure]") {
  TextBox box;
  box.append("data");
  Screen tiny{0, 0};
  box.set_geometry({0, 0, 0, 0});
  box.draw(tiny); // must not crash
  REQUIRE(true);
}

TEST_CASE("TextBox: scroll indicator appears when scrolled up", "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 20; ++i)
    box.append("line " + std::to_string(i));
  box.scroll(-5);
  Screen s{20, 3};
  box.draw(s);
  // the "[more]" indicator should be somewhere on row 0
  std::string row0;
  for (int x = 0; x < 20; ++x)
    row0 += s.text_at(x, 0);
  REQUIRE(row0.find("[more]") != std::string::npos);
}

TEST_CASE("TextBox: over-scrolling up clamps instead of blanking",
          "[textbox]") {
  // Regression: scroll(-N) grew m_scroll without bound; once it exceeded
  // the wrapped line count the draw window went negative and nothing
  // rendered.
  TextBox box;
  box.set_geometry({0, 0, 20, 10});
  for (int i = 0; i < 5; ++i)
    box.append("line " + std::to_string(i));

  box.scroll(-9); // PageUp with everything already visible
  box.scroll(-9);
  Screen s{20, 10};
  box.draw(s);

  std::string all;
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 20; ++x)
      all += s.text_at(x, y);
  REQUIRE(all.find("line 0") != std::string::npos);
  REQUIRE(all.find("line 4") != std::string::npos);
  // Content fits entirely → the clamp lands back at the bottom.
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: wheel scrolls the view, sign convention unchanged (#35)",
          "[textbox]") {
  // #35 routed TextBox's wheel through the shared viewport helper. Its
  // m_scroll is INVERTED relative to the library convention (0 == pinned to
  // bottom, larger == scrolled further up), so the helper's uniform direction
  // is converted at the widget's boundary. This pins that the app-visible
  // behaviour -- at_bottom() and the m_follow auto-scroll latch -- is
  // byte-for-byte unchanged.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 20; ++i)
    box.append("line " + std::to_string(i));
  REQUIRE(box.at_bottom());

  // Wheel UP over the box scrolls toward older lines: no longer at the bottom.
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  REQUIRE_FALSE(box.at_bottom());

  // Wheel back DOWN returns to the bottom and re-arms the follow latch.
  box.on_event(tfsupport::wheel(2, 1, /*up=*/false));
  REQUIRE(box.at_bottom());

  // The follow latch still auto-scrolls on new content only while at the
  // bottom: scroll up, append, the view does NOT jump to the new line...
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.append("new line");
  REQUIRE_FALSE(box.at_bottom());
  // ...and an explicit scroll_to_bottom re-pins it.
  box.scroll_to_bottom();
  REQUIRE(box.at_bottom());
}

TEST_CASE(
    "TextBox: scrollbar appears when content overflows, thumb at bottom (#21)",
    "[textbox]") {
  // A TextBox pins to the bottom, so the bar's first honest state is a thumb
  // AT THE BOTTOM -- the opposite end from a fresh list. m_scroll counts up
  // from the bottom; the strip gets the converted offset at the boundary.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 3; ++i)
    box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  REQUIRE(s.text_at(19, 0) != "█"); // everything fits: no bar
  for (int i = 3; i < 12; ++i)
    box.append("line " + std::to_string(i));
  box.draw(s);
  REQUIRE(s.text_at(19, 3) == "█"); // pinned to the bottom
  REQUIRE(s.text_at(19, 0) == "│");
}

TEST_CASE("TextBox: scrollbar thumb moves as the view scrolls up (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 12; ++i)
    box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  box.scroll_to_bottom(); // no-op state-wise, pins intent
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.draw(s);
  // 12 rows, 4 visible, scrolled up 9 -> clamped to offset-from-bottom 8 =
  // the TOP of the content: thumb at the top row.
  REQUIRE(s.text_at(19, 0) == "█");
  REQUIRE(s.text_at(19, 3) == "│");
}

TEST_CASE("TextBox: scrollbar glyphs follow the ascii style (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  box.set_style(termforge::BorderStyle::Ascii);
  for (int i = 0; i < 12; ++i)
    box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  REQUIRE(s.text_at(19, 3) == "#");
  REQUIRE(s.text_at(19, 0) == "|");
}

TEST_CASE("TextBox: [more] chip and scrollbar coexist (#21)", "[textbox]") {
  // Orthogonal indicators: the chip marks the follow LATCH (auto-scroll
  // paused), the bar marks the viewport POSITION. Scrolled up, both show.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 12; ++i)
    box.append("line " + std::to_string(i));
  box.scroll(-6);
  Screen s{20, 4};
  box.draw(s);
  std::string row0;
  for (int x = 0; x < 20; ++x)
    row0 += s.text_at(x, 0);
  REQUIRE(row0.find("[more]") != std::string::npos);
  // 12 rows, 4 visible: the thumb covers a third of the track (rounds to 1
  // row), and scrolled up 6 of the 8 max it sits one row down from the top.
  REQUIRE(s.text_at(19, 0) == "│");
  REQUIRE(s.text_at(19, 1) == "█");
  // Back at the bottom the chip clears but the bar stays (still scrollable).
  box.scroll_to_bottom();
  box.draw(s);
  row0.clear();
  for (int x = 0; x < 20; ++x)
    row0 += s.text_at(x, 0);
  REQUIRE(row0.find("[more]") == std::string::npos);
  REQUIRE(s.text_at(19, 3) == "█");
}

TEST_CASE("TextBox: wrapping leaves the scrollbar column free (#21)",
          "[textbox]") {
  // With the bar up the wrap width is one column narrower, so text never
  // paints under the strip and the strip never overpaints text.
  TextBox box;
  box.set_geometry({0, 0, 10, 3});
  for (int i = 0; i < 6; ++i)
    box.append("0123456789"); // 10 chars, wraps
  Screen s{10, 3};
  box.draw(s);
  // 10-char lines wrap at 9 columns with the bar possible: no row's text
  // reaches the last column.
  for (int y = 0; y < 3; ++y) {
    const std::string_view last = s.text_at(9, y);
    REQUIRE((last == "█" || last == "│"));
  }
}

TEST_CASE("TextBox: click on the scrollbar column page-jumps the view (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 20; ++i)
    box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s); // bar up
  // Upper half of the strip -> toward older (up a page).
  REQUIRE(box.on_event(tfsupport::press(19, 0)));
  REQUIRE_FALSE(box.at_bottom());
  // Lower half -> back toward newer. Two page-ups happened above? No: one.
  REQUIRE(box.on_event(tfsupport::press(19, 3)));
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: a one-column rect keeps the text, drops the bar (#21)",
          "[textbox][failure]") {
  TextBox box;
  box.set_geometry({0, 0, 1, 3});
  for (int i = 0; i < 9; ++i)
    box.append("x");
  Screen s{1, 3};
  box.draw(s); // must not crash
  // The narrow exception, resolving the OTHER way from ListWidget's w == 2
  // (which gives the strip the last column): a 1-wide TextBox shows one
  // clipped text column and no bar -- position-only is the worse trade for
  // a widget whose whole job is text, and its caller can give it two
  // columns. The wrap still ran (cw 0 -> wrap_into's "don't wrap" passes the
  // lines through), so the pinned-to-the-bottom view shows the newest lines.
  REQUIRE(s.text_at(0, 2) == "x");
  REQUIRE(s.text_at(0, 0) == "x");
  REQUIRE(s.text_at(0, 0) != "█");
  REQUIRE(s.text_at(0, 0) != "│");
}

TEST_CASE("TextBox: styled append paints per-span fg/bg/attrs (#25)",
          "[textbox][styled]") {
  using termforge::Attr;
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  TextBox box;
  box.set_geometry({0, 0, 20, 3});
  const Rgb red{0xFF, 0, 0};
  const Rgb blue{0, 0, 0xFF};
  StyledText line;
  line.push_back(TextSpan{"hi", TextStyle{red, {}, Attr::Bold}});
  line.push_back(TextSpan{"there", TextStyle{blue, {}, Attr::None}});
  box.append(std::move(line));

  Screen s{20, 3};
  box.draw(s);
  REQUIRE(s.text_at(0, 0) == "h");
  REQUIRE(s.at(0, 0).fg == red);
  REQUIRE(s.at(0, 0).attrs == Attr::Bold);
  REQUIRE(s.text_at(2, 0) == "t");
  REQUIRE(s.at(2, 0).fg == blue);
  REQUIRE(s.at(2, 0).attrs == Attr::None);
}

TEST_CASE("TextBox: span boundary exactly at wrap column (#25)",
          "[textbox][styled]") {
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  TextBox box;
  const Rgb a{1, 0, 0}, b{0, 1, 0};
  StyledText line;
  line.push_back(TextSpan{"abcd", TextStyle{a, {}}});
  line.push_back(TextSpan{"efgh", TextStyle{b, {}}});
  box.append(std::move(line));

  Screen s{4, 5};
  box.set_geometry({0, 0, 4, 5});
  box.draw(s);
  REQUIRE(s.text_at(0, 0) == "a");
  REQUIRE(s.at(0, 0).fg == a);
  REQUIRE(s.text_at(3, 0) == "d");
  REQUIRE(s.at(3, 0).fg == a);
  REQUIRE(s.text_at(0, 1) == "e");
  REQUIRE(s.at(0, 1).fg == b);
  REQUIRE(s.text_at(3, 1) == "h");
  REQUIRE(s.at(3, 1).fg == b);
}

TEST_CASE("TextBox: style continues across a wrapped span (#25)",
          "[textbox][styled]") {
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  TextBox box;
  const Rgb red{0xCC, 0, 0};
  const Rgb blue{0, 0, 0xCC};
  StyledText line;
  line.push_back(TextSpan{"ab", TextStyle{red, {}}});
  line.push_back(TextSpan{"cdef", TextStyle{blue, {}}});
  box.append(std::move(line));

  Screen s{4, 5};
  box.set_geometry({0, 0, 4, 5});
  box.draw(s);
  // Row 0: ab + cd; row 1: ef — blue continues onto the continuation row.
  REQUIRE(s.at(0, 0).fg == red);
  REQUIRE(s.at(1, 0).fg == red);
  REQUIRE(s.at(2, 0).fg == blue);
  REQUIRE(s.at(3, 0).fg == blue);
  REQUIRE(s.text_at(0, 1) == "e");
  REQUIRE(s.at(0, 1).fg == blue);
  REQUIRE(s.text_at(1, 1) == "f");
  REQUIRE(s.at(1, 1).fg == blue);
}

TEST_CASE("TextBox: zero-length spans paint nothing (#25)",
          "[textbox][styled]") {
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  TextBox box;
  const Rgb fg{0x10, 0x20, 0x30};
  StyledText line;
  line.push_back(TextSpan{"", TextStyle{fg, {}}});
  line.push_back(TextSpan{"ok", TextStyle{fg, {}}});
  line.push_back(TextSpan{"", TextStyle{fg, {}}});
  box.append(std::move(line));

  Screen s{10, 2};
  box.set_geometry({0, 0, 10, 2});
  box.draw(s);
  REQUIRE(s.text_at(0, 0) == "o");
  REQUIRE(s.text_at(1, 0) == "k");
  REQUIRE(s.text_at(2, 0).empty());
}

TEST_CASE("TextBox: sanitizes each span at append (#25)",
          "[textbox][styled][security]") {
  using termforge::StyledText;
  using termforge::TextSpan;
  using termforge::TextStyle;

  TextBox box;
  StyledText line;
  line.push_back(TextSpan{"hi\033[1J", TextStyle{termforge::theme::kFg, {}}});
  line.push_back(TextSpan{"there\007", TextStyle{termforge::theme::kFg, {}}});
  box.append(std::move(line));

  Screen s{20, 2};
  box.set_geometry({0, 0, 20, 2});
  box.draw(s);
  std::string row;
  for (int x = 0; x < 10; ++x)
    row += s.text_at(x, 0);
  REQUIRE(row.substr(0, 7) == "hithere");
}

TEST_CASE("TextBox: plain append is a single-span wrapper (#25)",
          "[textbox][styled]") {
  // Same glyphs as the historical plain path; colours match theme defaults.
  TextBox plain;
  plain.append("wrapme!!"); // 8 chars -> two rows at width 4
  REQUIRE(render_row(plain, 4, 5, 0) == "wrap");
  REQUIRE(render_row(plain, 4, 5, 1) == "me!!");

  Screen s{4, 5};
  plain.set_geometry({0, 0, 4, 5});
  plain.draw(s);
  REQUIRE(s.at(0, 0).fg == termforge::theme::kFg);
}

TEST_CASE("TextBox: a live entry appends, replaces, and finalizes (#217)",
          "[textbox][stream]") {
  TextBox box;
  const auto entry = box.begin_entry("hel");
  REQUIRE(entry);
  CHECK(box.line_count() == 1);
  CHECK(box.retained_bytes() == 3);
  CHECK(render_row(box, 20, 3, 0) == "hel");

  REQUIRE(box.append_to_entry(entry, "lo"));
  CHECK(box.retained_bytes() == 5);
  CHECK(render_row(box, 20, 3, 0) == "hello");
  CHECK(box.at_bottom());

  REQUIRE(box.replace_entry(entry, "world"));
  CHECK(box.retained_bytes() == 5);
  CHECK(render_row(box, 20, 3, 0) == "world");

  REQUIRE(box.finalize_entry(entry));
  CHECK_FALSE(box.append_to_entry(entry, "!"));
  CHECK_FALSE(box.replace_entry(entry, "replacement"));
  CHECK_FALSE(box.finalize_entry(entry));
  CHECK(render_row(box, 20, 3, 0) == "world");
}

TEST_CASE("TextBox: beginning a new tail finalizes the prior one (#217)",
          "[textbox][stream][failure]") {
  TextBox box;
  const auto first = box.begin_entry("first");
  const auto second = box.begin_entry("second");

  CHECK_FALSE(box.append_to_entry(first, " stale"));
  REQUIRE(box.append_to_entry(second, " live"));
  CHECK(box.line_count() == 2);
  CHECK(render_row(box, 20, 3, 0) == "first");
  CHECK(render_row(box, 20, 3, 1) == "second live");
}

TEST_CASE("TextBox: streaming UTF-8 survives chunk boundaries (#217)",
          "[textbox][stream][utf8][failure]") {
  TextBox box;
  const auto entry = box.begin_entry();
  const std::string b1(1, static_cast<char>(0xE7));
  const std::string b2(1, static_cast<char>(0x95));
  const std::string b3(1, static_cast<char>(0x8C));

  REQUIRE(box.append_to_entry(entry, b1));
  CHECK(box.retained_bytes() == 1);
  CHECK(render_row(box, 10, 2, 0).empty());
  REQUIRE(box.append_to_entry(entry, b2));
  CHECK(box.retained_bytes() == 2);
  REQUIRE(box.append_to_entry(entry, b3));
  CHECK(box.retained_bytes() == 3);
  CHECK(render_row(box, 10, 2, 0) ==
        std::string{"\xE7\x95\x8C", 3} + std::string(1, '\0'));

  REQUIRE(box.append_to_entry(
      entry, termforge::StyledText{{"!", {Rgb{1, 2, 3}, {}}}}));
  Screen styled{10, 2};
  box.set_geometry({0, 0, 10, 2});
  box.draw(styled);
  CHECK(styled.text_at(0, 0) == "\xE7\x95\x8C");
  CHECK(styled.text_at(1, 0) == std::string(1, '\0'));
  CHECK(styled.text_at(2, 0) == "!");
  CHECK(styled.at(2, 0).fg == Rgb{1, 2, 3});

  const auto incomplete = box.begin_entry(b1);
  CHECK(box.retained_bytes() == 5); // completed glyph + ! + held lead
  REQUIRE(box.finalize_entry(incomplete));
  CHECK(box.retained_bytes() == 4); // finalization drops the held lead

  TextBox malformed;
  const auto malformed_entry = malformed.begin_entry(b1);
  REQUIRE(malformed.append_to_entry(malformed_entry, "A"));
  CHECK(render_row(malformed, 10, 2, 0) == "A");
}

TEST_CASE(
    "TextBox: empty stream deltas are successful cache-stable no-ops (#217)",
    "[textbox][stream][cache]") {
  TextBox box;
  const auto entry = box.begin_entry("stable");
  Screen screen{20, 3};
  box.set_geometry({0, 0, 20, 3});
  box.draw(screen);
  const auto builds = box.wrap_build_count();

  REQUIRE(box.append_to_entry(entry, std::string{}));
  REQUIRE(box.append_to_entry(entry, termforge::StyledText{}));
  box.draw(screen);
  CHECK(box.wrap_build_count() == builds);
  CHECK(box.retained_bytes() == 6);
}

TEST_CASE(
    "TextBox: wrap caches rebuild only for changed entries and widths (#217)",
    "[textbox][stream][cache]") {
  TextBox box;
  box.append("alpha beta");
  box.append("gamma delta");
  const auto live = box.begin_entry("epsilon");
  Screen screen{12, 8};
  box.set_geometry({0, 0, 12, 8});

  box.draw(screen);
  CHECK(box.wrap_build_count() == 3);
  box.draw(screen);
  CHECK(box.wrap_build_count() == 3);

  REQUIRE(box.append_to_entry(live, " zeta"));
  box.draw(screen);
  CHECK(box.wrap_build_count() == 4);

  Screen narrow{8, 8};
  box.set_geometry({0, 0, 8, 8});
  box.draw(narrow);
  CHECK(box.wrap_build_count() == 7);
  box.draw(narrow);
  CHECK(box.wrap_build_count() == 7);

  REQUIRE(box.append_to_entry(live, " eta"));
  box.draw(narrow);
  CHECK(box.wrap_build_count() == 8);
}

TEST_CASE("TextBox: retention evicts finalized entries but exempts the live "
          "tail (#217)",
          "[textbox][stream][retention][failure]") {
  using termforge::TextBoxRetention;

  TextBox box;
  box.set_retention(TextBoxRetention{.max_entries = 2, .max_bytes = 4});
  box.append("old");
  const auto live = box.begin_entry("12345");

  CHECK(box.line_count() == 1); // old was the oldest finalized victim
  CHECK(box.retained_bytes() == 5);
  CHECK(box.retention_over_budget());
  REQUIRE(box.append_to_entry(live, "6"));
  CHECK(box.retained_bytes() == 6);
  CHECK(box.retention_over_budget());

  REQUIRE(box.finalize_entry(live));
  CHECK(box.line_count() == 0); // now eligible, the oversized tail is evicted
  CHECK(box.retained_bytes() == 0);
  CHECK_FALSE(box.retention_over_budget());
}

TEST_CASE(
    "TextBox: an evicted handle stays stale after its slot is reused (#217)",
    "[textbox][stream][retention][failure]") {
  using termforge::TextBoxRetention;

  TextBox box;
  box.set_retention(
      TextBoxRetention{.max_entries = 1, .max_bytes = std::nullopt});
  const auto old = box.begin_entry("old");
  REQUIRE(box.finalize_entry(old));
  const auto middle = box.begin_entry("middle"); // evicts old
  const auto replacement = box.begin_entry("replacement");

  CHECK(old.index == replacement.index); // the physical slot was recycled
  CHECK(old.generation != replacement.generation);
  CHECK_FALSE(box.append_to_entry(old, " corrupt"));
  REQUIRE(box.append_to_entry(replacement, " safe"));
  CHECK_FALSE(box.append_to_entry(middle, " finalized"));
  CHECK(render_row(box, 30, 3, 0) == "replacement safe");

  box.clear();
  CHECK_FALSE(box.append_to_entry(replacement, " after clear"));
}

TEST_CASE(
    "TextBox: streaming growth preserves a scrolled viewport anchor (#217)",
    "[textbox][stream][scroll]") {
  using termforge::TextBoxRetention;

  TextBox box;
  box.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 10; ++i)
    box.append("line " + std::to_string(i));
  Screen screen{20, 3};
  box.draw(screen);
  box.scroll(-3);
  box.draw(screen);
  CHECK(render_row(box, 20, 3, 0).starts_with("line 4"));

  const auto live = box.begin_entry("stream");
  REQUIRE(box.append_to_entry(live, " grows"));
  CHECK(render_row(box, 20, 3, 0).starts_with("line 4"));
  CHECK_FALSE(box.at_bottom());

  box.set_retention(
      TextBoxRetention{.max_entries = 5, .max_bytes = std::nullopt});
  CHECK(render_row(box, 20, 3, 0).starts_with("line 6"));
  CHECK_FALSE(box.at_bottom());
}

TEST_CASE("TextBox: a bounded chunked tail matches one-shot wrapping (#217)",
          "[textbox][stream][retention]") {
  std::string accumulated;
  for (int i = 0; i < 100; ++i)
    accumulated += std::to_string(i) + (i == 99 ? "" : " ");

  TextBox chunked;
  chunked.set_retention(termforge::TextBoxRetention{
      .max_entries = 1, .max_bytes = accumulated.size()});
  const auto live = chunked.begin_entry();
  std::size_t offset = 0;
  for (int i = 0; i < 100; ++i) {
    const std::string chunk = std::to_string(i) + (i == 99 ? "" : " ");
    REQUIRE(chunked.append_to_entry(live, chunk));
    offset += chunk.size();
    CHECK(chunked.line_count() == 1);
    CHECK(chunked.retained_bytes() == offset);
    CHECK_FALSE(chunked.retention_over_budget());
  }

  TextBox one_shot;
  one_shot.append(accumulated);
  CHECK(chunked.retained_bytes() == accumulated.size());
  for (int row = 0; row < 8; ++row)
    CHECK(render_row(chunked, 24, 8, row) == render_row(one_shot, 24, 8, row));
}
