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
// handler.
//
// Focus is the whole-rect fg/bg swap Button uses, not a decoration on the mark
// -- one focus idiom across the widget set (see widget.hpp). Keys other than
// Space are declined: Tab reaches the FocusRing and cycles, and Enter falls
// through to Dialog's submit path instead of flipping the value (#39).

#include <functional>
#include <string>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class Checkbox final : public Widget {
 public:
  Checkbox() = default;
  explicit Checkbox(std::string label) : m_label(std::move(label)) {}

  auto set_label(std::string label) -> void {
    m_label = std::move(label);
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
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  Rgb m_focused_fg{0x0A, 0x0A, 0x14};
  Rgb m_focused_bg{0x40, 0x80, 0xFF};

  std::function<void(bool)> m_on_change;
};

}  // namespace termforge
