#include <catch2/catch_test_macros.hpp>
#include <variant>
#include "termforge/core/input.hpp"

using termforge::Event;
using termforge::Input;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;
using termforge::PasteEvent;
using termforge::ResizeEvent;

namespace {
auto first_key(std::deque<Event>& ev) -> KeyEvent {
  REQUIRE(!ev.empty());
  auto* k = std::get_if<KeyEvent>(&ev.front());
  REQUIRE(k != nullptr);
  return *k;
}
}

TEST_CASE("Input: plain ASCII chars decode", "[input]") {
  Input in;
  auto ev = in.decode("abc");
  REQUIRE(ev.size() == 3);
  REQUIRE(first_key(ev).ch == 'a');
}

TEST_CASE("Input: multibyte UTF-8 decodes to a code point", "[input]") {
  Input in;
  auto ev = in.decode("\xC3\xA9");  // é (U+00E9)
  REQUIRE(ev.size() == 1);
  REQUIRE(first_key(ev).ch == 0xE9);
}

TEST_CASE("Input: arrow keys via CSI", "[input]") {
  Input in;
  auto up = in.decode("\033[A");
  REQUIRE(first_key(up).key == Key::Up);
  auto down = in.decode("\033[B");
  REQUIRE(first_key(down).key == Key::Down);
  auto right = in.decode("\033[C");
  REQUIRE(first_key(right).key == Key::Right);
  auto left = in.decode("\033[D");
  REQUIRE(first_key(left).key == Key::Left);
}

TEST_CASE("Input: special keys (Enter, Backspace, Tab, Delete)", "[input]") {
  Input in;
  auto enter = in.decode("\r");
  REQUIRE(first_key(enter).key == Key::Enter);
  auto bs = in.decode("\x7F");
  REQUIRE(first_key(bs).key == Key::Backspace);
  auto tab = in.decode("\t");
  REQUIRE(first_key(tab).key == Key::Tab);
  auto del = in.decode("\033[3~");
  REQUIRE(first_key(del).key == Key::Delete);
}

TEST_CASE("Input: Ctrl+letter sets the ctrl modifier", "[input]") {
  Input in;
  auto ev = in.decode("\x03");  // Ctrl+C
  auto k = first_key(ev);
  REQUIRE(k.ctrl);
  REQUIRE(k.ch == 'c');
}

TEST_CASE("Input: malformed/truncated escape doesn't wedge the parser", "[input][failure]") {
  Input in;
  // A lone ESC[ with no final byte must not crash or loop; it needs more data.
  auto ev = in.decode("\033[");
  REQUIRE(ev.empty());  // incomplete -> nothing yet
  // A bare ESC followed by an unknown sequence returns gracefully.
  auto ev2 = in.decode("\033");
  // should not crash; may yield nothing or an unknown
  REQUIRE(ev2.size() <= 1);
}


TEST_CASE("Input: a lone ESC decodes as Escape (quit key)", "[input][failure]") {
  // Regression: ESC alone previously waited forever for a second byte that
  // never comes, so ESC-to-quit never fired. A trailing lone ESC must decode.
  Input in;
  auto ev = in.decode("\x1B");
  REQUIRE(!ev.empty());
  REQUIRE(first_key(ev).key == Key::Escape);
}

TEST_CASE("Input: escape sequences still decode (not mistaken for lone ESC)", "[input]") {
  Input in;
  auto up = in.decode("\x1B[A");
  REQUIRE(first_key(up).key == Key::Up);
  auto down = in.decode("\x1B[B");
  REQUIRE(first_key(down).key == Key::Down);
  // split across feeds: a held ESC is committed by flush() — the caller's
  // "fd drained" boundary signal (see input.hpp) — not by feed() alone,
  // which cannot know whether the sequence continues in the kernel buffer.
  Input in2;
  in2.feed("\x1B");
  in2.flush();
  auto ev2 = in2.poll();  // lone ESC flushed as Escape
  REQUIRE(!ev2.empty());
}

TEST_CASE("Input: resize event is pushed", "[input]") {
  Input in;
  in.push_resize(120, 40);
  auto ev = in.poll();
  auto* r = std::get_if<ResizeEvent>(&ev.front());
  REQUIRE(r != nullptr);
  REQUIRE(r->cols == 120);
  REQUIRE(r->rows == 40);
}

TEST_CASE("Input: SGR mouse press decodes", "[input][mouse]") {
  Input in;
  // ESC [ < 0 ; 10 ; 5 M — button 0, x=10, y=5, press.
  auto ev = in.decode("\033[<0;10;5M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->pressed);
  REQUIRE(m->button == 0);
  REQUIRE(m->x == 9);   // 1-based → 0-based
  REQUIRE(m->y == 4);
}

TEST_CASE("Input: SGR mouse release decodes", "[input][mouse]") {
  Input in;
  auto ev = in.decode("\033[<0;10;5m");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE_FALSE(m->pressed);
}

TEST_CASE("Input: drag motion is not a press", "[input][mouse]") {
  // Regression: ?1002h motion-while-pressed sets bit 5 (32) in the button
  // code; unmasked it decoded as a fresh press, so dragging across a
  // Button re-fired its click handler continuously.
  Input in;
  auto ev = in.decode("\033[<32;5;3M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE_FALSE(m->pressed);
  REQUIRE(m->button == 0);
  REQUIRE(m->x == 4);
  REQUIRE(m->y == 2);
}

TEST_CASE("Input: SGR mouse scroll wheel decodes", "[input][mouse]") {
  Input in;
  // Scroll up: button 64 (bit 6 set, low bit 0).
  auto ev = in.decode("\033[<64;15;8M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->scroll_up);
  REQUIRE_FALSE(m->scroll_down);
  REQUIRE(m->x == 14);
  REQUIRE(m->y == 7);
}

TEST_CASE("Input: SGR mouse scroll down decodes", "[input][mouse]") {
  Input in;
  auto ev = in.decode("\033[<65;15;8M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->scroll_down);
  REQUIRE_FALSE(m->scroll_up);
}

TEST_CASE("Input: scroll wheel events are not button presses",
          "[input][mouse]") {
  // Regression: wheel events kept button = btn & 3 and pressed = true, so
  // a scroll over a Button widget registered as a left click.
  Input in;
  auto ev = in.decode("\033[<64;5;5M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->scroll_up);
  REQUIRE_FALSE(m->pressed);
  REQUIRE(m->button != 0);
}

TEST_CASE("Input: oversized CSI parameters do not overflow", "[input][mouse]") {
  // Regression: unbounded p = p * 10 + digit overflowed int (UB) on a
  // hostile digit run. Parameters are now capped during accumulation.
  Input in;
  auto ev = in.decode("\033[<0;99999999999999999999;1M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->x >= 0);  // clamped garbage, but no UB and non-negative
}

TEST_CASE("Input: invalid UTF-8 resynchronizes instead of swallowing keys",
          "[input][security]") {
  // A stray lead byte whose "length" would consume a following real keypress
  // must not eat it. 0xC3 expects one continuation; 'A' is not one.
  Input in;
  auto ev = in.decode("\xC3" "A");
  REQUIRE(ev.size() == 2);
  const auto* bad = std::get_if<KeyEvent>(&ev[0]);
  REQUIRE(bad != nullptr);
  REQUIRE(bad->key == Key::Char);
  REQUIRE(bad->ch == 0xFFFD);  // replacement char for the bad lead
  const auto* a = std::get_if<KeyEvent>(&ev[1]);
  REQUIRE(a != nullptr);
  REQUIRE(a->key == Key::Char);
  REQUIRE(a->ch == U'A');  // the following keypress survives
}

TEST_CASE("Input: a bad lead doesn't eat a following ESC sequence",
          "[input][security]") {
  // 0xF0 expects three continuations; a following ESC must not be consumed
  // as one. The arrow key after the stray byte must decode.
  Input in;
  auto ev = in.decode("\xF0\x1B[A");
  bool saw_replacement = false;
  bool saw_up = false;
  for (const auto& e : ev) {
    if (const auto* k = std::get_if<KeyEvent>(&e)) {
      if (k->key == Key::Char && k->ch == 0xFFFD) saw_replacement = true;
      if (k->key == Key::Up) saw_up = true;
    }
  }
  REQUIRE(saw_replacement);
  REQUIRE(saw_up);
}

TEST_CASE("Input: invalid lead bytes are rejected, not passed as chars",
          "[input][security]") {
  Input in;
  for (const char* bad : {"\xF8", "\xF9", "\xFC", "\xFE", "\xFF"}) {
    auto ev = in.decode(std::string{bad});
    REQUIRE(ev.size() == 1);
    const auto* k = std::get_if<KeyEvent>(&ev.front());
    REQUIRE(k != nullptr);
    REQUIRE(k->ch == 0xFFFD);  // never a raw invalid byte
  }
}

TEST_CASE("Input: incomplete multibyte at end of stream waits then flushes",
          "[input][failure]") {
  // A genuine split glyph across two reads completes normally.
  Input in;
  in.feed("\xC3");           // é lead, continuation not yet arrived
  in.feed("\xA9");           // completes U+00E9
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  REQUIRE(std::get_if<KeyEvent>(&ev.front())->ch == 0xE9);
}

TEST_CASE("Input: valid multibyte still decodes after hardening",
          "[input]") {
  Input in;
  auto ev = in.decode("h\xC3\xA9llo \xE4\xB8\xAD \xF0\x9F\x8E\x89");
  std::u32string got;
  for (const auto& e : ev)
    if (const auto* k = std::get_if<KeyEvent>(&e)) got += k->ch;
  REQUIRE(got == U"h\xE9llo \x4E2D \x1F389");
}

// ── SS3 (ESC O …) — issue #13.3 ───────────────────────────────────────────

TEST_CASE("Input: SS3 Home decodes as Home, not a spurious 'H'",
          "[input][ss3]") {
  // Regression: ESC O H (application-cursor Home) decoded as Alt+O plus a
  // literal Char 'H', so pressing Home typed "H" into a field.
  Input in;
  auto ev = in.decode("\033OH");
  REQUIRE(ev.size() == 1);  // exactly one event — no leaked 'H'
  REQUIRE(first_key(ev).key == Key::Home);
}

TEST_CASE("Input: SS3 arrows and End decode", "[input][ss3]") {
  Input in;
  auto up = in.decode("\033OA");
  REQUIRE(first_key(up).key == Key::Up);
  auto down = in.decode("\033OB");
  REQUIRE(first_key(down).key == Key::Down);
  auto right = in.decode("\033OC");
  REQUIRE(first_key(right).key == Key::Right);
  auto left = in.decode("\033OD");
  REQUIRE(first_key(left).key == Key::Left);
  auto end = in.decode("\033OF");
  REQUIRE(first_key(end).key == Key::End);
}

TEST_CASE("Input: SS3 F1–F4 decode", "[input][ss3]") {
  Input in;
  auto f1 = in.decode("\033OP");
  REQUIRE(first_key(f1).key == Key::F1);
  auto f2 = in.decode("\033OQ");
  REQUIRE(first_key(f2).key == Key::F2);
  auto f3 = in.decode("\033OR");
  REQUIRE(first_key(f3).key == Key::F3);
  auto f4 = in.decode("\033OS");
  REQUIRE(first_key(f4).key == Key::F4);
}

// ── key modifiers (CSI ;<mod>) — issue #13.4 ──────────────────────────────

TEST_CASE("Input: Ctrl+Right via ESC[1;5C sets ctrl", "[input][mods]") {
  // Regression: the modifier parameter was parsed but never applied, so
  // ESC[1;5C arrived as a plain Right and word-jump was impossible.
  Input in;
  auto ev = in.decode("\033[1;5C");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::Right);
  REQUIRE(k.ctrl);
  REQUIRE_FALSE(k.alt);
  REQUIRE_FALSE(k.shift);
}

TEST_CASE("Input: Shift+Up via ESC[1;2A sets shift", "[input][mods]") {
  Input in;
  auto ev = in.decode("\033[1;2A");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::Up);
  REQUIRE(k.shift);
  REQUIRE_FALSE(k.ctrl);
}

TEST_CASE("Input: Ctrl+Delete via ESC[3;5~ sets ctrl", "[input][mods]") {
  Input in;
  auto ev = in.decode("\033[3;5~");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::Delete);
  REQUIRE(k.ctrl);
}

TEST_CASE("Input: unmodified arrow leaves modifiers clear", "[input][mods]") {
  Input in;
  auto ev = in.decode("\033[C");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::Right);
  REQUIRE_FALSE(k.ctrl);
  REQUIRE_FALSE(k.alt);
  REQUIRE_FALSE(k.shift);
}

// ── mouse modifiers + wheel sentinel — issue #13.4 / #13.6 ────────────────

TEST_CASE("Input: Ctrl+left click carries the ctrl modifier",
          "[input][mouse][mods]") {
  // SGR button code 16 = ctrl bit; low bits still button 0.
  Input in;
  auto ev = in.decode("\033[<16;5;5M");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->button == 0);
  REQUIRE(m->pressed);
  REQUIRE(m->ctrl);
  REQUIRE_FALSE(m->shift);
  REQUIRE_FALSE(m->alt);
}

TEST_CASE("Input: wheel event reports the -1 (none) button sentinel",
          "[input][mouse]") {
  // types.hpp documents button -1 = none (wheel/motion); pin it so consumers
  // trusting the 0/1/2 domain aren't surprised by a stray value.
  Input in;
  auto ev = in.decode("\033[<64;5;5M");
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE(m->button == -1);
  REQUIRE(m->scroll_up);
}

// ── bracketed paste — issue #13.5 ─────────────────────────────────────────

TEST_CASE("Input: bracketed paste yields one PasteEvent", "[input][paste]") {
  Input in;
  auto ev = in.decode("\033[200~hello\033[201~");
  REQUIRE(ev.size() == 1);
  auto* p = std::get_if<PasteEvent>(&ev.front());
  REQUIRE(p != nullptr);
  REQUIRE(p->text == "hello");
}

TEST_CASE("Input: an ESC inside a paste is content, not an Escape key",
          "[input][paste][failure]") {
  // Regression target: without mode 2004 an ESC in pasted text fabricated an
  // Escape keypress (which quits the default app). It must stay paste content.
  Input in;
  auto ev = in.decode("\033[200~a\033b\033[201~");
  REQUIRE(ev.size() == 1);
  auto* p = std::get_if<PasteEvent>(&ev.front());
  REQUIRE(p != nullptr);
  REQUIRE(p->text == std::string("a\033b"));
}

TEST_CASE("Input: a paste terminator split across feeds still yields one event",
          "[input][paste]") {
  Input in;
  in.feed("\033[200~hi\033[2");  // terminator begins…
  REQUIRE(in.poll().empty());     // …but isn't complete yet — no event
  in.feed("01~");                 // completes ESC[201~
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  auto* p = std::get_if<PasteEvent>(&ev.front());
  REQUIRE(p != nullptr);
  REQUIRE(p->text == "hi");
}

TEST_CASE("Input: an empty paste yields an empty PasteEvent", "[input][paste]") {
  Input in;
  auto ev = in.decode("\033[200~\033[201~");
  REQUIRE(ev.size() == 1);
  auto* p = std::get_if<PasteEvent>(&ev.front());
  REQUIRE(p != nullptr);
  REQUIRE(p->text.empty());
}

TEST_CASE("Input: a stray paste-end with no open paste is swallowed",
          "[input][paste]") {
  Input in;
  auto ev = in.decode("\033[201~");
  REQUIRE(ev.empty());  // no spurious key, no crash
}

