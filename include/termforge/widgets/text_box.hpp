#pragma once

// TermForge — TextBox: a scrolling multi-line text area (chat-scrollback
// style). Lines are appended; the view shows the most recent lines that fit
// its rect, auto-scrolling to the bottom on new content unless the user has
// scrolled up. Supports manual scroll (PageUp/PageDown / scroll wheel) and
// display-width-aware word wrapping across styled spans (#24/#25), with hard
// wrapping only for an unbroken run wider than the widget. This is the
// foundation of a chat message view.

#include <string>
#include <vector>

#include "termforge/core/styled_text.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class TextBox final : public Widget {
 public:
  TextBox() = default;

  // Append a logical line (a chat message, a log entry). Long lines wrap at
  // the last fitting space, falling back to a display-width-safe hard split
  // for an overlong word. Source whitespace is preserved. Marks the widget
  // dirty and auto-scrolls to the bottom if the user is already at the bottom.
  // Plain text becomes one default-colour span; sanitization runs here (#25).
  auto append(std::string line) -> void;

  // Append a styled logical line. Each span's text is sanitized at this
  // boundary (styles are data, never escape codes). Empty spans are retained
  // in the document but paint nothing.
  auto append(StyledText line) -> void;

  // Replace all content.
  auto clear() -> void;

  // Scroll the view. positive = toward newer (down), negative = older (up).
  auto scroll(int delta) -> void;
  auto scroll_to_bottom() -> void;

  // Event handling: PageUp/PageDown scroll a page; scroll wheel scrolls.
  auto on_event(const Event& ev) -> bool override;

  auto draw(Screen& screen) -> void override;

  [[nodiscard]] auto line_count() const noexcept -> std::size_t { return m_lines.size(); }
  [[nodiscard]] auto at_bottom() const noexcept -> bool;

  // Which glyph family #21's scrollbar comes from. TextBox has no other
  // glyph need, so this knob exists purely for the bar: an app holding one
  // BorderStyle passes it here too, and BorderStyle::Ascii is what keeps the
  // strip 7-bit on a bare TTY. Same convention as ListWidget/TableWidget.
  auto set_style(BorderStyle style) -> void {
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  // Scrollbar colours (#21): the │ track and the █ thumb.
  auto set_scrollbar_colors(Rgb track_fg, Rgb thumb_fg) -> void {
    m_track_fg = track_fg;
    m_thumb_fg = thumb_fg;
    mark_dirty();
  }

  // The width text wraps and paints at: the rect width minus the column #21's
  // scrollbar claims when the content overflows. Whether the bar is up is
  // decided at draw time (only then is the wrapped row count known), so this
  // is the draw-loop's width -- an app laying out against it should treat it
  // as advisory, like the wrap itself. Floored at 0 (never negative): the
  // wrap helper requires a positive width and the narrow-rect draw guards
  // before calling it.
  [[nodiscard]] auto content_w() const noexcept -> int;

 private:
  // Wrap a styled logical line to `width` columns, appending visual rows.
  static auto wrap_into(std::vector<StyledText>& out, const StyledText& line,
                        int width) -> void;

  std::vector<StyledText> m_lines;  // logical (unwrapped) lines
  int m_scroll{0};                  // 0 = pinned to bottom; >0 = lines scrolled up
  bool m_follow{true};              // auto-scroll to bottom on new content

  // #21: the scrollbar strip's family and colours. Default colours mirror
  // the list/table: dim track, selection-blue thumb.
  BorderStyle m_style{BorderStyle::Single};
  Rgb m_track_fg{theme::kDim};
  Rgb m_thumb_fg{theme::kFocusBg};
};

}  // namespace termforge
