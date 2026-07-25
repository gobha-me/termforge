#include "termforge/widgets/select.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/width.hpp"

namespace termforge {

Select::Select(std::vector<std::string> options) {
  set_options(std::move(options));
}

auto Select::set_options(std::vector<std::string> options) -> void {
  m_options = std::move(options);
  m_selected = m_options.empty() ? -1 : 0;
  close_dropdown();
  mark_dirty();
}

auto Select::add_option(std::string option) -> void {
  m_options.push_back(std::move(option));
  if (m_selected < 0) m_selected = 0;
  mark_dirty();
}

auto Select::clear() -> void {
  m_options.clear();
  m_selected = -1;
  close_dropdown();
  mark_dirty();
}

auto Select::selected_text() const -> std::string {
  if (m_selected < 0 || m_selected >= static_cast<int>(m_options.size()))
    return {};
  return m_options[static_cast<std::size_t>(m_selected)];
}

auto Select::set_selected(int index) -> void {
  if (m_options.empty()) {
    m_selected = -1;
  } else {
    m_selected = std::clamp(index, 0, static_cast<int>(m_options.size()) - 1);
  }
  // Match set_options()/clear(): a programmatic set re-seeds the control, so
  // an open dropdown must not survive with a stale m_highlight that the next
  // Enter would commit over the value the app just set (#36).
  close_dropdown();
  mark_dirty();
}

auto Select::dropdown_rect() const -> Rect {
  if (!m_open || m_options.empty()) return {0, 0, 0, 0};
  const Rect r = rect();
  // Exactly as wide as the control (see the header note), one row per option,
  // starting BELOW the whole rect -- not r.y + 1, which overlaps the box line
  // draw() centers at r.y + r.h/2 for any h >= 2 (#36 item 1).
  return {r.x, r.y + r.h, r.w, static_cast<int>(m_options.size())};
}

auto Select::hit_test(int px, int py) const -> bool {
  return rect().contains(px, py) || dropdown_rect().contains(px, py);
}

auto Select::open_dropdown() -> void {
  if (m_open || m_options.empty()) return;
  m_open = true;
  m_highlight = std::max(0, m_selected);
  mark_dirty();
}

auto Select::close_dropdown() -> void {
  if (!m_open) return;
  m_open = false;
  m_highlight = -1;
  mark_dirty();
}

auto Select::set_focused(bool focused) -> void {
  Widget::set_focused(focused);
  // Focus loss closes the list. This covers Tab-out, FocusRing::focus(), and
  // — the useful one — a click on any other ring member, because focus_at
  // moves focus and therefore calls this. See the header note.
  if (!focused) close_dropdown();
}

auto Select::commit(int index) -> void {
  if (index < 0 || index >= static_cast<int>(m_options.size())) return;
  // Copy BOTH before closing and firing (#5, #32): the option, because the
  // callback may call set_options() and reallocate the vector behind a
  // reference into our own storage; and the callback, because it may call
  // on_change() and destroy the std::function it is running inside.
  const std::string item = m_options[static_cast<std::size_t>(index)];
  auto cb = m_on_change;
  const bool changed = (index != m_selected);
  m_selected = index;
  close_dropdown();
  mark_dirty();
  // No-change commits stay silent -- the no-op-silence rule RadioGroup::select
  // and Checkbox::set_checked already follow (#36 item 3). Re-committing the
  // current value still closes the list, but fires nothing.
  if (changed && cb) cb(index, item);
}

auto Select::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  const MarkGlyphs g = mark_glyphs(m_style);

  Rgb fg = m_fg, bg = m_bg;
  if (focused()) {
    fg = m_focused_fg;
    bg = m_focused_bg;
  }

  // Own the whole rect (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);

  // "[ value…      ▾ ]" — padded so the closing bracket sits on the last
  // column, then truncated once so a narrow rect degrades by one rule.
  const int inner = std::max(0, r.w - kChromeCols);
  // Hold the value in a named local: truncate_to_width returns a view, and
  // selected_text() returns by value, so viewing the temporary directly would
  // dangle at the end of the full-expression.
  const std::string current = selected_text();
  const std::string_view value = detail::truncate_to_width(current, inner);
  std::string line;
  line += g.check_open;
  line += ' ';
  line += value;
  const int pad = inner - detail::display_width(value);
  line.append(static_cast<std::size_t>(std::max(0, pad)), ' ');
  line += ' ';
  line += g.arrow_down;
  line += ' ';
  line += g.check_close;

  const int y = r.y + r.h / 2;
  screen.write_text(r.x, y, detail::truncate_to_width(line, r.w), fg, bg);

  // The dropdown draws BELOW rect() — the documented exception, matched by
  // hit_test(). Geometry comes from dropdown_rect() so the two cannot disagree.
  if (const Rect dr = dropdown_rect(); dr.w > 0 && dr.h > 0) {
    for (int vi = 0; vi < dr.h; ++vi) {
      const int dy = dr.y + vi;
      const bool is_hl = (vi == m_highlight);
      const Rgb rfg = is_hl ? m_highlight_fg : m_dropdown_fg;
      const Rgb rbg = is_hl ? m_highlight_bg : m_dropdown_bg;

      screen.fill_rect(dr.x, dy, dr.w, 1, rfg, rbg);
      const int avail = std::max(0, dr.w - 2);
      screen.write_text(
          dr.x + 1, dy,
          detail::truncate_to_width(m_options[static_cast<std::size_t>(vi)],
                                    avail),
          rfg, rbg);
    }
  }

  clear_dirty();
}

auto Select::handle_mouse(const MouseEvent& m) -> bool {
  const Rect dr = dropdown_rect();

  // Wheel FIRST. A wheel report arrives with pressed == false
  // (input.cpp:221-225), so checking it after the hover branch below would
  // make this unreachable and let a scroll drag the highlight around.
  // Ignored like RadioGroup's — a stray scroll must not change a form value —
  // but consumed while the list is open so it cannot reach the widget behind.
  if (m.scroll_up || m.scroll_down) return m_open && hit_test(m.x, m.y);

  // Hover over the open list moves the highlight (MenuBar's behavior).
  if (!m.pressed) {
    if (m_open && dr.contains(m.x, m.y)) {
      const int vi = m.y - dr.y;
      if (vi != m_highlight && vi >= 0 && vi < dr.h) {
        m_highlight = vi;
        mark_dirty();
      }
      return true;
    }
    return false;
  }

  if (m.button != 0) {
    // Non-left press: while open, consume inside our area so it cannot leak
    // to the widget underneath the dropdown. While CLOSED the leak rationale
    // does not apply and every sibling declines (button.cpp, checkbox.cpp,
    // radio_group.cpp), so an app-level right-click handler works over a
    // closed Select too (#36 item 4).
    return m_open && hit_test(m.x, m.y);
  }

  if (rect().contains(m.x, m.y)) {
    if (m_open)
      close_dropdown();
    else
      open_dropdown();
    return true;
  }

  if (m_open && dr.contains(m.x, m.y)) {
    commit(m.y - dr.y);
    return true;
  }

  return false;
}

auto Select::on_event(const Event& ev) -> bool {
  if (const auto* m = std::get_if<MouseEvent>(&ev)) return handle_mouse(*m);

  const auto* k = std::get_if<KeyEvent>(&ev);
  if (k == nullptr) return false;

  if (!m_open) {
    if (k->key == Key::Enter || k->key == Key::Down ||
        (k->key == Key::Char && k->ch == U' ')) {
      open_dropdown();
      return true;
    }
    // Everything else declined while closed — Escape in particular, so a
    // Select inside a Dialog does not eat the dialog's cancel, and Tab so the
    // FocusRing cycles.
    return false;
  }

  // ── open ──
  if (k->key == Key::Tab) {
    // Close and DECLINE, so FocusRing::handle_key cycles on the same press.
    // See the divergence from MenuBar in the header.
    close_dropdown();
    return false;
  }
  if (k->key == Key::Escape) {
    close_dropdown();  // no commit
    return true;
  }
  if (k->key == Key::Enter) {
    commit(m_highlight);
    return true;
  }
  const int last = static_cast<int>(m_options.size()) - 1;
  if (k->key == Key::Up) {
    m_highlight = std::max(0, m_highlight - 1);
    mark_dirty();
    return true;
  }
  if (k->key == Key::Down) {
    m_highlight = std::min(last, m_highlight + 1);
    mark_dirty();
    return true;
  }
  if (k->key == Key::Home) {
    m_highlight = 0;
    mark_dirty();
    return true;
  }
  if (k->key == Key::End) {
    m_highlight = last;
    mark_dirty();
    return true;
  }

  // The open list is a mini-modal: consume everything else so a stray key
  // cannot reach the widget behind it.
  return true;
}

}  // namespace termforge
