#pragma once

// TermForge — FocusRing: owns the Tab-order and is the keyboard-focus gatekeeper.
//
// A GUI needs exactly one owner of "who has focus"; without it every app
// hand-rolls Tab-cycling and widgets disagree about whether to self-guard on
// focus. FocusRing is that owner. It holds an ordered, non-owning list of
// focusable widgets (the app still owns the widgets themselves — the ring keeps
// Widget* the way App::route_mouse takes Widget*) and:
//
//   * routes keyboard events ONLY to the focused member — so a widget acts on
//     any key it is *given*, and broadcasting keys to every widget stops being
//     the model (this is the uniform convention documented in widget.hpp);
//   * cycles focus on Tab / Shift+Tab, skipping members that decline focus
//     (Widget::focusable() == false);
//   * moves focus to a member the user clicks (focus_at, called from the app's
//     mouse handling alongside App::route_mouse).
//
// Typical use (see examples/widgets.cpp):
//   ring.add(&menu); ring.add(&input); ring.add(&ok); ring.add(&list);
//   // in on_event:
//   if (auto* m = std::get_if<MouseEvent>(&ev)) {
//     if (m->pressed) ring.focus_at(m->x, m->y);
//     route_mouse(*m, {&menu, &input, &ok, &list});
//     return;
//   }
//   if (ring.handle_key(ev)) return;   // focused widget + Tab-cycling
//   // ...app-level keys (quit, etc.)

#include <cstddef>
#include <vector>

#include "termforge/core/types.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class FocusRing {
 public:
  // Append a widget to the tab order. nullptr is ignored. The first focusable
  // member added receives focus, so a freshly-built ring has a sensible cursor.
  auto add(Widget* w) -> void;

  // Drop all members; no widget is focused afterwards.
  auto clear() -> void;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return m_members.size();
  }

  // The focused member, or nullptr if the ring is empty / nothing focusable.
  [[nodiscard]] auto current() const -> Widget*;

  // Focus a specific member. Returns false if it is not in the ring or is not
  // currently focusable().
  auto focus(Widget* w) -> bool;

  // Move focus to the next / previous focusable member, wrapping around. No-op
  // if the ring has no focusable member.
  auto focus_next() -> void;
  auto focus_prev() -> void;

  // Deliver a keyboard event to the focused member; if it does not consume it
  // and the event is Tab / Shift+Tab, cycle focus. Returns true if the ring
  // handled the event (member consumed it, or focus cycled). Mouse events are
  // ignored here (return false) — route those via focus_at + App::route_mouse.
  auto handle_key(const Event& ev) -> bool;

  // If (x, y) lands on a focusable member (topmost-first, i.e. last added wins
  // like App::route_mouse), focus it and return it; otherwise return nullptr
  // and leave focus unchanged. Call this on a mouse press before route_mouse.
  auto focus_at(int x, int y) -> Widget*;

 private:
  // Apply set_focused(true) to member idx and set_focused(false) to the rest.
  auto set_index(int idx) -> void;
  // Step +1/-1 through the ring, skipping !focusable(), wrapping.
  auto advance(int dir) -> void;

  std::vector<Widget*> m_members;
  int m_focused{-1};
};

}  // namespace termforge
