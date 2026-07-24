#pragma once

// TermForge — Dialog: the base for a centered, self-sizing modal panel.
//
// A dialog is a Frame, a body of wrapped text, and a row of controls that own
// their own Tab order. Dialog assembles those three and leaves the specifics
// to subclasses (see dialogs.hpp for Message/Confirm/Prompt).
//
// It solves the three problems every dialog has:
//
//   * SIZE. Nothing tells a dialog how big it is; it works it out from its
//     content — the title, the wrapped body, and the subclass's control row —
//     measured in display columns (not bytes, see issue #10), clamped to
//     max_width and to the screen.
//   * POSITION. layout() re-centers from the Screen's current dimensions on
//     every draw(). A dialog therefore survives a resize with no wiring, and
//     never asks App for a Screen it may not have yet.
//   * CLOSING. Dialog does not know about App — widgets/ must not depend on
//     core/app.hpp. It calls the on_close callback and the app decides what
//     that means, which in practice is one line:
//
//       m_confirm.on_close([this] { pop_overlay(); });
//       push_overlay(m_confirm);
//
// Input arrives already filtered: App gives the top overlay every key, so a
// dialog's Escape is the dialog's Escape (it never reaches App::on_event's
// default quit). Inside, keys go to the dialog's own FocusRing, so Tab cycles
// the controls and cannot escape the modal.
//
// Note the layering rule this inherits from push_overlay: the app owns the
// dialog object. A callback must not destroy the dialog it was invoked from —
// pop the overlay (which only drops a pointer) and destroy later if you must.

#include <functional>
#include <string>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class Dialog : public Widget {
 public:
  Dialog() = default;
  explicit Dialog(std::string title);

  // Not copyable or movable: a Dialog holds Widget* to its OWN members (in
  // its child list and its focus ring) and its controls' callbacks capture
  // `this`. A copy or move would leave every one of those pointing at the
  // original — so a Dialog in a std::vector that reallocates would dispatch
  // clicks into freed memory. Hold dialogs by reference or unique_ptr.
  Dialog(const Dialog&) = delete;
  auto operator=(const Dialog&) -> Dialog& = delete;
  Dialog(Dialog&&) = delete;
  auto operator=(Dialog&&) -> Dialog& = delete;

  auto set_title(std::string title) -> void;
  [[nodiscard]] auto title() const noexcept -> const std::string& {
    return m_title;
  }

  // Body text. Wrapped to the dialog's inner width; '\n' is a hard break.
  auto set_text(std::string text) -> void;
  [[nodiscard]] auto text() const noexcept -> const std::string& {
    return m_text;
  }

  // Upper bound on the inner (inside-the-border) width, in columns. The
  // dialog may be narrower if its content is; it is always clamped to the
  // screen. Default 48 — wide enough for a sentence, narrow enough to read.
  auto set_max_width(int cols) -> void;

  // Border family for the dialog's frame (default Single). Called
  // set_border_style, not set_style, because a dialog has more than a border to
  // style; it forwards to the Frame the dialog owns privately, which is
  // otherwise unreachable — without this no dialog could ever be ASCII, which
  // is the tier that needs it most (widgets/glyphs.hpp). The dialog's *size*
  // does not depend on the style: every family's glyphs are one column wide.
  auto set_border_style(BorderStyle style) -> void;
  [[nodiscard]] auto border_style() const noexcept -> BorderStyle;

  // Fired when the dialog is finished. The app wires this to pop_overlay().
  auto on_close(std::function<void()> cb) -> void;

  // Size and center for a screen of these dimensions. draw() calls this every
  // frame, so apps normally never do. Call it manually only when you need a
  // real rect() before the first frame — e.g. to hit-test a mouse event that
  // arrives in the same input batch that pushed the dialog.
  auto layout(int screen_cols, int screen_rows) -> void;

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

 protected:
  // Register a control: it is drawn by the subclass and, unless tab_stop is
  // false, joins the dialog's focus ring in call order (so the first one
  // added starts focused). Non-owning, like every other widget list.
  //
  // tab_stop == false means "clicks route here, keys never do". Such a child
  // must not focus itself on click (TextInput does — so it always wants to be
  // a tab stop), or it and the ring's member would both render as focused
  // with no way for the ring to repair it.
  auto add_child(Widget* w, bool tab_stop = true) -> void;

  // A result may be reported once per showing. Returns false if this showing
  // has already reported one — a mouse press and an Enter can arrive in the
  // same input batch, and a confirm must not fire twice. The latch clears on
  // the next draw(), because a dialog that reported a result closed and was
  // popped, so the next frame that draws it is a new showing.
  auto begin_result() -> bool;

  // Extra size the subclass's controls need, inside the border and below the
  // body text. Rows > 0 also buys a blank spacer row above the controls.
  [[nodiscard]] virtual auto content_rows() const -> int { return 0; }
  [[nodiscard]] virtual auto content_cols() const -> int { return 0; }

  // Place the controls. `area` is the region under the body text, inside the
  // border. Called from layout() every frame.
  virtual auto layout_content(Rect /*area*/) -> void {}
  // Draw the controls, after the frame and body are on screen.
  virtual auto draw_content(Screen& /*screen*/) -> void {}

  // What Escape means. The base closes; Confirm/Prompt report a cancel first.
  virtual auto on_escape() -> void { close(); }

  // Fire on_close (copying the callback first — a callback may replace the
  // one it was called from; see issue #5).
  auto close() -> void;

  [[nodiscard]] auto ring() -> FocusRing& { return m_ring; }
  // The body text as wrapped by the last layout(), one entry per screen row.
  [[nodiscard]] auto body_lines() const noexcept
      -> const std::vector<std::string>& {
    return m_lines;
  }

  [[nodiscard]] auto fg() const noexcept -> Rgb { return m_fg; }
  [[nodiscard]] auto bg() const noexcept -> Rgb { return m_bg; }

 private:
  std::string m_title;
  std::string m_text;
  std::vector<std::string> m_lines;  // wrapped body, rebuilt by layout()
  Frame m_frame;
  FocusRing m_ring;
  std::vector<Widget*> m_children;
  Rect m_content_area;      // where the subclass's controls went, or h == 0
  bool m_reported{false};   // see begin_result
  int m_max_width{48};
  // Must match Frame's hardcoded background, or the border row and the
  // interior disagree. There is no Theme type yet to hold this.
  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  std::function<void()> m_on_close;
};

}  // namespace termforge
