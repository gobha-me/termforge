#pragma once

// Exact signed-64-bit or finite-double numeric entry, with a separate draft.
// Parsing is ASCII/locale-free: optional sign; decimal dot/exponent only in
// decimal mode. No whitespace, hex, separators, non-finite values or suffixes.
// Invalid drafts remain editable. Enter commits; Escape restores the committed
// draft (clean Escape declines); Tab/blur never commit. Up/Down commit one step
// from a valid draft, refusing overflow/range violations without clamping.
// Programmatic configure/set_value are atomic and silent, replacing the draft
// only on success. User notifications occur only for changed committed values,
// copied and invoked last. Paste inserts raw bytes at the cursor, never
// commits. Raw control bytes display visibly/inertly with matching cursor/hit
// measurement. Draft bytes are bounded to INT_MAX/4 for escaped display.
// Error feedback is explicit and colorless; drawing/reset never changes
// content.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "termforge/widgets/text_input.hpp"

namespace termforge {
struct IntegerInputConfig {
  std::int64_t minimum{0}, maximum{100}, value{0}, step{1};
  auto operator==(const IntegerInputConfig&) const -> bool = default;
};
struct DecimalInputConfig {
  double minimum{0}, maximum{100}, value{0}, step{1};
  auto operator==(const DecimalInputConfig&) const -> bool = default;
};
using NumericInputConfig = std::variant<IntegerInputConfig, DecimalInputConfig>;
using NumericValue = std::variant<std::int64_t, double>;

class NumericInput final : public Widget {
 public:
  NumericInput();
  [[nodiscard]] auto configure(NumericInputConfig config)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto configuration() const -> NumericInputConfig {
    return m_config;
  }
  [[nodiscard]] auto value() const -> NumericValue;
  [[nodiscard]] auto set_value(NumericValue value)
      -> std::expected<void, ErrorEvent>;
  // Invalid numeric text is allowed; failure is only an editor-size refusal.
  [[nodiscard]] auto set_draft(std::string draft)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto draft() const noexcept -> const std::string& {
    return m_editor.text();
  }
  [[nodiscard]] auto draft_value() const
      -> std::expected<NumericValue, ErrorEvent>;
  [[nodiscard]] auto cursor_pos() const noexcept -> int {
    return m_editor.cursor_pos();
  }
  // Last edit/commit/step refusal, cleared by an accepted model update,
  // successful commit, cancellation or a valid draft edit. Borrowed state.
  [[nodiscard]] auto error() const noexcept
      -> const std::optional<ErrorEvent>& {
    return m_error;
  }
  [[nodiscard]] auto commit() -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto step_up() -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto step_down() -> std::expected<void, ErrorEvent>;
  auto cancel() -> void;
  auto set_label(std::string label) -> void;
  auto on_change(std::function<void(NumericValue)> callback) -> void {
    m_on_change = std::move(callback);
  }
  auto set_focused(bool focus) -> void override;
  [[nodiscard]] auto editor_rect() const noexcept -> Rect;
  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;

 private:
  [[nodiscard]] auto check_value(NumericValue candidate) const
      -> std::expected<void, ErrorEvent>;
  auto assign_value(NumericValue candidate, bool notify) -> void;
  auto refresh_error() -> void;
  auto refuse(ErrorEvent error) -> std::expected<void, ErrorEvent>;
  auto step(bool up) -> std::expected<void, ErrorEvent>;
  auto paste(const std::string& text) -> std::expected<void, ErrorEvent>;
  NumericInputConfig m_config{IntegerInputConfig{}};
  // Deliberately no callbacks on this private editor. We can observe its
  // handled edits after it returns; only our commit/step may call app code.
  TextInput m_editor;
  std::string m_label;
  std::optional<ErrorEvent> m_error;
  std::function<void(NumericValue)> m_on_change;
};
} // namespace termforge
