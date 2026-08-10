#pragma once

// TermForge — ChoiceDialog: single/multiple-choice modal composition.
//
// ChoiceDialog is the reusable Layer-3 form assembled from Dialog,
// RadioGroup/Checkbox, TextInput and Buttons. Results use stable presentation
// indices; applications keep their own ids and map them at the boundary.
// Cancellation is std::nullopt, which stays distinct from a valid empty
// multiple-choice result.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "termforge/widgets/button.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/text_input.hpp"

namespace termforge {

enum class ChoiceMode { Single, Multiple };

struct ChoiceOption {
  std::string label;
  std::string description;
};

struct ChoiceResult {
  std::vector<std::size_t> selected_indices;
  std::optional<std::string> other;
};

class ChoiceDialog final : public Dialog {
 public:
  ChoiceDialog() { build(); }
  ChoiceDialog(std::string title, std::string text,
               ChoiceMode mode = ChoiceMode::Single);

  auto set_mode(ChoiceMode mode) -> void;
  [[nodiscard]] auto mode() const noexcept -> ChoiceMode { return m_mode; }

  // Labels and descriptions are sanitized at this setter. Replacing choices
  // preserves still-valid selections by index and drops stale indices.
  auto set_choices(std::vector<ChoiceOption> choices) -> void;
  [[nodiscard]] auto choices() const noexcept
      -> const std::vector<ChoiceOption>& {
    return m_choices;
  }

  // Programmatic selection is silent. Single mode keeps the first valid
  // index; Multiple keeps every distinct valid index.
  auto set_selected_indices(std::vector<std::size_t> indices) -> void;
  [[nodiscard]] auto selected_indices() const -> std::vector<std::size_t>;

  // Multiple-mode submit limits. An absent maximum means unbounded. Invalid
  // max < min is rejected atomically and leaves the previous limits intact.
  [[nodiscard]] auto set_selection_limits(
      std::size_t minimum, std::optional<std::size_t> maximum = std::nullopt)
      -> bool;
  [[nodiscard]] auto minimum_selected() const noexcept -> std::size_t {
    return m_minimum;
  }
  [[nodiscard]] auto maximum_selected() const noexcept
      -> std::optional<std::size_t> {
    return m_maximum;
  }

  auto set_other_enabled(bool enabled) -> void;
  [[nodiscard]] auto other_enabled() const noexcept -> bool {
    return m_other_enabled;
  }
  auto set_other_label(std::string label) -> void;
  auto set_other_placeholder(std::string placeholder) -> void;
  auto set_other_text(std::string text) -> void;
  [[nodiscard]] auto other_text() const noexcept -> const std::string& {
    return m_other_input.text();
  }
  auto set_other_selected(bool selected) -> void;
  [[nodiscard]] auto other_selected() const noexcept -> bool;

  auto set_labels(std::string submit, std::string cancel) -> void;

  // Called once per showing: ChoiceResult on submit, nullopt on cancellation.
  auto on_result(
      std::function<void(std::optional<ChoiceResult>)> callback) -> void {
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
  auto rebuild_children() -> void;
  auto rebuild_controls(const std::vector<std::size_t>& selected,
                        bool other_selected) -> void;
  auto sync_other_transition() -> void;
  [[nodiscard]] auto activates_result_control(const Event& event) const -> bool;
  auto ensure_multiple_visible() -> void;
  [[nodiscard]] auto current_description() const -> const std::string&;
  [[nodiscard]] auto has_descriptions() const -> bool;
  [[nodiscard]] auto selected_count() const -> std::size_t;
  [[nodiscard]] auto validate() -> bool;
  auto finish_submit() -> void;
  auto finish_cancel() -> void;

  ChoiceMode m_mode{ChoiceMode::Single};
  std::vector<ChoiceOption> m_choices;
  RadioGroup m_single;
  std::vector<std::unique_ptr<Checkbox>> m_multiple;
  Checkbox m_other_check{"Other"};
  TextInput m_other_input;
  Button m_submit{"[ Submit ]"};
  Button m_cancel{"[ Cancel ]"};
  std::function<void(std::optional<ChoiceResult>)> m_on_result;

  std::size_t m_minimum{0};
  std::optional<std::size_t> m_maximum;
  bool m_other_enabled{false};
  bool m_other_selected{false};
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

}  // namespace termforge
