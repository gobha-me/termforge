#pragma once

// TermForge — Checkbox: a labelled boolean toggle.
//
//   [x] Enable telemetry        [ ] Wrap long lines
//
// Space or a left click toggles; on_change(bool) fires with the new
// state. The mark comes from widgets/glyphs.hpp, so BorderStyle::Ascii turns
// (•) into (*) and ▾ into v across every control in the app at once — see
// examples/forms.cpp.
//
// set_checked() is the programmatic setter and does NOT fire the callback;
// toggle() is the user-level action and does. That split matches
// ListWidget::set_selected and TextInput::set_text: a setter that fired would
// make an app that syncs widget state from a model recurse through its own
// handler. For the same reason toggle() deliberately does NOT delegate to
// set_checked() (#42 item 4): toggle = flip + fire, set_checked = silent
// no-op-guarded set — they are different operations, and routing one through
// the other would either fire from a programmatic path or lose the fire.
//
// Focus is the whole-rect fg/bg swap Button uses, not a decoration on the mark
// -- one focus idiom across the widget set (see widget.hpp). Keys other than
// Space are declined: Tab reaches the FocusRing and cycles, and Enter falls
// through to Dialog's submit path instead of flipping the value (#39).

#include <functional>
#include <string>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class Checkbox final : public Widget {
 public:
  Checkbox() = default;
  explicit Checkbox(std::string label) : m_label(std::move(label)) {}

  auto set_label(std::string label) -> void {
    m_label = std::move(label);
    m_line.clear(); // invalidate the composed draw line (#42 item 5)
    mark_dirty();
  }
  [[nodiscard]] auto label() const noexcept -> const std::string& {
    return m_label;
  }

  [[nodiscard]] auto checked() const noexcept -> bool { return m_checked; }

  // Programmatic set — does not fire on_change (see the header note).
  auto set_checked(bool checked) -> void;

  // The user-level action: flips the state and fires on_change. This is what
  // the key and mouse paths call.
  auto toggle() -> void;

  // Mark family (default Single → Unicode marks). Ascii is the bare-TTY /
  // FallbackDriver choice — see widgets/glyphs.hpp.
  auto set_style(BorderStyle style) -> void {
    m_style = style;
    m_line.clear(); // glyphs change: invalidate the composed line
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  auto on_change(std::function<void(bool)> cb) -> void {
    m_on_change = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  // Columns the "[x] " chrome costs on top of the label. A parent sizing a
  // checkbox asks width_for() rather than repeating the 4 — the
  // Frame::kTitleChromeCols pattern, which exists because a duplicated
  // constant and its comment drift apart.
  static constexpr int kMarkCols = 4;
  [[nodiscard]] static constexpr auto width_for(int label_width) -> int {
    return label_width + kMarkCols;
  }

 private:
  std::string m_label;
  bool m_checked{false};
  // Composed "[x] label" line, rebuilt in draw() only when something it
  // depends on changed (empty = stale) -- the run loop renders ~10x/s and
  // the old per-frame composition + UTF-8 truncation scan was pure churn
  // (#42 item 5).
  std::string m_line;
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_focused_fg{theme::kFocusFg};
  Rgb m_focused_bg{theme::kFocusBg};

  std::function<void(bool)> m_on_change;
};

} // namespace termforge
