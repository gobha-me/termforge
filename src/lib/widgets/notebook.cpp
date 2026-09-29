#include "termforge/widgets/notebook.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

Notebook::Notebook() {
  m_tabs.on_change([this](int index) { select(index, true); });
}

auto Notebook::add_page(std::string title, Widget* page) -> bool {
  if (page == nullptr || page == this ||
      std::any_of(m_pages.begin(), m_pages.end(),
                  [page](const Page& p) { return p.root == page; }))
    return false;
  page->set_focused(false);
  m_tabs.add_tab(title);
  m_pages.push_back({std::move(title), page});
  if (m_active < 0) m_active = 0;
  layout();
  sync_focus();
  mark_dirty();
  return true;
}

auto Notebook::remove_page(Widget* page) -> bool {
  const auto it =
      std::find_if(m_pages.begin(), m_pages.end(),
                   [page](const Page& p) { return p.root == page; });
  if (it == m_pages.end()) return false;
  const int index = static_cast<int>(it - m_pages.begin());
  page->set_focused(false);
  page->reset_transient();
  m_pages.erase(it);
  if (index < m_active) --m_active;
  if (m_pages.empty())
    m_active = -1;
  else
    m_active = std::min(m_active, static_cast<int>(m_pages.size()) - 1);
  std::vector<std::string> titles;
  titles.reserve(m_pages.size());
  for (const auto& p : m_pages)
    titles.push_back(p.title);
  m_tabs.set_tabs(std::move(titles));
  m_tabs.set_active(m_active);
  layout();
  sync_focus();
  mark_dirty();
  return true;
}

auto Notebook::clear() -> void {
  for (const auto& page : m_pages) {
    page.root->set_focused(false);
    page.root->reset_transient();
  }
  m_pages.clear();
  m_tabs.clear();
  m_tabs.set_focused(false);
  m_active = -1;
  m_content_focus = false;
  mark_dirty();
}

auto Notebook::active_page() const noexcept -> Widget* {
  return m_active < 0 ? nullptr
                      : m_pages[static_cast<std::size_t>(m_active)].root;
}

auto Notebook::content_rect() const noexcept -> Rect {
  const Rect r = rect();
  const int strip = r.h > 0 ? 1 : 0;
  return {r.x, r.y + strip, std::max(0, r.w), std::max(0, r.h - strip)};
}

auto Notebook::layout() -> void {
  const Rect r = rect();
  m_tabs.set_geometry({r.x, r.y, std::max(0, r.w), r.h > 0 ? 1 : 0});
  if (auto* page = active_page()) page->set_geometry(content_rect());
}

auto Notebook::sync_focus() -> void {
  const auto* page = active_page();
  const Rect c = content_rect();
  const bool content = m_content_focus && page != nullptr &&
                       page->focusable() && c.w > 0 && c.h > 0;
  m_tabs.set_focused(focused() && !content);
  for (const auto& p : m_pages)
    p.root->set_focused(focused() && content && p.root == page);
}

auto Notebook::set_focused(bool focus) -> void {
  Widget::set_focused(focus);
  sync_focus();
}

auto Notebook::set_active(int index) -> void {
  select(index, false);
}

auto Notebook::select(int index, bool notify) -> void {
  if (m_pages.empty()) return;
  index = std::clamp(index, 0, static_cast<int>(m_pages.size()) - 1);
  if (index == m_active) return;
  if (auto* old = active_page()) {
    old->set_focused(false);
    old->reset_transient();
  }
  m_active = index;
  m_tabs.set_active(index);
  layout();
  sync_focus();
  mark_dirty();
  if (notify) detail::invoke_copy(m_on_change, index);
}

auto Notebook::on_change(std::function<void(int)> callback) -> void {
  m_on_change = std::move(callback);
}

auto Notebook::draw(Screen& screen) -> void {
  layout();
  sync_focus();
  const Rect r = rect();
  screen.fill_rect(r.x, r.y, r.w, r.h,
                   theme_color(&Theme::content_fg, theme::kFg),
                   theme_color(&Theme::content_bg, theme::kBg));
  m_tabs.draw(screen);
  const Rect c = content_rect();
  if (auto* page = active_page(); page && c.w > 0 && c.h > 0)
    page->draw(screen);
  clear_dirty();
}

auto Notebook::on_event(const Event& event) -> bool {
  layout();
  sync_focus();
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release) return false;
    if (key->key == Key::Tab && key->ctrl && count() > 0) {
      select((m_active + (key->shift ? count() - 1 : 1)) % count(), true);
      return true;
    }
  }
  auto* page = active_page();
  if (page == nullptr) return false;
  const Rect c = content_rect();
  const bool visible = c.w > 0 && c.h > 0;
  const bool content = m_content_focus && visible && page->focusable();
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (m_tabs.hit_test(mouse->x, mouse->y)) {
      if (mouse->pressed) {
        m_content_focus = false;
        sync_focus();
      }
      return m_tabs.on_event(event);
    }
    if (!visible || !page->hit_test_tree(mouse->x, mouse->y)) return false;
    if (mouse->pressed) {
      m_content_focus = true;
      sync_focus();
    }
    return page->on_event(event);
  }
  if (content) {
    if (page->on_event(event)) return true;
  } else if (m_tabs.on_event(event))
    return true;
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->key == Key::Tab) {
    // A compound page returns false at its tab-order boundary. Forward Tab
    // from content escapes to the outer ring; backward Tab returns to the
    // strip. Never trap forward traversal in a nested focus loop.
    if (!content && key->shift) return false;
    if (content && !key->shift) return false;
    if (!content && (!visible || !page->focusable())) return false;
    m_content_focus = !content;
    sync_focus();
    return true;
  }
  return false;
}

auto Notebook::on_tick(std::chrono::duration<double> dt) -> void {
  if (auto* page = active_page()) page->on_tick(dt);
}

auto Notebook::reset_transient() -> void {
  m_tabs.reset_transient();
  if (auto* page = active_page()) page->reset_transient();
}

auto Notebook::hit_test_tree(int x, int y) const -> bool {
  const Rect c = content_rect();
  const auto* page = active_page();
  return hit_test(x, y) ||
         (page && c.w > 0 && c.h > 0 && page->hit_test_tree(x, y));
}

auto Notebook::pixel_children() -> std::vector<Widget*> {
  const Rect c = content_rect();
  if (auto* page = active_page(); page && c.w > 0 && c.h > 0) return {page};
  return {};
}

} // namespace termforge
