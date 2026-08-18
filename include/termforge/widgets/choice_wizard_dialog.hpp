#pragma once

// TermForge — ChoiceWizardDialog: a paged choice-form modal composition.
//
// Each page uses the same presentation-index contract as ChoiceDialog. The
// wizard owns navigation and preserves page-local selections and Other text;
// applications retain stable ids and map them at the boundary.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "termforge/widgets/button.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/choice_dialog.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/text_input.hpp"

namespace termforge {

struct ChoiceWizardPage {
  std::string title;
  std::string text;
  ChoiceMode mode{ChoiceMode::Single};
  std::vector<ChoiceOption> choices;
  std::vector<std::size_t> selected_indices;
  std::size_t minimum_selected{0};
  std::optional<std::size_t> maximum_selected;
  bool other_enabled{false};
  std::string other_label{"Other"};
  std::string other_placeholder;
  bool other_selected{false};
  std::string other_text;
};

struct ChoiceWizardResult {
  std::vector<ChoiceResult> pages;
};

class ChoiceWizardDialog final : public Dialog {
 public:
  ChoiceWizardDialog() { build(); }

  // Replaces the complete wizard atomically. Empty page sets, an out-of-range
  // initial page and maximum < minimum are rejected without changing the
  // current wizard. Presentation strings are sanitized before measurement.
  [[nodiscard]] auto set_pages(std::vector<ChoiceWizardPage> pages,
                               std::size_t initial_page = 0) -> bool;
  [[nodiscard]] auto page_count() const noexcept -> std::size_t {
    return m_pages.size();
  }
  [[nodiscard]] auto current_page() const noexcept -> std::size_t {
    return m_current_page;
  }

  auto set_labels(std::string back, std::string next, std::string submit,
                  std::string cancel) -> void;

  // Called once per showing: ordered page results on final submission,
  // nullopt on cancellation. Back and Next never report or close.
  auto on_result(
      std::function<void(std::optional<ChoiceWizardResult>)> callback) -> void {
    m_on_result = std::move(callback);
  }

  auto on_event(const Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override;
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen& screen) -> void override;
  auto on_escape() -> void override { finish_cancel(); }
  auto on_show() -> void override;

 private:
  static constexpr int kMaxChoiceRows{8};

  auto build() -> void;
  auto load_page() -> void;
  auto save_current_page() -> void;
  auto rebuild_children() -> void;
  auto rebuild_controls() -> void;
  auto sync_other_transition() -> void;
  auto focus_page() -> void;
  auto refresh_button_labels() -> void;
  [[nodiscard]] auto activates_terminal_control(const Event& event) const
      -> bool;
  auto ensure_multiple_visible() -> void;
  [[nodiscard]] auto current_description() const -> const std::string&;
  [[nodiscard]] auto has_descriptions() const -> bool;
  [[nodiscard]] auto selected_indices() const -> std::vector<std::size_t>;
  [[nodiscard]] auto other_selected() const noexcept -> bool;
  [[nodiscard]] auto selected_count() const -> std::size_t;
  [[nodiscard]] auto validate() -> bool;
  auto go_back() -> void;
  auto advance_or_submit() -> void;
  auto finish_submit() -> void;
  auto finish_cancel() -> void;

  std::vector<ChoiceWizardPage> m_pages;
  std::size_t m_current_page{0};

  RadioGroup m_single;
  std::vector<std::unique_ptr<Checkbox>> m_multiple;
  Checkbox m_other_check{"Other"};
  TextInput m_other_input;
  Button m_back{"[ Back ]"};
  Button m_advance{"[ Submit ]"};
  Button m_cancel{"[ Cancel ]"};
  std::function<void(std::optional<ChoiceWizardResult>)> m_on_result;

  std::string m_back_label{"Back"};
  std::string m_next_label{"Next"};
  std::string m_submit_label{"Submit"};
  std::string m_cancel_label{"Cancel"};
  bool m_rebuild_pending{false};
  bool m_focus_other_pending{false};
  int m_multiple_scroll{0};
  Rect m_choice_area{};
  Rect m_description_area{};
  Rect m_other_area{};
  Rect m_validation_area{};
  Rect m_button_area{};
  std::string m_validation;
};

} // namespace termforge
