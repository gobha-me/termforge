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

#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/widget.hpp"

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

  // Whether a dropdown is currently open.
  [[nodiscard]] auto dropdown_open() const noexcept -> bool {
    return m_open;
  }

  // Close the dropdown (parent can call on Escape or click-away).
  auto close_dropdown() -> void;

  [[nodiscard]] auto active_menu() const noexcept -> int { return m_active; }

 private:
  // Compute the x position and width of each menu title.
  auto layout_menus() const -> std::vector<std::pair<int, int>>;

  // Width of a menu's dropdown given its title width.
  auto dropdown_width(const Menu& menu, int title_w) const -> int;

  // Screen rect of the open dropdown; {0,0,0,0} when closed. draw() and
  // hit_test()/on_event() share this so they can never disagree.
  [[nodiscard]] auto dropdown_rect() const -> Rect;

  auto handle_mouse(const MouseEvent& m) -> bool;
  auto open_menu(int index) -> void;

  std::vector<Menu> m_menus;
  int m_active{0};       // which menu is highlighted/open
  int m_selected{-1};    // selected item in the open dropdown (-1 = none)
  bool m_open{false};

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x20, 0x20, 0x40};
  Rgb m_active_fg{0x0A, 0x0A, 0x14};
  Rgb m_active_bg{0x40, 0x80, 0xFF};
  Rgb m_dropdown_fg{0xE0, 0xE0, 0xF0};
  Rgb m_dropdown_bg{0x15, 0x15, 0x25};
  Rgb m_selected_fg{0x0A, 0x0A, 0x14};
  Rgb m_selected_bg{0x40, 0x80, 0xFF};
};

}  // namespace termforge
