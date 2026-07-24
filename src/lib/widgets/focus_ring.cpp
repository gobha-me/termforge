#include "termforge/widgets/focus_ring.hpp"

#include <variant>

namespace termforge {

auto FocusRing::add(Widget* w) -> void {
  if (w == nullptr) return;
  m_members.push_back(w);
  if (m_focused < 0 && w->focusable())
    set_index(static_cast<int>(m_members.size()) - 1);
}

auto FocusRing::clear() -> void {
  m_members.clear();
  m_focused = -1;
}

auto FocusRing::current() const -> Widget* {
  if (m_focused < 0 || m_focused >= static_cast<int>(m_members.size()))
    return nullptr;
  return m_members[static_cast<std::size_t>(m_focused)];
}

auto FocusRing::focus(Widget* w) -> bool {
  for (int i = 0; i < static_cast<int>(m_members.size()); ++i) {
    if (m_members[static_cast<std::size_t>(i)] == w) {
      if (!w->focusable()) return false;
      set_index(i);
      return true;
    }
  }
  return false;
}

auto FocusRing::focus_next() -> void { advance(+1); }
auto FocusRing::focus_prev() -> void { advance(-1); }

auto FocusRing::handle_key(const Event& ev) -> bool {
  // Mouse is routed separately (focus_at + App::route_mouse); ignore it here so
  // an app can pass every event through without double-routing clicks.
  if (std::holds_alternative<MouseEvent>(ev)) return false;

  if (Widget* cur = current(); cur != nullptr && cur->on_event(ev)) return true;

  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Tab && !m_members.empty()) {
      if (k->shift)
        focus_prev();
      else
        focus_next();
      return true;
    }
  }
  return false;
}

auto FocusRing::focus_at(int x, int y) -> Widget* {
  // Topmost-first: last added is drawn on top, so it wins a hit — mirrors
  // App::route_mouse's reverse iteration.
  for (int i = static_cast<int>(m_members.size()) - 1; i >= 0; --i) {
    Widget* w = m_members[static_cast<std::size_t>(i)];
    if (w->focusable() && w->hit_test(x, y)) {
      set_index(i);
      return w;
    }
  }
  return nullptr;
}

auto FocusRing::set_index(int idx) -> void {
  if (idx == m_focused) return;
  for (int i = 0; i < static_cast<int>(m_members.size()); ++i)
    m_members[static_cast<std::size_t>(i)]->set_focused(i == idx);
  m_focused = idx;
}

auto FocusRing::advance(int dir) -> void {
  const int n = static_cast<int>(m_members.size());
  if (n == 0) return;
  // Start just outside the current position so the first candidate is the
  // adjacent member; when nothing is focused yet, next starts at 0 and prev at
  // n-1.
  const int start = (m_focused < 0) ? (dir > 0 ? -1 : 0) : m_focused;
  for (int step = 1; step <= n; ++step) {
    const int idx = (((start + dir * step) % n) + n) % n;
    if (m_members[static_cast<std::size_t>(idx)]->focusable()) {
      set_index(idx);
      return;
    }
  }
  // No focusable member — leave focus unchanged.
}

}  // namespace termforge
