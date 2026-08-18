#include "termforge/widgets/choice_wizard_dialog.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

#include "detail/width.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {

namespace {

auto buttons_width(std::initializer_list<const Button*> buttons) -> int {
  int total = 0;
  for (const Button* button : buttons)
    total += detail::display_width(button->label()) + 1;
  return total > 0 ? total - 1 : 0;
}

auto place_buttons(Rect area, std::initializer_list<Button*> buttons) -> void {
  int total = 0;
  for (const Button* button : buttons)
    total += detail::display_width(button->label()) + 1;
  if (total > 0) --total;
  const int right = area.x + area.w;
  int x = area.x + std::max(0, area.w - total);
  for (Button* button : buttons) {
    const int want = detail::display_width(button->label());
    const int width = area.h > 0 ? std::clamp(right - x, 0, want) : 0;
    button->set_geometry({x, area.y, width, width > 0 ? 1 : 0});
    x += want + 1;
  }
}

auto empty_rect_at(Rect area) -> Rect {
  return {area.x, area.y, 0, 0};
}

auto sanitize_multiline(std::string_view text) -> std::string {
  std::string sanitized;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = text.find('\n', begin);
    const std::size_t count =
        end == std::string_view::npos ? text.size() - begin : end - begin;
    sanitized += Screen::sanitize(text.substr(begin, count));
    if (end == std::string_view::npos) break;
    sanitized += '\n';
    begin = end + 1;
  }
  return sanitized;
}

auto normalize_page(ChoiceWizardPage& page) -> void {
  page.title = Screen::sanitize(page.title);
  page.text = sanitize_multiline(page.text);
  page.other_label = Screen::sanitize(page.other_label);
  page.other_placeholder = Screen::sanitize(page.other_placeholder);
  for (auto& choice : page.choices) {
    choice.label = Screen::sanitize(choice.label);
    choice.description = Screen::sanitize(choice.description);
  }

  std::set<std::size_t> wanted;
  for (const std::size_t index : page.selected_indices)
    if (index < page.choices.size()) wanted.insert(index);
  page.selected_indices.assign(wanted.begin(), wanted.end());
  page.other_selected = page.other_enabled && page.other_selected;

  if (page.mode != ChoiceMode::Single) return;
  if (!page.selected_indices.empty()) {
    page.selected_indices.erase(page.selected_indices.begin() + 1,
                                page.selected_indices.end());
    page.other_selected = false;
  } else if (page.other_selected) {
    page.selected_indices.clear();
  } else if (!page.choices.empty()) {
    page.selected_indices = {0};
  } else {
    page.other_selected = page.other_enabled;
  }
}

} // namespace

auto ChoiceWizardDialog::build() -> void {
  m_single.on_change([this](int selected) {
    if (m_pages.empty() || !m_pages[m_current_page].other_enabled) return;
    const bool other =
        selected == static_cast<int>(m_pages[m_current_page].choices.size());
    if (other == m_pages[m_current_page].other_selected) return;
    m_pages[m_current_page].other_selected = other;
    m_rebuild_pending = true;
    m_focus_other_pending = other;
    m_validation.clear();
  });
  m_other_check.on_change([this](bool selected) {
    if (!m_pages.empty()) m_pages[m_current_page].other_selected = selected;
    m_rebuild_pending = true;
    m_focus_other_pending = selected;
    m_validation.clear();
  });
  m_other_input.on_change([this](const std::string&) {
    m_validation.clear();
    mark_dirty();
  });
  m_back.on_activate([this] { go_back(); });
  m_advance.on_activate([this] { advance_or_submit(); });
  m_cancel.on_activate([this] { finish_cancel(); });
  refresh_button_labels();
  rebuild_children();
}

auto ChoiceWizardDialog::set_pages(std::vector<ChoiceWizardPage> pages,
                                   std::size_t initial_page) -> bool {
  if (pages.empty() || initial_page >= pages.size()) return false;
  for (const auto& page : pages)
    if (page.maximum_selected && *page.maximum_selected < page.minimum_selected)
      return false;

  for (auto& page : pages)
    normalize_page(page);
  m_pages = std::move(pages);
  m_current_page = initial_page;
  load_page();
  mark_dirty();
  return true;
}

auto ChoiceWizardDialog::set_labels(std::string back, std::string next,
                                    std::string submit, std::string cancel)
    -> void {
  m_back_label = Screen::sanitize(back);
  m_next_label = Screen::sanitize(next);
  m_submit_label = Screen::sanitize(submit);
  m_cancel_label = Screen::sanitize(cancel);
  refresh_button_labels();
  mark_dirty();
}

auto ChoiceWizardDialog::refresh_button_labels() -> void {
  m_back.set_label("[ " + m_back_label + " ]");
  const bool final = !m_pages.empty() && m_current_page + 1 == m_pages.size();
  m_advance.set_label("[ " + (final ? m_submit_label : m_next_label) + " ]");
  m_cancel.set_label("[ " + m_cancel_label + " ]");
}

auto ChoiceWizardDialog::load_page() -> void {
  if (m_pages.empty()) return;
  const auto& page = m_pages[m_current_page];
  set_title(page.title);
  set_text(page.text);
  m_other_check.set_label(page.other_label);
  m_other_input.set_placeholder(page.other_placeholder);
  m_other_input.set_text(page.other_text);
  m_validation.clear();
  m_multiple_scroll = 0;
  refresh_button_labels();
  rebuild_controls();
  focus_page();
}

auto ChoiceWizardDialog::save_current_page() -> void {
  if (m_pages.empty()) return;
  auto& page = m_pages[m_current_page];
  page.selected_indices = selected_indices();
  page.other_selected = other_selected();
  page.other_text = m_other_input.text();
}

auto ChoiceWizardDialog::rebuild_controls() -> void {
  clear_children();
  m_multiple.clear();
  if (m_pages.empty()) {
    rebuild_children();
    return;
  }

  auto& page = m_pages[m_current_page];
  std::vector<std::string> labels;
  labels.reserve(page.choices.size() + (page.other_enabled ? 1U : 0U));
  for (const auto& choice : page.choices)
    labels.push_back(choice.label);
  if (page.other_enabled) labels.push_back(page.other_label);
  m_single.set_options(std::move(labels));

  std::set<std::size_t> wanted(page.selected_indices.begin(),
                               page.selected_indices.end());
  m_multiple.reserve(page.choices.size());
  for (std::size_t i = 0; i < page.choices.size(); ++i) {
    auto checkbox = std::make_unique<Checkbox>(page.choices[i].label);
    checkbox->set_style(border_style());
    checkbox->set_checked(wanted.contains(i));
    checkbox->on_change([this](bool) {
      m_validation.clear();
      mark_dirty();
    });
    m_multiple.push_back(std::move(checkbox));
  }

  if (page.mode == ChoiceMode::Single) {
    if (!page.selected_indices.empty()) {
      m_single.set_selected(static_cast<int>(page.selected_indices.front()));
      page.other_selected = false;
    } else if (page.other_enabled && page.other_selected) {
      m_single.set_selected(static_cast<int>(page.choices.size()));
    }
  } else {
    m_other_check.set_checked(page.other_selected);
  }
  rebuild_children();
}

auto ChoiceWizardDialog::rebuild_children() -> void {
  clear_children();
  if (!m_pages.empty()) {
    const auto& page = m_pages[m_current_page];
    if (page.mode == ChoiceMode::Single) {
      add_child(&m_single);
    } else {
      for (auto& checkbox : m_multiple)
        add_child(checkbox.get());
      if (page.other_enabled) add_child(&m_other_check);
    }
    if (page.other_enabled && other_selected()) add_child(&m_other_input);
    if (m_current_page > 0) add_child(&m_back);
  }
  add_child(&m_advance);
  add_child(&m_cancel);
  m_rebuild_pending = false;
}

auto ChoiceWizardDialog::sync_other_transition() -> void {
  if (!m_rebuild_pending) return;
  rebuild_children();
  if (m_focus_other_pending && other_selected()) ring().focus(&m_other_input);
  m_focus_other_pending = false;
}

auto ChoiceWizardDialog::focus_page() -> void {
  if (m_pages.empty()) {
    ring().focus(&m_advance);
    return;
  }
  const auto& page = m_pages[m_current_page];
  if (page.mode == ChoiceMode::Single) {
    if (other_selected())
      ring().focus(&m_other_input);
    else
      ring().focus(&m_single);
  } else if (!m_multiple.empty()) {
    ring().focus(m_multiple.front().get());
  } else if (page.other_enabled) {
    ring().focus(other_selected() ? static_cast<Widget*>(&m_other_input)
                                  : static_cast<Widget*>(&m_other_check));
  } else {
    ring().focus(&m_advance);
  }
}

auto ChoiceWizardDialog::on_show() -> void {
  m_validation.clear();
  focus_page();
}

auto ChoiceWizardDialog::on_event(const Event& event) -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key != nullptr && key->action == KeyAction::Release)
    return false;

  const bool may_finish = activates_terminal_control(event);
  const bool handled = Dialog::on_event(event);
  if (may_finish) return handled;
  sync_other_transition();
  ensure_multiple_visible();
  if (handled) return true;

  if (const auto* key = std::get_if<KeyEvent>(&event);
      key != nullptr && key->action != KeyAction::Release &&
      key->key == Key::Enter && !key->ctrl && !key->alt) {
    advance_or_submit();
    return true;
  }
  return false;
}

auto ChoiceWizardDialog::activates_terminal_control(const Event& event) const
    -> bool {
  const bool final = !m_pages.empty() && m_current_page + 1 == m_pages.size();
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release) return false;
    if (key->key == Key::Escape && !key->ctrl && !key->alt) return true;
    const bool activation =
        key->key == Key::Enter || (key->key == Key::Char && key->ch == U' ');
    return activation && (ring().current() == &m_cancel ||
                          (final && ring().current() == &m_advance));
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    return mouse->pressed && mouse->button == 0 &&
           (m_cancel.rect().contains(mouse->x, mouse->y) ||
            (final && m_advance.rect().contains(mouse->x, mouse->y)));
  }
  return false;
}

auto ChoiceWizardDialog::selected_indices() const -> std::vector<std::size_t> {
  std::vector<std::size_t> selected;
  if (m_pages.empty()) return selected;
  const auto& page = m_pages[m_current_page];
  if (page.mode == ChoiceMode::Single) {
    const int index = m_single.selected();
    if (index >= 0 && index < static_cast<int>(page.choices.size()))
      selected.push_back(static_cast<std::size_t>(index));
    return selected;
  }
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i]->checked()) selected.push_back(i);
  return selected;
}

auto ChoiceWizardDialog::other_selected() const noexcept -> bool {
  if (m_pages.empty()) return false;
  const auto& page = m_pages[m_current_page];
  if (!page.other_enabled) return false;
  if (page.mode == ChoiceMode::Single)
    return m_single.selected() == static_cast<int>(page.choices.size());
  return m_other_check.checked();
}

auto ChoiceWizardDialog::has_descriptions() const -> bool {
  if (m_pages.empty()) return false;
  return std::ranges::any_of(
      m_pages[m_current_page].choices,
      [](const ChoiceOption& choice) { return !choice.description.empty(); });
}

auto ChoiceWizardDialog::content_rows() const -> int {
  if (m_pages.empty()) return 3;
  const auto& page = m_pages[m_current_page];
  const int count =
      static_cast<int>(page.choices.size()) + (page.other_enabled ? 1 : 0);
  const int choices = std::clamp(count, 1, kMaxChoiceRows);
  return choices + (has_descriptions() ? 1 : 0) +
         (page.other_enabled && other_selected() ? 1 : 0) + 2;
}

auto ChoiceWizardDialog::content_cols() const -> int {
  int width = 24;
  if (m_current_page > 0)
    width = std::max(width, buttons_width({&m_back, &m_advance, &m_cancel}));
  else
    width = std::max(width, buttons_width({&m_advance, &m_cancel}));
  if (m_pages.empty()) return width;
  const auto& page = m_pages[m_current_page];
  for (const auto& choice : page.choices) {
    width = std::max(width,
                     Checkbox::width_for(detail::display_width(choice.label)));
    width = std::max(width, detail::display_width(choice.description));
  }
  if (page.other_enabled)
    width = std::max(
        width, Checkbox::width_for(detail::display_width(page.other_label)));
  return width;
}

auto ChoiceWizardDialog::layout_content(Rect area) -> void {
  const BorderStyle style = border_style();
  if (m_single.style() != style) m_single.set_style(style);
  for (auto& checkbox : m_multiple)
    if (checkbox->style() != style) checkbox->set_style(style);
  if (m_other_check.style() != style) m_other_check.set_style(style);

  m_choice_area = m_description_area = m_other_area = m_validation_area =
      m_button_area = empty_rect_at(area);
  m_single.set_geometry(empty_rect_at(area));
  for (auto& checkbox : m_multiple)
    checkbox->set_geometry(empty_rect_at(area));
  m_other_check.set_geometry(empty_rect_at(area));
  m_other_input.set_geometry(empty_rect_at(area));
  m_back.set_geometry(empty_rect_at(area));
  m_advance.set_geometry(empty_rect_at(area));
  m_cancel.set_geometry(empty_rect_at(area));

  int bottom = area.y + area.h;
  auto take_bottom = [&](bool wanted) -> Rect {
    if (!wanted || bottom <= area.y) return empty_rect_at(area);
    --bottom;
    return {area.x, bottom, area.w, 1};
  };

  m_button_area = take_bottom(true);
  m_validation_area = take_bottom(true);
  const bool show_other = !m_pages.empty() &&
                          m_pages[m_current_page].other_enabled &&
                          other_selected();
  m_other_area = take_bottom(show_other);
  m_description_area = take_bottom(has_descriptions());
  m_choice_area = {area.x, area.y, area.w, std::max(0, bottom - area.y)};

  if (m_current_page > 0)
    place_buttons(m_button_area, {&m_back, &m_advance, &m_cancel});
  else
    place_buttons(m_button_area, {&m_advance, &m_cancel});
  if (m_other_area.h > 0) m_other_input.set_geometry(m_other_area);
  if (m_pages.empty()) return;

  const auto& page = m_pages[m_current_page];
  if (page.mode == ChoiceMode::Single) {
    m_single.set_geometry(m_choice_area);
    return;
  }

  const int total =
      static_cast<int>(m_multiple.size()) + (page.other_enabled ? 1 : 0);
  const int visible = m_choice_area.h;
  m_multiple_scroll = std::clamp(m_multiple_scroll, 0,
                                 std::max(0, total - std::max(0, visible)));
  for (int row = 0; row < visible; ++row) {
    const int index = m_multiple_scroll + row;
    if (index < static_cast<int>(m_multiple.size())) {
      m_multiple[static_cast<std::size_t>(index)]->set_geometry(
          {m_choice_area.x, m_choice_area.y + row, m_choice_area.w, 1});
    } else if (page.other_enabled &&
               index == static_cast<int>(m_multiple.size())) {
      m_other_check.set_geometry(
          {m_choice_area.x, m_choice_area.y + row, m_choice_area.w, 1});
    }
  }
}

auto ChoiceWizardDialog::ensure_multiple_visible() -> void {
  if (m_pages.empty() || m_pages[m_current_page].mode != ChoiceMode::Multiple ||
      m_choice_area.h <= 0)
    return;
  const Widget* current = ring().current();
  int index = -1;
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i].get() == current) index = static_cast<int>(i);
  if (m_pages[m_current_page].other_enabled && current == &m_other_check)
    index = static_cast<int>(m_multiple.size());
  if (index < 0) return;
  if (index < m_multiple_scroll) m_multiple_scroll = index;
  if (index >= m_multiple_scroll + m_choice_area.h)
    m_multiple_scroll = index - m_choice_area.h + 1;
  mark_dirty();
}

auto ChoiceWizardDialog::current_description() const -> const std::string& {
  static const std::string empty;
  if (m_pages.empty()) return empty;
  const auto& page = m_pages[m_current_page];
  if (page.mode == ChoiceMode::Single) {
    const int index = m_single.selected();
    if (index >= 0 && index < static_cast<int>(page.choices.size()))
      return page.choices[static_cast<std::size_t>(index)].description;
    return empty;
  }
  Widget* current = ring().current();
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i].get() == current) return page.choices[i].description;
  const auto selected = selected_indices();
  if (!selected.empty()) return page.choices[selected.front()].description;
  return empty;
}

auto ChoiceWizardDialog::draw_content(Screen& screen) -> void {
  if (!m_pages.empty()) {
    const auto& page = m_pages[m_current_page];
    if (page.mode == ChoiceMode::Single) {
      m_single.draw(screen);
    } else {
      for (auto& checkbox : m_multiple)
        if (checkbox->rect().w > 0 && checkbox->rect().h > 0)
          checkbox->draw(screen);
      if (m_other_check.rect().w > 0 && m_other_check.rect().h > 0)
        m_other_check.draw(screen);
    }
    if (m_other_area.h > 0) m_other_input.draw(screen);
  }

  if (m_description_area.h > 0) {
    const auto& description = current_description();
    screen.write_text(
        m_description_area.x, m_description_area.y,
        detail::truncate_to_width(description, m_description_area.w),
        Rgb{0x90, 0x98, 0xA8}, bg());
  }
  if (m_validation_area.h > 0 && !m_validation.empty()) {
    screen.write_text(
        m_validation_area.x, m_validation_area.y,
        detail::truncate_to_width(m_validation, m_validation_area.w),
        Rgb{0xFF, 0xA0, 0x60}, bg());
  }
  if (m_current_page > 0) m_back.draw(screen);
  m_advance.draw(screen);
  m_cancel.draw(screen);
}

auto ChoiceWizardDialog::selected_count() const -> std::size_t {
  return selected_indices().size() + (other_selected() ? 1U : 0U);
}

auto ChoiceWizardDialog::validate() -> bool {
  if (m_pages.empty()) {
    m_validation = "Add at least one page.";
    mark_dirty();
    return false;
  }
  const auto& page = m_pages[m_current_page];
  const std::size_t count = selected_count();
  if (page.mode == ChoiceMode::Single && count != 1) {
    m_validation = "Select one option.";
    mark_dirty();
    return false;
  }
  if (page.mode == ChoiceMode::Multiple && count < page.minimum_selected) {
    m_validation =
        std::format("Select at least {} option{}.", page.minimum_selected,
                    page.minimum_selected == 1 ? "" : "s");
    mark_dirty();
    return false;
  }
  if (page.mode == ChoiceMode::Multiple && page.maximum_selected &&
      count > *page.maximum_selected) {
    m_validation =
        std::format("Select at most {} option{}.", *page.maximum_selected,
                    *page.maximum_selected == 1 ? "" : "s");
    mark_dirty();
    return false;
  }
  if (other_selected() && m_other_input.text().empty()) {
    m_validation = "Enter an Other response.";
    ring().focus(&m_other_input);
    mark_dirty();
    return false;
  }
  m_validation.clear();
  return true;
}

auto ChoiceWizardDialog::go_back() -> void {
  if (m_pages.empty() || m_current_page == 0) return;
  save_current_page();
  --m_current_page;
  load_page();
  mark_dirty();
}

auto ChoiceWizardDialog::advance_or_submit() -> void {
  if (!validate()) return;
  if (m_current_page + 1 == m_pages.size()) {
    finish_submit();
    return;
  }
  save_current_page();
  ++m_current_page;
  load_page();
  mark_dirty();
}

auto ChoiceWizardDialog::finish_submit() -> void {
  if (!validate()) return;
  save_current_page();
  if (!begin_result()) return;

  ChoiceWizardResult result;
  result.pages.reserve(m_pages.size());
  for (const auto& page : m_pages) {
    ChoiceResult answer{.selected_indices = page.selected_indices, .other = {}};
    if (page.other_selected) answer.other = page.other_text;
    result.pages.push_back(std::move(answer));
  }

  auto callback = m_on_result;
  close();
  detail::invoke_copy(callback,
                      std::optional<ChoiceWizardResult>{std::move(result)});
}

auto ChoiceWizardDialog::finish_cancel() -> void {
  save_current_page();
  if (!begin_result()) return;
  auto callback = m_on_result;
  close();
  detail::invoke_copy(callback, std::optional<ChoiceWizardResult>{});
}

} // namespace termforge
