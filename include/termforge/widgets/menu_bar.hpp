#pragma once

// TermForge — MenuBar: horizontal menu navigation with dropdowns.
//
// A horizontal bar of menu titles. Left/Right navigates between menus,
// Enter/Down opens the dropdown (rendered as an inline list), Escape
// closes it. Each dropdown item fires a callback when selected.
//
// The dropdown renders below the bar, overlaying whatever is beneath.
// The parent app should call draw() after all other widgets so the
// dropdown appears on top.
//
// Keyboard model:
//   Left/Right  — move between menus
//   Enter/Down  — open dropdown (or navigate into it if open)
//   Up/Down     — navigate dropdown items (when open)
//   Enter       — select item (when open)
//   Escape      — close dropdown
//
// Mouse model (button 0, acts on press):
//   Click a title      — open that menu (click the open one again to close)
//   Click a dropdown   — activate that item and close
//   Hover a dropdown   — move the selection highlight
// hit_test() covers the bar plus the open dropdown, so App::route_mouse
// delivers dropdown clicks here instead of to the widget underneath —
// list the MenuBar last (topmost) in route_mouse. Click-away close is the
// parent's call (check dropdown_open() + hit_test before routing).

// The open dropdown states its selection TWICE (#76): inverted colours, and a
// marker glyph in the two columns the item labels were already indented by.
// Colour alone was the whole affordance until v0.1.12, and
// FallbackDriver::draw_text discards colour — so on the tier the framework
// promises always works, an open menu's highlighted item was byte-for-byte
// identical to the rest of the list. That is sharper here than it was for
// ListWidget (#72), because a dropdown is modal and commits: Up/Down moved a
// cursor the user could not see and Enter fired whichever action it happened to
// be on. The marker is drawn by the shared detail/dropdown.hpp skeleton, so
// Select cannot drift away from it.

#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

struct MenuItem {
  std::string label;
  std::function<void()> action;
};

struct Menu {
  std::string title;
  std::vector<MenuItem> items;
};

class MenuBar final : public Widget {
 public:
  MenuBar() = default;

  // Set the menus (replaces existing).
  auto set_menus(std::vector<Menu> menus) -> void;

  // Add a single menu.
  auto add_menu(Menu menu) -> void;

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  // Bar row plus the open dropdown's rows (which extend below rect()).
  [[nodiscard]] auto hit_test(int px, int py) const -> bool override;

  // Whether a dropdown is currently open. Derived from the selection, not a
  // separate flag (#56 item 7): every write site kept m_open == (m_selected
  // >= 0), the same lockstep-flag pattern #42 item 4 removed from Select --
  // one fact, no way to desync.
  [[nodiscard]] auto dropdown_open() const noexcept -> bool {
    return m_selected >= 0;
  }

  // Close the dropdown (parent can call on Escape or click-away).
  auto close_dropdown() -> void;

  [[nodiscard]] auto active_menu() const noexcept -> int { return m_active; }

  // Which glyph family the dropdown's selection marker comes from (#76) — the
  // same knob Select, Frame and ListWidget take, so an app holding one
  // BorderStyle passes it here too and BorderStyle::Ascii keeps a bare TTY
  // 7-bit. MenuBar draws no box, so this is its only use for a style.
  auto set_style(BorderStyle style) -> void {
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

 private:
  // Compute the x position and width of each menu title.
  auto layout_menus() const -> std::vector<std::pair<int, int>>;

  // Width of a menu's dropdown given its title width.
  auto dropdown_width(const Menu& menu, int title_w) const -> int;

  // Screen rect of the open dropdown; {0,0,0,0} when closed. draw() and
  // hit_test()/on_event() share this so they can never disagree. draw()
  // passes the title layout it already computed so an open frame scans the
  // titles once, not twice (#56 item 4).
  using TitleLayout = std::vector<std::pair<int, int>>;
  [[nodiscard]] auto dropdown_rect(const TitleLayout* layout = nullptr) const
      -> Rect;

  auto handle_mouse(const MouseEvent& m) -> bool;
  auto open_menu(int index) -> void;

  std::vector<Menu> m_menus;
  int m_active{0};       // which menu is highlighted/open
  // Selected item in the open dropdown; doubles as the open flag (>= 0 iff
  // open, see dropdown_open()) -- the Select m_highlight pattern (#42/4).
  int m_selected{-1};
  int m_screen_rows{0};  // memoized from draw(); 0 = no frame yet (unclamped)

  // #76: the affordance that survives a driver which drops colour.
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{0x20, 0x20, 0x40};
  Rgb m_active_fg{theme::kFocusFg};
  Rgb m_active_bg{theme::kFocusBg};
  Rgb m_dropdown_fg{theme::kDropdownFg};
  Rgb m_dropdown_bg{theme::kDropdownBg};
  Rgb m_selected_fg{theme::kFocusFg};
  Rgb m_selected_bg{theme::kFocusBg};
};

}  // namespace termforge
