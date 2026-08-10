#include "termforge/widgets/choice_dialog.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <set>
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

auto empty_rect_at(Rect area) -> Rect { return {area.x, area.y, 0, 0}; }

}  // namespace

ChoiceDialog::ChoiceDialog(std::string title, std::string text, ChoiceMode mode)
    : Dialog(std::move(title)), m_mode(mode) {
  set_text(std::move(text));
  build();
}

auto ChoiceDialog::build() -> void {
  m_single.on_change([this](int selected) {
    if (!m_other_enabled) return;
    const bool other = selected == static_cast<int>(m_choices.size());
    if (other == m_other_selected) return;
    m_other_selected = other;
    m_rebuild_pending = true;
    m_focus_other_pending = other;
    m_validation.clear();
  });
  m_other_check.on_change([this](bool selected) {
    m_other_selected = selected;
    m_rebuild_pending = true;
    m_focus_other_pending = selected;
    m_validation.clear();
  });
  m_other_input.on_change([this](const std::string&) {
    m_validation.clear();
    mark_dirty();
  });
  m_submit.on_activate([this] { finish_submit(); });
  m_cancel.on_activate([this] { finish_cancel(); });
  rebuild_controls({}, false);
}

auto ChoiceDialog::set_mode(ChoiceMode mode) -> void {
  if (m_mode == mode) return;
  const auto selected = selected_indices();
  const bool other = other_selected();
  m_mode = mode;
  rebuild_controls(selected, other);
  mark_dirty();
}

auto ChoiceDialog::set_choices(std::vector<ChoiceOption> choices) -> void {
  const auto selected = selected_indices();
  const bool other = other_selected();
  for (auto& choice : choices) {
    choice.label = Screen::sanitize(choice.label);
    choice.description = Screen::sanitize(choice.description);
  }
  m_choices = std::move(choices);
  rebuild_controls(selected, other);
  mark_dirty();
}

auto ChoiceDialog::set_selected_indices(std::vector<std::size_t> indices)
    -> void {
  std::set<std::size_t> wanted;
  for (const std::size_t index : indices)
    if (index < m_choices.size()) wanted.insert(index);

  if (m_mode == ChoiceMode::Single) {
    if (!wanted.empty()) {
      m_single.set_selected(static_cast<int>(*wanted.begin()));
      m_other_selected = false;
    } else if (!m_choices.empty()) {
      m_single.set_selected(0);
      m_other_selected = false;
    } else if (m_other_enabled) {
      m_single.set_selected(0);
      m_other_selected = true;
    }
  } else {
    for (std::size_t i = 0; i < m_multiple.size(); ++i)
      m_multiple[i]->set_checked(wanted.contains(i));
  }
  m_validation.clear();
  rebuild_children();
  mark_dirty();
}

auto ChoiceDialog::selected_indices() const -> std::vector<std::size_t> {
  std::vector<std::size_t> selected;
  if (m_mode == ChoiceMode::Single) {
    const int index = m_single.selected();
    if (index >= 0 && index < static_cast<int>(m_choices.size()))
      selected.push_back(static_cast<std::size_t>(index));
    return selected;
  }
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i]->checked()) selected.push_back(i);
  return selected;
}

auto ChoiceDialog::set_selection_limits(
    std::size_t minimum, std::optional<std::size_t> maximum) -> bool {
  if (maximum && *maximum < minimum) return false;
  m_minimum = minimum;
  m_maximum = maximum;
  m_validation.clear();
  mark_dirty();
  return true;
}

auto ChoiceDialog::set_other_enabled(bool enabled) -> void {
  if (m_other_enabled == enabled) return;
  const auto selected = selected_indices();
  const bool was_selected = enabled && m_other_selected;
  m_other_enabled = enabled;
  if (!enabled) m_other_selected = false;
  rebuild_controls(selected, was_selected);
  mark_dirty();
}

auto ChoiceDialog::set_other_label(std::string label) -> void {
  m_other_check.set_label(Screen::sanitize(label));
  const auto selected = selected_indices();
  const bool other = other_selected();
  rebuild_controls(selected, other);
  mark_dirty();
}

auto ChoiceDialog::set_other_placeholder(std::string placeholder) -> void {
  m_other_input.set_placeholder(std::move(placeholder));
  mark_dirty();
}

auto ChoiceDialog::set_other_text(std::string text) -> void {
  m_other_input.set_text(std::move(text));
  m_validation.clear();
  mark_dirty();
}

auto ChoiceDialog::set_other_selected(bool selected) -> void {
  selected = selected && m_other_enabled;
  if (m_mode == ChoiceMode::Single) {
    if (selected) {
      m_single.set_selected(static_cast<int>(m_choices.size()));
      m_other_selected = true;
    } else if (!m_choices.empty()) {
      m_single.set_selected(0);
      m_other_selected = false;
    } else {
      m_other_selected = m_other_enabled;
    }
  } else {
    m_other_selected = selected;
    m_other_check.set_checked(selected);
  }
  m_validation.clear();
  rebuild_children();
  if (selected) ring().focus(&m_other_input);
  mark_dirty();
}

auto ChoiceDialog::other_selected() const noexcept -> bool {
  if (!m_other_enabled) return false;
  if (m_mode == ChoiceMode::Single)
    return m_single.selected() == static_cast<int>(m_choices.size());
  return m_other_check.checked();
}

auto ChoiceDialog::set_labels(std::string submit, std::string cancel) -> void {
  m_submit.set_label("[ " + Screen::sanitize(submit) + " ]");
  m_cancel.set_label("[ " + Screen::sanitize(cancel) + " ]");
  mark_dirty();
}

auto ChoiceDialog::rebuild_controls(
    const std::vector<std::size_t>& selected, bool other_selected_value)
    -> void {
  // Unregister heap-owned checkboxes before destroying them. The order is the
  // whole safety contract of Dialog::clear_children(): clearing afterwards
  // would dereference the stale Widget* entries while trying to blur them.
  clear_children();
  std::set<std::size_t> wanted;
  for (const std::size_t index : selected)
    if (index < m_choices.size()) wanted.insert(index);

  std::vector<std::string> labels;
  labels.reserve(m_choices.size() + (m_other_enabled ? 1U : 0U));
  for (const auto& choice : m_choices) labels.push_back(choice.label);
  if (m_other_enabled) labels.push_back(m_other_check.label());
  m_single.set_options(std::move(labels));

  m_multiple.clear();
  m_multiple.reserve(m_choices.size());
  for (std::size_t i = 0; i < m_choices.size(); ++i) {
    auto checkbox = std::make_unique<Checkbox>(m_choices[i].label);
    checkbox->set_style(border_style());
    checkbox->set_checked(wanted.contains(i));
    checkbox->on_change([this](bool) {
      m_validation.clear();
      mark_dirty();
    });
    m_multiple.push_back(std::move(checkbox));
  }

  if (m_mode == ChoiceMode::Single) {
    if (!wanted.empty()) {
      m_single.set_selected(static_cast<int>(*wanted.begin()));
      m_other_selected = false;
    } else if (m_other_enabled && other_selected_value) {
      m_single.set_selected(static_cast<int>(m_choices.size()));
      m_other_selected = true;
    } else if (!m_choices.empty()) {
      m_single.set_selected(0);
      m_other_selected = false;
    } else {
      m_other_selected = m_other_enabled;
    }
  } else {
    m_other_selected = m_other_enabled && other_selected_value;
    m_other_check.set_checked(m_other_selected);
  }

  m_validation.clear();
  m_multiple_scroll = 0;
  rebuild_children();
}

auto ChoiceDialog::rebuild_children() -> void {
  clear_children();
  if (m_mode == ChoiceMode::Single) {
    add_child(&m_single);
  } else {
    for (auto& checkbox : m_multiple) add_child(checkbox.get());
    if (m_other_enabled) add_child(&m_other_check);
  }
  if (m_other_enabled && other_selected()) add_child(&m_other_input);
  add_child(&m_submit);
  add_child(&m_cancel);
  m_rebuild_pending = false;
}

auto ChoiceDialog::sync_other_transition() -> void {
  if (!m_rebuild_pending) return;
  rebuild_children();
  if (m_focus_other_pending && other_selected()) ring().focus(&m_other_input);
  m_focus_other_pending = false;
}

auto ChoiceDialog::on_event(const Event& event) -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key != nullptr && key->action == KeyAction::Release)
    return false;

  // A result control (including Escape) may run on_close code that destroys
  // this dialog. Compute the shape before dispatch and return immediately
  // afterwards; touching a pending-rebuild flag after Dialog::on_event would
  // be the same UAF class the standard dialog suites guard against.
  const bool may_finish = activates_result_control(event);
  const bool handled = Dialog::on_event(event);
  if (may_finish) return handled;
  sync_other_transition();
  ensure_multiple_visible();
  if (handled) return true;

  if (const auto* key = std::get_if<KeyEvent>(&event);
      key != nullptr && key->action != KeyAction::Release &&
      key->key == Key::Enter && !key->ctrl && !key->alt) {
    finish_submit();
    return true;
  }
  return false;
}

auto ChoiceDialog::activates_result_control(const Event& event) const -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release) return false;
    if (key->key == Key::Escape && !key->ctrl && !key->alt) return true;
    const bool activation = key->key == Key::Enter ||
                            (key->key == Key::Char && key->ch == U' ');
    return activation &&
           (ring().current() == &m_submit || ring().current() == &m_cancel);
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    return mouse->pressed && mouse->button == 0 &&
           (m_submit.rect().contains(mouse->x, mouse->y) ||
            m_cancel.rect().contains(mouse->x, mouse->y));
  }
  return false;
}

auto ChoiceDialog::on_show() -> void {
  m_validation.clear();
  if (m_mode == ChoiceMode::Single) {
    if (other_selected())
      ring().focus(&m_other_input);
    else
      ring().focus(&m_single);
  } else if (!m_multiple.empty()) {
    ring().focus(m_multiple.front().get());
  } else if (m_other_enabled) {
    ring().focus(other_selected() ? static_cast<Widget*>(&m_other_input)
                                  : static_cast<Widget*>(&m_other_check));
  }
}

auto ChoiceDialog::has_descriptions() const -> bool {
  return std::ranges::any_of(m_choices, [](const ChoiceOption& choice) {
    return !choice.description.empty();
  });
}

auto ChoiceDialog::content_rows() const -> int {
  const int count = static_cast<int>(m_choices.size()) +
                    (m_other_enabled ? 1 : 0);
  const int choices = std::clamp(count, 1, kMaxChoiceRows);
  return choices + (has_descriptions() ? 1 : 0) +
         (m_other_enabled && other_selected() ? 1 : 0) +
         2;  // validation row + button row
}

auto ChoiceDialog::content_cols() const -> int {
  int width = std::max(24, buttons_width({&m_submit, &m_cancel}));
  for (const auto& choice : m_choices) {
    width = std::max(width,
                     Checkbox::width_for(detail::display_width(choice.label)));
    width = std::max(width, detail::display_width(choice.description));
  }
  if (m_other_enabled)
    width = std::max(
        width, Checkbox::width_for(detail::display_width(m_other_check.label())));
  return width;
}

auto ChoiceDialog::layout_content(Rect area) -> void {
  // Dialog::set_border_style is intentionally non-virtual. Read the base-owned
  // state here so styling through either ChoiceDialog or Dialog& reaches the
  // composed controls without hiding the base setter by name.
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
  m_submit.set_geometry(empty_rect_at(area));
  m_cancel.set_geometry(empty_rect_at(area));

  int bottom = area.y + area.h;
  auto take_bottom = [&](bool wanted) -> Rect {
    if (!wanted || bottom <= area.y) return empty_rect_at(area);
    --bottom;
    return {area.x, bottom, area.w, 1};
  };

  m_button_area = take_bottom(true);
  m_validation_area = take_bottom(true);
  m_other_area = take_bottom(m_other_enabled && other_selected());
  m_description_area = take_bottom(has_descriptions());
  m_choice_area = {area.x, area.y, area.w, std::max(0, bottom - area.y)};

  place_buttons(m_button_area, {&m_submit, &m_cancel});
  if (m_other_area.h > 0) m_other_input.set_geometry(m_other_area);

  if (m_mode == ChoiceMode::Single) {
    m_single.set_geometry(m_choice_area);
    return;
  }

  const int total = static_cast<int>(m_multiple.size()) +
                    (m_other_enabled ? 1 : 0);
  const int visible = m_choice_area.h;
  m_multiple_scroll = std::clamp(
      m_multiple_scroll, 0, std::max(0, total - std::max(0, visible)));
  for (int row = 0; row < visible; ++row) {
    const int index = m_multiple_scroll + row;
    if (index < static_cast<int>(m_multiple.size())) {
      m_multiple[static_cast<std::size_t>(index)]->set_geometry(
          {m_choice_area.x, m_choice_area.y + row, m_choice_area.w, 1});
    } else if (m_other_enabled &&
               index == static_cast<int>(m_multiple.size())) {
      m_other_check.set_geometry(
          {m_choice_area.x, m_choice_area.y + row, m_choice_area.w, 1});
    }
  }
}

auto ChoiceDialog::ensure_multiple_visible() -> void {
  if (m_mode != ChoiceMode::Multiple || m_choice_area.h <= 0) return;
  const Widget* current = ring().current();
  int index = -1;
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i].get() == current) index = static_cast<int>(i);
  if (m_other_enabled && current == &m_other_check)
    index = static_cast<int>(m_multiple.size());
  if (index < 0) return;
  if (index < m_multiple_scroll) m_multiple_scroll = index;
  if (index >= m_multiple_scroll + m_choice_area.h)
    m_multiple_scroll = index - m_choice_area.h + 1;
  mark_dirty();
}

auto ChoiceDialog::current_description() const -> const std::string& {
  static const std::string empty;
  if (m_mode == ChoiceMode::Single) {
    const int index = m_single.selected();
    if (index >= 0 && index < static_cast<int>(m_choices.size()))
      return m_choices[static_cast<std::size_t>(index)].description;
    return empty;
  }
  Widget* current = ring().current();
  for (std::size_t i = 0; i < m_multiple.size(); ++i)
    if (m_multiple[i].get() == current) return m_choices[i].description;
  const auto selected = selected_indices();
  if (!selected.empty()) return m_choices[selected.front()].description;
  return empty;
}

auto ChoiceDialog::draw_content(Screen& screen) -> void {
  if (m_mode == ChoiceMode::Single) {
    m_single.draw(screen);
  } else {
    for (auto& checkbox : m_multiple)
      if (checkbox->rect().w > 0 && checkbox->rect().h > 0)
        checkbox->draw(screen);
    if (m_other_check.rect().w > 0 && m_other_check.rect().h > 0)
      m_other_check.draw(screen);
  }
  if (m_other_area.h > 0) m_other_input.draw(screen);

  if (m_description_area.h > 0) {
    const auto& description = current_description();
    screen.write_text(m_description_area.x, m_description_area.y,
                      detail::truncate_to_width(description,
                                                m_description_area.w),
                      Rgb{0x90, 0x98, 0xA8}, bg());
  }
  if (m_validation_area.h > 0 && !m_validation.empty()) {
    screen.write_text(m_validation_area.x, m_validation_area.y,
                      detail::truncate_to_width(m_validation,
                                                m_validation_area.w),
                      Rgb{0xFF, 0xA0, 0x60}, bg());
  }
  m_submit.draw(screen);
  m_cancel.draw(screen);
}

auto ChoiceDialog::selected_count() const -> std::size_t {
  return selected_indices().size() + (other_selected() ? 1U : 0U);
}

auto ChoiceDialog::validate() -> bool {
  const std::size_t count = selected_count();
  if (m_mode == ChoiceMode::Single && count != 1) {
    m_validation = "Select one option.";
    mark_dirty();
    return false;
  }
  if (m_mode == ChoiceMode::Multiple && count < m_minimum) {
    m_validation = std::format("Select at least {} option{}.", m_minimum,
                               m_minimum == 1 ? "" : "s");
    mark_dirty();
    return false;
  }
  if (m_mode == ChoiceMode::Multiple && m_maximum && count > *m_maximum) {
    m_validation = std::format("Select at most {} option{}.", *m_maximum,
                               *m_maximum == 1 ? "" : "s");
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

auto ChoiceDialog::finish_submit() -> void {
  if (!validate() || !begin_result()) return;
  ChoiceResult result{.selected_indices = selected_indices(), .other = {}};
  if (other_selected()) result.other = m_other_input.text();
  auto callback = m_on_result;
  close();
  detail::invoke_copy(callback,
                      std::optional<ChoiceResult>{std::move(result)});
}

auto ChoiceDialog::finish_cancel() -> void {
  if (!begin_result()) return;
  auto callback = m_on_result;
  close();
  detail::invoke_copy(callback, std::optional<ChoiceResult>{});
}

}  // namespace termforge
