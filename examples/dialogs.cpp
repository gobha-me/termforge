// TermForge example: dialogs
//
// The modal overlay stack and the three standard dialogs. Three buttons raise
// a MessageDialog, a ConfirmDialog and a PromptDialog; a status line reports
// what each one returned. While a dialog is up the buttons underneath are
// dead to both the keyboard and the mouse, and the background is dimmed —
// that is the whole point of push_overlay().
//
// Note how little wiring a dialog needs: construct it with its callback, tell
// it what closing means (pop_overlay), and push it. Escape is the dialog's,
// not the app's, for as long as it is on the stack.
//
// Layout:
//   ┌┤ TermForge Dialogs ├────────────────────────────────────┐
//   │ [ Message ]  [ Confirm ]  [ Prompt ]                    │
//   │                                                         │
//   │ last result: ...                  ╔╣ Confirm ╠════════╗ │
//   │                                   ║ Delete file?      ║ │
//   │                                   ║      [ Yes ] [ No ]║ │
//   │                                   ╚════════════════════╝ │
//   └─────────────────────────────────────────────────────────┘
//
// The confirm dialog uses BorderStyle::Double and the prompt BorderStyle::Ascii
// to show that a dialog's border family is settable even though the Frame is a
// private member (see set_border_style in the constructor).
//
// Keyboard: Tab cycles focus, Enter/Space activates, ESC quits (or cancels
// the dialog, when one is open). Inside a confirm: Y/N are hotkeys.

#include <string>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"

using namespace termforge;

class DialogsDemo final : public App {
 public:
  DialogsDemo() {
    m_btn_message.set_label("[ Message ]");
    m_btn_confirm.set_label("[ Confirm ]");
    m_btn_prompt.set_label("[ Prompt ]");

    m_btn_message.on_activate([this] { show(m_message); });
    m_btn_confirm.on_activate([this] { show(m_confirm); });
    m_btn_prompt.on_activate([this] { show(m_prompt); });

    m_ring.add(&m_btn_message);
    m_ring.add(&m_btn_confirm);
    m_ring.add(&m_btn_prompt);

    // Every dialog closes the same way: drop it off the overlay stack. The
    // dialog itself knows nothing about App.
    m_message.on_close([this] { pop_overlay(); });
    m_confirm.on_close([this] { pop_overlay(); });
    m_prompt.on_close([this] { pop_overlay(); });

    m_message.on_ok([this] { m_status.set_text("message: acknowledged"); });
    m_confirm.on_result([this](bool yes) {
      m_status.set_text(yes ? "confirm: deleted" : "confirm: kept");
    });
    m_prompt.on_submit([this](std::string value) {
      m_status.set_text("prompt: \"" + value + "\"");
    });
    m_prompt.on_cancel([this] { m_status.set_text("prompt: cancelled"); });
    m_prompt.set_placeholder("untitled.txt");

    // A dialog's border is stylable even though it owns its Frame privately.
    // Ascii is the one that matters: on the FallbackDriver tier a box-drawing
    // border is mojibake, and a modal is the worst place for that.
    m_confirm.set_border_style(BorderStyle::Double);
    m_prompt.set_border_style(BorderStyle::Ascii);

    m_status.set_text("last result: (nothing yet)");
  }

  auto on_event(const Event& ev) -> void override {
    // Nothing here runs while a dialog is up — App::dispatch_event routes
    // everything to the overlay instead. No modal guard needed.
    if (const auto* m = std::get_if<MouseEvent>(&ev)) {
      if (m->pressed) m_ring.focus_at(m->x, m->y);
      route_mouse(*m, {&m_btn_message, &m_btn_confirm, &m_btn_prompt});
      return;
    }
    if (m_ring.handle_key(ev)) return;
    App::on_event(ev);  // ESC / Ctrl+C quits
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int w = screen.cols(), h = screen.rows();

    m_frame.set_geometry({0, 0, w, h});
    m_frame.draw(screen);
    const Rect inner = m_frame.content_rect();

    int x = inner.x + 1;
    for (auto* b : {&m_btn_message, &m_btn_confirm, &m_btn_prompt}) {
      const int bw = static_cast<int>(b->label().size()) + 1;
      b->set_geometry({x, inner.y + 1, bw, 1});
      b->draw(screen);
      x += bw + 2;
    }

    m_status.set_geometry({inner.x + 1, inner.y + 3, inner.w - 2, 1});
    m_status.draw(screen);

    // Dialogs are NOT drawn here — the overlay pass draws them after this
    // returns, so they land on top of everything above.
  }

 private:
  auto show(Dialog& dialog) -> void { push_overlay(dialog); }

  Frame m_frame{"TermForge Dialogs"};
  Button m_btn_message, m_btn_confirm, m_btn_prompt;
  Label m_status;
  FocusRing m_ring;

  MessageDialog m_message{"Message", "Your work has been saved."};
  ConfirmDialog m_confirm{"Confirm", "Delete this file? This cannot be undone.",
                          {}};
  PromptDialog m_prompt{"Prompt", "Name the new file:", {}};
};

auto main() -> int {
  DialogsDemo app;
  return app.run();
}
