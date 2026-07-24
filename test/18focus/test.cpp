// FocusRing: Tab-order ownership, key gatekeeping, and click-to-focus.
//
// The ring routes keys only to the focused member, cycles on Tab/Shift+Tab
// (skipping !focusable() members), and moves focus to a clicked member. These
// tests drive it the way the App does — feeding Events through handle_key /
// focus_at and asserting focus movement + set_focused() propagation.

#include <catch2/catch_test_macros.hpp>

#include <variant>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/widget.hpp"

using termforge::Event;
using termforge::FocusRing;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;
using termforge::Rect;
using termforge::Screen;
using termforge::Widget;

namespace {

// A minimal focusable widget: counts the keys it is given and can be told to
// consume them or to decline focus.
class Probe final : public Widget {
 public:
  Probe() = default;
  explicit Probe(bool focusable) : m_focusable(focusable) {}

  auto draw(Screen& /*screen*/) -> void override {}
  auto on_event(const Event& ev) -> bool override {
    if (std::holds_alternative<KeyEvent>(ev)) {
      ++m_keys;
      return m_consume;
    }
    return false;
  }
  [[nodiscard]] auto focusable() const -> bool override { return m_focusable; }

  auto set_consume(bool c) -> void { m_consume = c; }
  auto set_focusable(bool f) -> void { m_focusable = f; }
  [[nodiscard]] auto keys() const -> int { return m_keys; }

 private:
  bool m_focusable{true};
  bool m_consume{false};
  int m_keys{0};
};

auto key(Key k, bool shift = false) -> Event {
  KeyEvent e;
  e.key = k;
  e.shift = shift;
  return Event{e};
}
auto press(int x, int y) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.pressed = true;
  return Event{e};
}

}  // namespace

TEST_CASE("FocusRing: empty ring is inert", "[focus][failure]") {
  FocusRing ring;
  REQUIRE(ring.size() == 0);
  REQUIRE(ring.current() == nullptr);
  REQUIRE_FALSE(ring.handle_key(key(Key::Tab)));  // no members: Tab not owned
  REQUIRE(ring.focus_at(0, 0) == nullptr);
  ring.focus_next();  // must not crash
  ring.focus_prev();
  REQUIRE(ring.current() == nullptr);
}

TEST_CASE("FocusRing: add(nullptr) is ignored", "[focus][failure]") {
  FocusRing ring;
  ring.add(nullptr);
  REQUIRE(ring.size() == 0);
  REQUIRE(ring.current() == nullptr);
}

TEST_CASE("FocusRing: first focusable member added gets focus", "[focus]") {
  FocusRing ring;
  Probe a, b;
  ring.add(&a);
  ring.add(&b);
  REQUIRE(ring.size() == 2);
  REQUIRE(ring.current() == &a);
  REQUIRE(a.focused());
  REQUIRE_FALSE(b.focused());
}

TEST_CASE("FocusRing: a non-focusable leading member is skipped for initial focus",
          "[focus]") {
  FocusRing ring;
  Probe a{false};  // declines focus
  Probe b;
  ring.add(&a);
  ring.add(&b);
  REQUIRE(ring.current() == &b);
  REQUIRE_FALSE(a.focused());
  REQUIRE(b.focused());
}

TEST_CASE("FocusRing: focus_next / focus_prev cycle and wrap", "[focus]") {
  FocusRing ring;
  Probe a, b, c;
  ring.add(&a);
  ring.add(&b);
  ring.add(&c);
  REQUIRE(ring.current() == &a);

  ring.focus_next();
  REQUIRE(ring.current() == &b);
  REQUIRE(b.focused());
  REQUIRE_FALSE(a.focused());

  ring.focus_next();
  REQUIRE(ring.current() == &c);
  ring.focus_next();  // wraps
  REQUIRE(ring.current() == &a);

  ring.focus_prev();  // wraps back
  REQUIRE(ring.current() == &c);
  REQUIRE(c.focused());
  REQUIRE_FALSE(a.focused());
}

TEST_CASE("FocusRing: exactly one member is focused at a time", "[focus]") {
  FocusRing ring;
  Probe a, b, c;
  ring.add(&a);
  ring.add(&b);
  ring.add(&c);
  ring.focus_next();  // -> b
  REQUIRE(a.focused() == false);
  REQUIRE(b.focused() == true);
  REQUIRE(c.focused() == false);
}

TEST_CASE("FocusRing: focus_next skips non-focusable members", "[focus]") {
  FocusRing ring;
  Probe a, b{false}, c;
  ring.add(&a);
  ring.add(&b);
  ring.add(&c);
  REQUIRE(ring.current() == &a);
  ring.focus_next();  // b declines -> lands on c
  REQUIRE(ring.current() == &c);
  ring.focus_next();  // wraps to a
  REQUIRE(ring.current() == &a);
}

TEST_CASE("FocusRing: all-non-focusable ring never focuses and does not hang",
          "[focus][failure]") {
  FocusRing ring;
  Probe a{false}, b{false};
  ring.add(&a);
  ring.add(&b);
  REQUIRE(ring.current() == nullptr);
  ring.focus_next();  // no candidate — must return
  REQUIRE(ring.current() == nullptr);
  // Members exist, so the ring owns Tab even with nothing to focus.
  REQUIRE(ring.handle_key(key(Key::Tab)));
  REQUIRE(ring.current() == nullptr);
}

TEST_CASE("FocusRing: handle_key routes only to the focused member", "[focus]") {
  FocusRing ring;
  Probe a, b;
  ring.add(&a);
  ring.add(&b);  // a focused
  ring.handle_key(key(Key::Down));
  REQUIRE(a.keys() == 1);
  REQUIRE(b.keys() == 0);

  ring.focus_next();  // -> b
  ring.handle_key(key(Key::Down));
  REQUIRE(a.keys() == 1);
  REQUIRE(b.keys() == 1);
}

TEST_CASE("FocusRing: Tab / Shift+Tab cycle only when the member doesn't consume",
          "[focus]") {
  FocusRing ring;
  Probe a, b, c;
  ring.add(&a);
  ring.add(&b);
  ring.add(&c);

  REQUIRE(ring.handle_key(key(Key::Tab)));  // a doesn't consume -> cycle
  REQUIRE(ring.current() == &b);
  REQUIRE(ring.handle_key(key(Key::Tab, /*shift=*/true)));  // -> a
  REQUIRE(ring.current() == &a);

  // A member that consumes Tab prevents the ring from cycling. The focused
  // member is always given the key first, so `a` has now seen Tab twice (the
  // initial cycle attempt above + this consumed one).
  a.set_consume(true);
  REQUIRE(ring.handle_key(key(Key::Tab)));  // consumed by a
  REQUIRE(ring.current() == &a);            // focus unchanged
  REQUIRE(a.keys() == 2);
}

TEST_CASE("FocusRing: handle_key ignores mouse events", "[focus][failure]") {
  FocusRing ring;
  Probe a;
  ring.add(&a);
  REQUIRE_FALSE(ring.handle_key(press(0, 0)));
  REQUIRE(a.keys() == 0);  // mouse must not reach on_event via handle_key
}

TEST_CASE("FocusRing: single member consumes Tab but stays focused", "[focus]") {
  FocusRing ring;
  Probe a;
  ring.add(&a);
  REQUIRE(ring.handle_key(key(Key::Tab)));
  REQUIRE(ring.current() == &a);
}

TEST_CASE("FocusRing: focus(widget) targets a specific member", "[focus]") {
  FocusRing ring;
  Probe a, b, other;
  ring.add(&a);
  ring.add(&b);
  REQUIRE(ring.focus(&b));
  REQUIRE(ring.current() == &b);
  REQUIRE_FALSE(ring.focus(&other));    // not in the ring
  REQUIRE(ring.current() == &b);        // unchanged
}

TEST_CASE("FocusRing: focus() refuses a non-focusable member", "[focus][failure]") {
  FocusRing ring;
  Probe a, b{false};
  ring.add(&a);
  ring.add(&b);
  REQUIRE_FALSE(ring.focus(&b));
  REQUIRE(ring.current() == &a);
}

TEST_CASE("FocusRing: focus_at moves focus to the clicked member", "[focus][mouse]") {
  FocusRing ring;
  Probe a, b;
  a.set_geometry(Rect{0, 0, 10, 1});
  b.set_geometry(Rect{0, 2, 10, 1});
  ring.add(&a);
  ring.add(&b);  // a focused initially

  REQUIRE(ring.focus_at(3, 2) == &b);
  REQUIRE(ring.current() == &b);
  REQUIRE(b.focused());
  REQUIRE_FALSE(a.focused());

  // A click on empty space leaves focus unchanged.
  REQUIRE(ring.focus_at(3, 5) == nullptr);
  REQUIRE(ring.current() == &b);
}

TEST_CASE("FocusRing: focus_at picks the topmost (last-added) overlapping member",
          "[focus][mouse]") {
  FocusRing ring;
  Probe under, over;
  under.set_geometry(Rect{0, 0, 10, 3});
  over.set_geometry(Rect{0, 0, 10, 3});  // fully overlaps
  ring.add(&under);
  ring.add(&over);
  REQUIRE(ring.focus_at(2, 1) == &over);  // last added wins
  REQUIRE(over.focused());
  REQUIRE_FALSE(under.focused());
}

TEST_CASE("FocusRing: focus_at skips a non-focusable member on top", "[focus][mouse]") {
  FocusRing ring;
  Probe under, over{false};
  under.set_geometry(Rect{0, 0, 10, 3});
  over.set_geometry(Rect{0, 0, 10, 3});
  ring.add(&under);
  ring.add(&over);  // declines focus
  REQUIRE(ring.focus_at(2, 1) == &under);
  REQUIRE(under.focused());
}

TEST_CASE("FocusRing: clear() empties the ring and drops focus", "[focus]") {
  FocusRing ring;
  Probe a, b;
  ring.add(&a);
  ring.add(&b);
  ring.clear();
  REQUIRE(ring.size() == 0);
  REQUIRE(ring.current() == nullptr);
  REQUIRE_FALSE(ring.handle_key(key(Key::Tab)));
}
