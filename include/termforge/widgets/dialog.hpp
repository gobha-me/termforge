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
// default quit). Mouse delivery is gated on hit_test_tree(): the dialog's
// rect plus its children's hit areas, so a Select's open dropdown painted
// below the bottom border still takes clicks (issue #37). Inside, keys go to
// the dialog's own FocusRing FIRST, so Tab cycles the controls and cannot
// escape the modal — and a focused control with a transient sub-state
// (Select's open dropdown) gets first refusal on Escape before it means
// "cancel the dialog" (issue #33). Mouse events inside are pre-routed to a
// child whose rect-exceeding hit area owns the point (the same #37 case: a
// dropdown row overlapping the button row commits the option, not the button
// underneath) -- for presses, the wheel, and motion alike (#47), so an open
// dropdown scrolls and hover-highlights instead of the control beneath it. A
// press that lands on no child (the dialog's chrome) closes any open child
// dropdown (#47 item 3).
//
// Note the layering rule this inherits from push_overlay: the app owns the
// dialog object. A callback must not destroy the dialog it was invoked from —
// pop the overlay (which only drops a pointer) and destroy later if you must.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

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

  // Forwards the tick to every child, so a control inside a dialog animates
  // (#69). The app still has to tick the DIALOG — App ticks nothing by itself,
  // and an overlay is no exception. Only worth doing while the dialog is up,
  // and only if it holds something that animates: the standard dialogs hold
  // Buttons and TextInputs, which need no ticks at all (see reset_transient
  // below for why the press flash is not a reason).
  auto on_tick(std::chrono::duration<double> dt) -> void override;

  // Forwards to every child at the per-showing boundary, so a dialog re-opens
  // clean (#122). This is what closed the one failure of #69's design that was
  // not obvious on first use: a dialog's button closes the dialog on
  // activation, so the flash is armed and the overlay popped in the same
  // dispatch — it never renders, and before this it stayed lit into the next
  // showing unless the app kept ticking a dialog that was no longer pushed.
  //
  // Same family as on_tick and hit_test_tree: forward order, every child,
  // recursive. Fired from draw(), BEFORE on_show() and before layout(), so an
  // override may not trust rect(). An override that forwards further (the
  // shape FilePickerDialog uses for on_tick) must call
  // Dialog::reset_transient() or its children stop being reset.
  auto reset_transient() -> void override;

  // Covers the dialog's rect plus every child's hit area -- including a
  // Select's open dropdown, which paints below the dialog's bottom border
  // (#37). App's overlay dispatch gates mouse delivery on this, so without
  // it those rendered rows are dead or, worse, dialog-dismissing.
  [[nodiscard]] auto hit_test_tree(int px, int py) const -> bool override;

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

  // Drop every registered child and focus-ring member. Composite dialogs
  // whose controls are configured at runtime use this before destroying or
  // rebuilding those controls; without it the dialog would retain dangling
  // Widget* entries. Existing child objects are not owned or destroyed.
  auto clear_children() -> void;

  // A result may be reported once per showing. Returns false if this showing
  // has already reported one — a mouse press and an Enter can arrive in the
  // same input batch, and a confirm must not fire twice. The latch clears on
  // the next draw(), and that same transition (latched -> cleared) is what
  // fires on_show(): a dialog that reported a result closed and was popped,
  // so the next frame that draws it is a new showing.
  //
  // So calling this ENDS THE SHOWING, whether or not you also close(): the
  // next draw clears the latch, resets every child's transient state (#122)
  // and fires on_show(). A control that acts without finishing the dialog --
  // an "Apply" that stays up -- must NOT call this, or it re-runs the dialog's
  // per-showing work and wipes its own press flash before it can render.
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

  // Per-SHOWING hook, fired from draw() on the first frame of each showing --
  // the first draw() after the result latch was armed by a close (plus the
  // very first showing). Per-showing work belongs here (re-read a directory,
  // seed a field, assert a starting focus): draw() itself runs EVERY frame
  // (~10 Hz idle), so work placed there repeats or, worse, fights the user --
  // a refresh that resets a list's selection every frame makes navigation
  // impossible (issue #45). The base does nothing. Runs AFTER the boundary's
  // reset_transient() pass, so whatever it establishes survives.
  virtual auto on_show() -> void {}

  // Fire on_close (copying the callback first — a callback may replace the
  // one it was called from; see issue #5).
  auto close() -> void;

  [[nodiscard]] auto ring() -> FocusRing& { return m_ring; }
  [[nodiscard]] auto ring() const -> const FocusRing& { return m_ring; }
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
  bool m_shown_once{false}; // has any showing completed its first frame yet
  int m_max_width{48};
  // Must match Frame's hardcoded background, or the border row and the
  // interior disagree. There is no Theme type yet to hold this.
  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  std::function<void()> m_on_close;
};

}  // namespace termforge
