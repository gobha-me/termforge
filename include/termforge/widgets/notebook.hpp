#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/tab_bar.hpp"

namespace termforge {

// Persistent tabbed pages, not a window manager. Pages are borrowed Widget
// roots: keep them alive until removed (and through any in-flight dispatch).
// A compound page owns its internal layout/routing, and preserves its focus
// cursor across set_focused(false/true). It may expose active pixel_children.
// Notebook never resets content, selections or scroll positions.
class Notebook final : public Widget {
 public:
  Notebook();
  Notebook(const Notebook&) = delete;
  auto operator=(const Notebook&) -> Notebook& = delete;
  Notebook(Notebook&&) = delete;
  auto operator=(Notebook&&) -> Notebook& = delete;

  // Null, self and duplicate roots are refused without mutation. Registration
  // and programmatic selection/removal are silent; only user changes notify.
  auto add_page(std::string title, Widget* page) -> bool;
  auto remove_page(Widget* page) -> bool;
  auto clear() -> void;
  auto set_active(int index) -> void;
  [[nodiscard]] auto active() const noexcept -> int { return m_active; }
  [[nodiscard]] auto count() const noexcept -> int { return m_tabs.count(); }
  [[nodiscard]] auto active_page() const noexcept -> Widget*;
  [[nodiscard]] auto content_rect() const noexcept -> Rect;
  auto on_change(std::function<void(int)> callback) -> void;
  auto set_style(BorderStyle style) -> void {
    m_tabs.set_style(style);
    mark_dirty();
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  auto reset_transient() -> void override;
  auto set_focused(bool focus) -> void override;
  [[nodiscard]] auto focusable() const -> bool override { return count() != 0; }
  [[nodiscard]] auto hit_test_tree(int x, int y) const -> bool override;
  auto pixel_children() -> std::vector<Widget*> override;

 private:
  auto on_theme_changed() -> void override {
    if (theme_snapshot())
      m_tabs.set_theme(*theme_snapshot());
    else
      m_tabs.clear_theme();
  }
  struct Page {
    std::string title;
    Widget* root;
  };
  auto sync_focus() -> void;
  auto select(int index, bool notify) -> void;
  auto layout() -> void;
  TabBar m_tabs;
  std::vector<Page> m_pages;
  int m_active{-1};
  bool m_content_focus{false};
  std::function<void(int)> m_on_change;
};

} // namespace termforge
