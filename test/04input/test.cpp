#include "termforge/core/input.hpp"
#include <catch2/catch_test_macros.hpp>
#include <variant>

using termforge::Event;
using termforge::Input;
using termforge::Key;
using termforge::KeyAction;
using termforge::KeyEvent;
using termforge::MouseAction;
using termforge::MouseEvent;
using termforge::PasteEvent;
using termforge::ResizeEvent;
using termforge::TerminalReply;

namespace {
auto first_key(std::deque<Event>& ev) -> KeyEvent {
  REQUIRE(!ev.empty());
  auto* k = std::get_if<KeyEvent>(&ev.front());
  REQUIRE(k != nullptr);
  return *k;
}
} // namespace

TEST_CASE("Input: plain ASCII chars decode", "[input]") {
  Input in;
  auto ev = in.decode("abc");
  REQUIRE(ev.size() == 3);
  REQUIRE(first_key(ev).ch == 'a');
}

TEST_CASE("Input: multibyte UTF-8 decodes to a code point", "[input]") {
  Input in;
  auto ev = in.decode("\xC3\xA9"); // é (U+00E9)
  REQUIRE(ev.size() == 1);
  REQUIRE(first_key(ev).ch == 0xE9);
}

TEST_CASE("Input: four-byte UTF-8 preserves high-bit continuation bytes",
          "[input][failure]") {
  Input in;
  auto ev = in.decode("\xF4\x8F\xBF\xBF"); // U+10FFFF
  REQUIRE(ev.size() == 1);
  REQUIRE(first_key(ev).ch == 0x10FFFF);
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
  auto ev = in.decode("\x03"); // Ctrl+C
  auto k = first_key(ev);
  REQUIRE(k.ctrl);
  REQUIRE(k.ch == 'c');
}

TEST_CASE("Input: malformed/truncated escape doesn't wedge the parser",
          "[input][failure]") {
  Input in;
  // A lone ESC[ with no final byte must not crash or loop; it needs more data.
  auto ev = in.decode("\033[");
  REQUIRE(ev.empty()); // incomplete -> nothing yet
  // A bare ESC followed by an unknown sequence returns gracefully.
  auto ev2 = in.decode("\033");
  // should not crash; may yield nothing or an unknown
  REQUIRE(ev2.size() <= 1);
}

TEST_CASE("Input: kitty graphics replies use a separate ordered channel",
          "[input][kitty-reply]") {
  Input in;
  in.feed("a\033_Gi=42,p=7;O");
  CHECK(in.poll_replies().empty());
  in.feed("K\033\\b");

  const auto events = in.poll();
  REQUIRE(events.size() == 2);
  CHECK(std::get<KeyEvent>(events[0]).ch == U'a');
  CHECK(std::get<KeyEvent>(events[1]).ch == U'b');

  const auto replies = in.poll_replies();
  REQUIRE(replies.size() == 1);
  const auto* reply = std::get_if<TerminalReply>(&replies.front());
  REQUIRE(reply != nullptr);
  CHECK(reply->image_id == 42);
  REQUIRE(reply->placement_id.has_value());
  CHECK(*reply->placement_id == 7);
  CHECK(reply->ok());
}

TEST_CASE("Input: high-bit kitty reply status is rejected as non-ASCII",
          "[input][kitty-reply][failure]") {
  Input in;
  std::string bytes{"\033_Gi=42;"};
  bytes.push_back(static_cast<char>(0x80));
  bytes += "\033\\";
  in.feed(bytes);

  CHECK(in.poll().empty());
  const auto replies = in.poll_replies();
  REQUIRE(replies.size() == 1);
  const auto* error = std::get_if<termforge::ErrorEvent>(&replies.front());
  REQUIRE(error != nullptr);
  CHECK(error->message.find("status is not printable ASCII") !=
        std::string::npos);
}

TEST_CASE("Input: a new APC cannot complete a held user Escape",
          "[input][kitty-reply][failure]") {
  Input in;
  in.feed("\033");
  REQUIRE(in.esc_pending());

  in.feed("\033_Gi=42;OK\033\\");
  const auto events = in.poll();
  REQUIRE(events.size() == 1);
  CHECK(std::get<KeyEvent>(events.front()).key == Key::Escape);
  REQUIRE(in.poll_replies().size() == 1);
}

TEST_CASE("Input: malformed kitty replies cannot fabricate keypresses",
          "[input][kitty-reply][failure]") {
  Input in;
  in.feed("\033_Gi=4,i=5;EINVAL\033\\");
  CHECK(in.poll().empty());
  const auto replies = in.poll_replies();
  REQUIRE(replies.size() == 1);
  const auto* error = std::get_if<termforge::ErrorEvent>(&replies.front());
  REQUIRE(error != nullptr);
  CHECK(error->severity == termforge::Severity::Warning);
  CHECK(error->message.find("duplicate image identifier") != std::string::npos);
}

TEST_CASE("Input: oversized kitty replies are bounded and resynchronize",
          "[input][kitty-reply][failure]") {
  Input in;
  std::string oversized{"\033_Gi=9;"};
  oversized.append(5000, 'x');
  in.feed(oversized);
  auto replies = in.poll_replies();
  REQUIRE(replies.size() == 1);
  CHECK(std::holds_alternative<termforge::ErrorEvent>(replies.front()));

  in.feed("\033\\z");
  const auto events = in.poll();
  REQUIRE(events.size() == 1);
  CHECK(std::get<KeyEvent>(events.front()).ch == U'z');
  CHECK(in.poll_replies().empty());
}

TEST_CASE("Input: a split terminator on a discarded APC is never Escape",
          "[input][kitty-reply][failure]") {
  Input in;
  std::string oversized{"\033_Gi=9;"};
  oversized.append(5000, 'x');
  oversized += '\033';
  in.feed(oversized);
  in.flush();
  CHECK(in.poll().empty());
  REQUIRE(in.poll_replies().size() == 1);

  in.feed("\\k");
  const auto events = in.poll();
  REQUIRE(events.size() == 1);
  CHECK(std::get<KeyEvent>(events.front()).ch == U'k');
}

TEST_CASE("Input: a lone ESC decodes as Escape (quit key)",
          "[input][failure]") {
  // Regression: ESC alone previously waited forever for a second byte that
  // never comes, so ESC-to-quit never fired. A trailing lone ESC must decode.
  Input in;
  auto ev = in.decode("\x1B");
  REQUIRE(!ev.empty());
  REQUIRE(first_key(ev).key == Key::Escape);
}

TEST_CASE("Input: escape sequences still decode (not mistaken for lone ESC)",
          "[input]") {
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
  auto ev2 = in2.poll(); // lone ESC flushed as Escape
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
  REQUIRE_FALSE(m->motion);
  REQUIRE(m->action() == MouseAction::Press);
  REQUIRE(m->button == 0);
  REQUIRE(m->x == 9); // 1-based → 0-based
  REQUIRE(m->y == 4);
}

TEST_CASE("Input: SGR mouse release decodes", "[input][mouse]") {
  Input in;
  auto ev = in.decode("\033[<0;10;5m");
  REQUIRE(!ev.empty());
  auto* m = std::get_if<MouseEvent>(&ev.front());
  REQUIRE(m != nullptr);
  REQUIRE_FALSE(m->pressed);
  REQUIRE_FALSE(m->motion);
  REQUIRE(m->action() == MouseAction::Release);
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
  REQUIRE(m->motion);
  REQUIRE(m->action() == MouseAction::Drag);
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
  REQUIRE(m->action() == MouseAction::Wheel);
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
  REQUIRE(m->action() == MouseAction::Wheel);
}

TEST_CASE("Input: press drag and release remain distinct", "[input][mouse]") {
  Input in;
  const auto events = in.decode("\033[<0;5;3M"   // left press
                                "\033[<32;6;3M"  // left drag
                                "\033[<0;6;3m"); // left release
  REQUIRE(events.size() == 3);
  CHECK(std::get<MouseEvent>(events[0]).action() == MouseAction::Press);
  CHECK(std::get<MouseEvent>(events[1]).action() == MouseAction::Drag);
  CHECK(std::get<MouseEvent>(events[2]).action() == MouseAction::Release);
  CHECK(std::get<MouseEvent>(events[0]).pressed);
  CHECK_FALSE(std::get<MouseEvent>(events[1]).pressed);
  CHECK_FALSE(std::get<MouseEvent>(events[2]).pressed);
}

TEST_CASE("MouseEvent keeps the pre-267 aggregate and pressed projection",
          "[input][mouse][compatibility]") {
  const MouseEvent old_press{4, 3, 0, true};
  const MouseEvent old_release{4, 3, 0, false};
  CHECK(old_press.action() == MouseAction::Press);
  CHECK(old_release.action() == MouseAction::Release);
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
  REQUIRE(m->x >= 0); // clamped garbage, but no UB and non-negative
}

TEST_CASE("Input: invalid UTF-8 resynchronizes instead of swallowing keys",
          "[input][security]") {
  // A stray lead byte whose "length" would consume a following real keypress
  // must not eat it. 0xC3 expects one continuation; 'A' is not one.
  Input in;
  auto ev = in.decode("\xC3"
                      "A");
  REQUIRE(ev.size() == 2);
  const auto* bad = std::get_if<KeyEvent>(&ev[0]);
  REQUIRE(bad != nullptr);
  REQUIRE(bad->key == Key::Char);
  REQUIRE(bad->ch == 0xFFFD); // replacement char for the bad lead
  const auto* a = std::get_if<KeyEvent>(&ev[1]);
  REQUIRE(a != nullptr);
  REQUIRE(a->key == Key::Char);
  REQUIRE(a->ch == U'A'); // the following keypress survives
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
    REQUIRE(k->ch == 0xFFFD); // never a raw invalid byte
  }
}

TEST_CASE("Input: incomplete multibyte at end of stream waits then flushes",
          "[input][failure]") {
  // A genuine split glyph across two reads completes normally.
  Input in;
  in.feed("\xC3"); // é lead, continuation not yet arrived
  in.feed("\xA9"); // completes U+00E9
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  REQUIRE(std::get_if<KeyEvent>(&ev.front())->ch == 0xE9);
}

TEST_CASE("Input: valid multibyte still decodes after hardening", "[input]") {
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
  REQUIRE(ev.size() == 1); // exactly one event — no leaked 'H'
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

// ── CSI-tilde function keys F5–F12 — issue #61 ────────────────────────────

TEST_CASE("Input: CSI-tilde F1–F12 decode", "[input][fkeys]") {
  Input in;
  const struct {
    const char* seq;
    Key key;
  } cases[] = {
      {"\033[11~", Key::F1},  {"\033[12~", Key::F2},  {"\033[13~", Key::F3},
      {"\033[14~", Key::F4},  {"\033[15~", Key::F5},  {"\033[17~", Key::F6},
      {"\033[18~", Key::F7},  {"\033[19~", Key::F8},  {"\033[20~", Key::F9},
      {"\033[21~", Key::F10}, {"\033[23~", Key::F11}, {"\033[24~", Key::F12},
  };
  for (const auto& c : cases) {
    auto ev = in.decode(c.seq);
    REQUIRE(ev.size() == 1);
    REQUIRE(first_key(ev).key == c.key);
  }
}

TEST_CASE("Input: the 16 and 22 tilde gaps stay Unknown", "[input][fkeys]") {
  // 16 and 22 are historical holes in the xterm numbering, not typos. If a
  // future edit "completes" the table, F6–F12 all shift by one key.
  Input in;
  auto sixteen = in.decode("\033[16~");
  REQUIRE(first_key(sixteen).key == Key::Unknown);
  auto twentytwo = in.decode("\033[22~");
  REQUIRE(first_key(twentytwo).key == Key::Unknown);
}

TEST_CASE("Input: Ctrl+F5 via ESC[15;5~ sets ctrl", "[input][fkeys][mods]") {
  Input in;
  auto ev = in.decode("\033[15;5~");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::F5);
  REQUIRE(k.ctrl);
  REQUIRE_FALSE(k.alt);
  REQUIRE_FALSE(k.shift);
}

TEST_CASE("Input: Shift+F12 via ESC[24;2~ sets shift", "[input][fkeys][mods]") {
  Input in;
  auto ev = in.decode("\033[24;2~");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::F12);
  REQUIRE(k.shift);
  REQUIRE_FALSE(k.ctrl);
  REQUIRE_FALSE(k.alt);
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
  in.feed("\033[200~hi\033[2"); // terminator begins…
  REQUIRE(in.poll().empty());   // …but isn't complete yet — no event
  in.feed("01~");               // completes ESC[201~
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  auto* p = std::get_if<PasteEvent>(&ev.front());
  REQUIRE(p != nullptr);
  REQUIRE(p->text == "hi");
}

TEST_CASE("Input: an empty paste yields an empty PasteEvent",
          "[input][paste]") {
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
  REQUIRE(ev.empty()); // no spurious key, no crash
}

// ── kitty keyboard protocol (CSI-u) — issue #60 ────────────────────────────
//
// Under KeyboardMode::Disambiguate/Enhanced the terminal reports keys as
//   CSI <key>[:<alt>] ; <mods>[:<event>] [; <text>] u
// and attaches the same event sub-parameter to the keys that keep their
// legacy encoding (arrows, the "~" family). Nothing here depends on the mode
// having been pushed: the parser decodes what arrives.

TEST_CASE("Input: CSI-u named keys decode", "[input][keyboard]") {
  Input in;
  const struct {
    const char* seq;
    Key key;
  } cases[] = {
      {"\033[9u", Key::Tab},
      {"\033[13u", Key::Enter},
      {"\033[27u", Key::Escape},
      {"\033[127u", Key::Backspace},
  };
  for (const auto& c : cases) {
    auto ev = in.decode(c.seq);
    REQUIRE(ev.size() == 1);
    auto k = first_key(ev);
    REQUIRE(k.key == c.key);
    REQUIRE(k.ch == 0); // named keys carry no character
    REQUIRE(k.action == KeyAction::Press);
  }
}

TEST_CASE("Input: CSI-u text keys decode to Key::Char", "[input][keyboard]") {
  Input in;
  auto a = in.decode("\033[97u");
  REQUIRE(a.size() == 1);
  REQUIRE(first_key(a).key == Key::Char);
  REQUIRE(first_key(a).ch == U'a');
  auto sp = in.decode("\033[32u");
  REQUIRE(first_key(sp).ch == U' ');
}

TEST_CASE("Input: the event sub-parameter decodes press/repeat/release",
          "[input][keyboard]") {
  Input in;
  auto press = in.decode("\033[97;1:1u");
  REQUIRE(press.size() == 1);
  REQUIRE(first_key(press).action == KeyAction::Press);
  auto rep = in.decode("\033[97;1:2u");
  REQUIRE(rep.size() == 1);
  REQUIRE(first_key(rep).action == KeyAction::Repeat);
  auto rel = in.decode("\033[97;1:3u");
  REQUIRE(rel.size() == 1);
  REQUIRE(first_key(rel).action == KeyAction::Release);
  // No sub-parameter at all is a press — the whole default-compatibility bet.
  auto bare = in.decode("\033[97u");
  REQUIRE(first_key(bare).action == KeyAction::Press);
}

TEST_CASE("Input: an unrecognized event type degrades to a press",
          "[input][keyboard][failure]") {
  // Inventing a release the user never made is the worse failure mode.
  Input in;
  auto ev = in.decode("\033[97;1:9u");
  REQUIRE(ev.size() == 1);
  REQUIRE(first_key(ev).action == KeyAction::Press);
}

TEST_CASE("Input: Ctrl+I is no longer indistinguishable from Tab",
          "[input][keyboard][mods]") {
  // The acceptance criterion of #60: the ambiguity the protocol exists to fix.
  Input in;
  auto ctrl_i = in.decode("\033[105;5u");
  REQUIRE(ctrl_i.size() == 1);
  auto k = first_key(ctrl_i);
  REQUIRE(k.key == Key::Char);
  REQUIRE(k.ch == U'i');
  REQUIRE(k.ctrl);
  auto tab = in.decode("\t");
  REQUIRE(first_key(tab).key == Key::Tab);
}

TEST_CASE("Input: Ctrl+M is no longer indistinguishable from Enter",
          "[input][keyboard][mods]") {
  Input in;
  auto ctrl_m = in.decode("\033[109;5u");
  auto k = first_key(ctrl_m);
  REQUIRE(k.key == Key::Char);
  REQUIRE(k.ch == U'm');
  REQUIRE(k.ctrl);
  auto enter = in.decode("\r");
  REQUIRE(first_key(enter).key == Key::Enter);
}

TEST_CASE("Input: Ctrl+C still carries ch=='c' when reported as CSI-u",
          "[input][keyboard][regression]") {
  // App's break-glass quit tests (ctrl && ch == 'c'). Kitty omits the
  // associated text while Ctrl is held, so the key code has to carry it.
  Input in;
  auto ev = in.decode("\033[99;5u");
  auto k = first_key(ev);
  REQUIRE(k.ch == U'c');
  REQUIRE(k.ctrl);
}

TEST_CASE("Input: CSI-u modifiers use the same 1+bitmask as xterm",
          "[input][keyboard][mods]") {
  Input in;
  auto shift = in.decode("\033[97;2u");
  REQUIRE(first_key(shift).shift);
  REQUIRE_FALSE(first_key(shift).ctrl);
  auto alt = in.decode("\033[97;3u");
  REQUIRE(first_key(alt).alt);
  auto ctrl = in.decode("\033[97;5u");
  REQUIRE(first_key(ctrl).ctrl);
  auto ctrl_alt = in.decode("\033[97;7u");
  REQUIRE(first_key(ctrl_alt).ctrl);
  REQUIRE(first_key(ctrl_alt).alt);
  // Modifiers and an event type together.
  auto both = in.decode("\033[97;6:3u");
  auto k = first_key(both);
  REQUIRE(k.ctrl);
  REQUIRE(k.shift);
  REQUIRE(k.action == KeyAction::Release);
}

TEST_CASE("Input: associated text wins over the unshifted key code",
          "[input][keyboard]") {
  // Flag 8 reports the *unshifted* key, so Shift+a is (97, shift). Deriving
  // 'A' from that would mean guessing the layout; flag 16's text field is
  // what the terminal actually produced.
  Input in;
  auto ev = in.decode("\033[97;2;65u");
  auto k = first_key(ev);
  REQUIRE(k.key == Key::Char);
  REQUIRE(k.ch == U'A');
  REQUIRE(k.shift);
}

TEST_CASE("Input: an empty modifier field is no modifiers",
          "[input][keyboard]") {
  Input in;
  auto ev = in.decode("\033[97;;97u");
  REQUIRE(ev.size() == 1);
  auto k = first_key(ev);
  REQUIRE(k.ch == U'a');
  REQUIRE_FALSE(k.ctrl);
  REQUIRE_FALSE(k.alt);
  REQUIRE_FALSE(k.shift);
  REQUIRE(k.action == KeyAction::Press);
}

TEST_CASE("Input: alternate-key sub-parameters are discarded, not decoded",
          "[input][keyboard]") {
  // We never request flag 4, but a terminal may send it anyway and it must
  // not corrupt the stream into extra events.
  Input in;
  auto ev = in.decode("\033[97:65;2u");
  REQUIRE(ev.size() == 1);
  auto k = first_key(ev);
  REQUIRE(k.ch == U'a');
  REQUIRE(k.shift);
}

TEST_CASE("Input: legacy-encoded keys carry the event type too",
          "[input][keyboard][regression]") {
  // Kitty keeps arrows and the "~" family on their legacy encodings even
  // under "report all keys as escape codes", attaching the event type as a
  // sub-parameter of the modifiers. Before #60 the scan stopped at the ':'
  // and each of these decoded as *three* events.
  Input in;
  const struct {
    const char* seq;
    Key key;
    KeyAction action;
    bool ctrl;
  } cases[] = {
      {"\033[1;1:3A", Key::Up, KeyAction::Release, false},
      {"\033[1;5:2C", Key::Right, KeyAction::Repeat, true},
      {"\033[3;1:3~", Key::Delete, KeyAction::Release, false},
      {"\033[15;5:3~", Key::F5, KeyAction::Release, true},
  };
  for (const auto& c : cases) {
    auto ev = in.decode(c.seq);
    REQUIRE(ev.size() == 1);
    auto k = first_key(ev);
    REQUIRE(k.key == c.key);
    REQUIRE(k.action == c.action);
    REQUIRE(k.ctrl == c.ctrl);
  }
}

TEST_CASE("Input: bare Shift/Ctrl/Alt emit KeyEvents; locks stay silent",
          "[input][keyboard][regression]") {
  // Enhanced reports LeftShift (57441) as a real key (#209). Super and the
  // locks stay Dropped — Key::Unknown for those would be an Unknown storm.
  Input in;
  auto shift_down = in.decode("\033[57441;1:1u");
  REQUIRE(shift_down.size() == 1);
  REQUIRE(first_key(shift_down).key == Key::LeftShift);
  REQUIRE(first_key(shift_down).action == KeyAction::Press);
  auto shift_up = in.decode("\033[57441;1:3u");
  REQUIRE(first_key(shift_up).key == Key::LeftShift);
  REQUIRE(first_key(shift_up).action == KeyAction::Release);
  auto ctrl = in.decode("\033[57442u");
  REQUIRE(first_key(ctrl).key == Key::LeftCtrl);
  auto alt = in.decode("\033[57443u");
  REQUIRE(first_key(alt).key == Key::LeftAlt);
  auto rshift = in.decode("\033[57447u");
  REQUIRE(first_key(rshift).key == Key::RightShift);
  auto rctrl = in.decode("\033[57448u");
  REQUIRE(first_key(rctrl).key == Key::RightCtrl);
  auto ralt = in.decode("\033[57449u");
  REQUIRE(first_key(ralt).key == Key::RightAlt);
  REQUIRE(in.decode("\033[57444u").empty()); // LeftSuper — still Dropped
  REQUIRE(in.decode("\033[57358u").empty()); // CapsLock
  REQUIRE(in.decode("\033[57428u").empty()); // MediaPlay
}

TEST_CASE("Input: Shift-up arrives before W-up in a boost chord (#209)",
          "[input][keyboard][modifier]") {
  // Acceptance for #209: Shift press → W press → Shift release → W release
  // must expose the Shift-up transition before W-up so sprint can clear while
  // W remains held. Sequences are kitty Enhanced CSI-u (no Legacy synthesis).
  Input in;
  auto seq =
      in.decode("\033[57441;2u"   // LeftShift press (mod bit includes shift)
                "\033[119;2u"     // w press with shift
                "\033[57441;1:3u" // LeftShift release
                "\033[119;1:3u"); // w release
  REQUIRE(seq.size() == 4);
  REQUIRE(std::get<KeyEvent>(seq[0]).key == Key::LeftShift);
  REQUIRE(std::get<KeyEvent>(seq[0]).action == KeyAction::Press);
  REQUIRE(std::get<KeyEvent>(seq[1]).key == Key::Char);
  REQUIRE(std::get<KeyEvent>(seq[1]).ch == U'w');
  REQUIRE(std::get<KeyEvent>(seq[1]).shift);
  REQUIRE(std::get<KeyEvent>(seq[1]).action == KeyAction::Press);
  REQUIRE(std::get<KeyEvent>(seq[2]).key == Key::LeftShift);
  REQUIRE(std::get<KeyEvent>(seq[2]).action == KeyAction::Release);
  REQUIRE(std::get<KeyEvent>(seq[3]).key == Key::Char);
  REQUIRE(std::get<KeyEvent>(seq[3]).ch == U'w');
  REQUIRE(std::get<KeyEvent>(seq[3]).action == KeyAction::Release);
}

TEST_CASE(
    "Input: tapping Shift while W is held yields one modifier interval (#209)",
    "[input][keyboard][modifier]") {
  Input in;
  auto seq = in.decode("\033[119u"       // w press
                       "\033[57441;2u"   // LeftShift press
                       "\033[57441;1:3u" // LeftShift release
                       "\033[119;1:3u"); // w release
  REQUIRE(seq.size() == 4);
  int shift_downs = 0, shift_ups = 0;
  for (const auto& e : seq) {
    const auto* k = std::get_if<KeyEvent>(&e);
    REQUIRE(k != nullptr);
    if (k->key != Key::LeftShift) continue;
    if (k->action == KeyAction::Press) ++shift_downs;
    if (k->action == KeyAction::Release) ++shift_ups;
  }
  REQUIRE(shift_downs == 1);
  REQUIRE(shift_ups == 1);
}

TEST_CASE("Input: a real key with no Key enumerator stays Unknown",
          "[input][keyboard]") {
  // Insert and F13+ are keys TermForge cannot name — same disposition as
  // ESC[2~ has always had, and deliberately *not* the silent drop above.
  Input in;
  auto ins = in.decode("\033[57348u");
  REQUIRE(ins.size() == 1);
  REQUIRE(first_key(ins).key == Key::Unknown);
  auto f13 = in.decode("\033[57376u");
  REQUIRE(f13.size() == 1);
  REQUIRE(first_key(f13).key == Key::Unknown);
}

TEST_CASE("Input: keypad keys resolve to the key the user pressed",
          "[input][keyboard]") {
  Input in;
  auto zero = in.decode("\033[57399u");
  REQUIRE(first_key(zero).key == Key::Char);
  REQUIRE(first_key(zero).ch == U'0');
  auto seven = in.decode("\033[57406u");
  REQUIRE(first_key(seven).ch == U'7');
  auto plus = in.decode("\033[57413u");
  REQUIRE(first_key(plus).ch == U'+');
  auto kp_enter = in.decode("\033[57414u");
  REQUIRE(first_key(kp_enter).key == Key::Enter);
  auto kp_up = in.decode("\033[57419u");
  REQUIRE(first_key(kp_up).key == Key::Up);
  auto kp_del = in.decode("\033[57426u");
  REQUIRE(first_key(kp_del).key == Key::Delete);
}

TEST_CASE("Input: a CSI-u report split across feeds decodes once",
          "[input][keyboard][regression]") {
  // A 256-byte read can split anywhere, including inside a sub-parameter.
  Input in;
  in.feed("\033[97;2");
  REQUIRE(in.poll().empty()); // no final byte yet
  in.feed(":3u");
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  auto k = first_key(ev);
  REQUIRE(k.ch == U'a');
  REQUIRE(k.shift);
  REQUIRE(k.action == KeyAction::Release);
}

TEST_CASE(
    "Input: a hostile CSI-u parameter run is bounded and yields one event",
    "[input][keyboard][security]") {
  Input in;
  auto ev = in.decode("\033[1:2:3:4:5;6;7;8;9u");
  REQUIRE(ev.size() <= 1); // key code 1 is a control code: dropped
  auto big = in.decode("\033[99999999999999999999;2u");
  REQUIRE(big.size() <= 1); // clamped garbage, no UB
}

TEST_CASE("Input: an unencodable associated text falls back to the key code",
          "[input][keyboard][security]") {
  // Every other route to KeyEvent::ch goes through the UTF-8 decoder, which
  // validates. CSI-u is the one path where a code point arrives as a decimal
  // parameter straight off the wire, so a surrogate or an out-of-range value
  // must not reach an app that will try to encode it.
  Input in;
  auto surrogate = in.decode("\033[97;1;55296u"); // U+D800, a lone surrogate
  REQUIRE(surrogate.size() == 1);
  REQUIRE(first_key(surrogate).ch == U'a');       // the vetted key code instead
  auto too_big = in.decode("\033[97;1;1114112u"); // one past U+10FFFF
  REQUIRE(too_big.size() == 1);
  REQUIRE(first_key(too_big).ch == U'a');
  // A legitimate astral code point still survives the parameter cap.
  auto emoji = in.decode("\033[97;1;128512u"); // U+1F600
  REQUIRE(first_key(emoji).ch == U'\U0001F600');
}

// ── ground truth: bytes captured from kitty via `show_key -m kitty` ────────
//
// AGENTS.md: "Pin the Capabilities schema against real responses before it
// becomes load-bearing." The same applies to the key table. Every sequence
// below is copied verbatim from a real capture, not derived from the spec.

TEST_CASE("Input: captured kitty CSI-u sequences decode as observed",
          "[input][keyboard][ground-truth]") {
  Input in;
  // Shift+a: kitty reports the UNSHIFTED key (97) with the shifted key as a
  // sub-parameter we discard, and the produced text in the third parameter.
  auto shift_a = in.decode("\033[97:65;2;65u");
  REQUIRE(shift_a.size() == 1);
  REQUIRE(first_key(shift_a).ch == U'A');
  REQUIRE(first_key(shift_a).shift);
  auto a_rel = in.decode("\033[97;1:3u");
  REQUIRE(first_key(a_rel).ch == U'a');
  REQUIRE(first_key(a_rel).action == KeyAction::Release);
  // The ambiguities the protocol exists to resolve.
  auto ctrl_i = in.decode("\033[105;5u");
  REQUIRE(first_key(ctrl_i).ch == U'i');
  REQUIRE(first_key(ctrl_i).ctrl);
  auto ctrl_i_rel = in.decode("\033[105;5:3u");
  REQUIRE(first_key(ctrl_i_rel).action == KeyAction::Release);
  auto ctrl_m = in.decode("\033[109;5u");
  REQUIRE(first_key(ctrl_m).ch == U'm');
  REQUIRE(first_key(ctrl_m).ctrl);
  // Named keys, press and release.
  const struct {
    const char* seq;
    Key key;
    KeyAction action;
  } named[] = {
      {"\033[9u", Key::Tab, KeyAction::Press},
      {"\033[9;1:3u", Key::Tab, KeyAction::Release},
      {"\033[13u", Key::Enter, KeyAction::Press},
      {"\033[13;1:3u", Key::Enter, KeyAction::Release},
      {"\033[27u", Key::Escape, KeyAction::Press},
      {"\033[27;1:3u", Key::Escape, KeyAction::Release},
      {"\033[127u", Key::Backspace, KeyAction::Press},
      {"\033[127;1:3u", Key::Backspace, KeyAction::Release},
  };
  for (const auto& c : named) {
    auto ev = in.decode(c.seq);
    REQUIRE(ev.size() == 1);
    REQUIRE(first_key(ev).key == c.key);
    REQUIRE(first_key(ev).action == c.action);
  }
}

TEST_CASE("Input: captured legacy-encoded keys keep their encodings",
          "[input][keyboard][ground-truth]") {
  // The capture confirms the design assumption the whole sub-parameter scan
  // rests on: under "report all keys as escape codes" kitty still sends
  // arrows, Home, Delete, Insert and the F-keys in their legacy forms, and
  // attaches the event type as a sub-parameter of the modifiers.
  Input in;
  const struct {
    const char* seq;
    Key key;
    KeyAction action;
  } cases[] = {
      {"\033[A", Key::Up, KeyAction::Press},
      {"\033[1;1:3A", Key::Up, KeyAction::Release},
      {"\033[H", Key::Home, KeyAction::Press},
      {"\033[1;1:3H", Key::Home, KeyAction::Release},
      {"\033[3~", Key::Delete, KeyAction::Press},
      {"\033[3;1:3~", Key::Delete, KeyAction::Release},
      {"\033[2~", Key::Unknown, KeyAction::Press}, // Insert: unnameable
      {"\033[2;1:3~", Key::Unknown, KeyAction::Release},
      {"\033[P", Key::F1, KeyAction::Press},
      {"\033[1;1:3P", Key::F1, KeyAction::Release},
      {"\033[15~", Key::F5, KeyAction::Press},
      {"\033[15;1:3~", Key::F5, KeyAction::Release},
  };
  for (const auto& c : cases) {
    auto ev = in.decode(c.seq);
    REQUIRE(ev.size() == 1);
    REQUIRE(first_key(ev).key == c.key);
    REQUIRE(first_key(ev).action == c.action);
  }
}

TEST_CASE("Input: lock bits in the modifier mask are not modifiers",
          "[input][keyboard][ground-truth][regression]") {
  // The capture's sharpest edge: with NumLock on -- a very common state --
  // EVERY keystroke carries modifier parameter 129 (1 + 128), and CapsLock
  // pushes it to 193 (1 + 128 + 64). Kitty's bitmask extends well past the
  // three modifiers KeyEvent models, so anything that widened the mask
  // naively would hand a NumLock user phantom Ctrl/Alt/Shift on every key.
  Input in;
  auto numlock_enter = in.decode("\033[13;129u");
  REQUIRE(first_key(numlock_enter).key == Key::Enter);
  REQUIRE_FALSE(first_key(numlock_enter).ctrl);
  REQUIRE_FALSE(first_key(numlock_enter).alt);
  REQUIRE_FALSE(first_key(numlock_enter).shift);
  // CapsLock is likewise not Shift: it changes the produced text, and the
  // text parameter is what carries that.
  auto caps_num = in.decode("\033[13;193u");
  REQUIRE_FALSE(first_key(caps_num).shift);
  // …but a real Shift alongside the lock bits still reads as Shift (130 =
  // 1 + 128 + 1).
  auto shift_num = in.decode("\033[13;130u");
  REQUIRE(first_key(shift_num).shift);
  REQUIRE_FALSE(first_key(shift_num).ctrl);
  // Keypad 7 with NumLock on, exactly as captured: text 55 is the '7'.
  auto kp7 = in.decode("\033[57406;129;55u");
  REQUIRE(kp7.size() == 1);
  REQUIRE(first_key(kp7).key == Key::Char);
  REQUIRE(first_key(kp7).ch == U'7');
  REQUIRE_FALSE(first_key(kp7).shift);
  auto kp7_rel = in.decode("\033[57406;129:3u");
  REQUIRE(first_key(kp7_rel).action == KeyAction::Release);
}

TEST_CASE("Input: captured Shift/Ctrl emit KeyEvents; locks stay silent",
          "[input][keyboard][ground-truth]") {
  // Verbatim from the capture. Shift/Ctrl are named keys (#209); locks stay
  // Dropped — Key::Unknown for them would be an Unknown storm.
  Input in;
  auto shift_press = in.decode("\033[57441;2u");
  REQUIRE(first_key(shift_press).key == Key::LeftShift);
  REQUIRE(first_key(shift_press).action == KeyAction::Press);
  REQUIRE(first_key(shift_press).shift);
  auto shift_rel = in.decode("\033[57441;1:3u");
  REQUIRE(first_key(shift_rel).key == Key::LeftShift);
  REQUIRE(first_key(shift_rel).action == KeyAction::Release);
  auto shift_num = in.decode("\033[57441;130u");
  REQUIRE(first_key(shift_num).key == Key::LeftShift);
  REQUIRE(first_key(shift_num).shift);
  auto ctrl_press = in.decode("\033[57442;5u");
  REQUIRE(first_key(ctrl_press).key == Key::LeftCtrl);
  REQUIRE(first_key(ctrl_press).ctrl);
  auto ctrl_rel = in.decode("\033[57442;1:3u");
  REQUIRE(first_key(ctrl_rel).key == Key::LeftCtrl);
  REQUIRE(first_key(ctrl_rel).action == KeyAction::Release);
  for (const char* seq : {"\033[57360u",          // NUM_LOCK press
                          "\033[57360;129:3u",    // NUM_LOCK release
                          "\033[57358;129u",      // CAPS_LOCK press
                          "\033[57358;193:3u"}) { // CAPS_LOCK release
    REQUIRE(in.decode(seq).empty());
  }
}
