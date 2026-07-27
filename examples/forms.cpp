// TermForge example: forms
//
// The three form controls from #19 — Checkbox, RadioGroup and Select — wired
// into one FocusRing alongside Buttons, which is what a settings dialog
// actually looks like.
//
// Layout:
//   ┌┤ Settings ├───────────────────────────┐
//   │ [x] Enable telemetry                  │
//   │ [ ] Wrap long lines                   │
//   │                                       │
//   │ Theme                                 │
//   │ (•) Dark                              │
//   │ ( ) Light                             │
//   │ ( ) High contrast                     │
//   │                                       │
//   │ Driver   [ ansi-rgb    ▾ ]            │
//   │                                       │
//   │ [   OK   ]  [ Cancel ]                │
//   └───────────────────────────────────────┘
//   Last change: ...
//
// Keyboard: Tab / Shift+Tab cycle, Space toggles a checkbox, arrows move
// within the radio group, Enter opens and commits the Select, Escape closes it
// (or quits when nothing is open), F1 cycles the border/mark style.
//
// F1 is the ASCII-fallback demo and the "how do I style a whole app" answer:
// there is no global default style in the library on purpose
// (widgets/glyphs.hpp), so the app holds one BorderStyle and hands it to every
// frame AND every control — the same enum drives the box-drawing family and
// the (•)/(*) and ▾/v marks. F1 to ASCII is what an app on the FallbackDriver
// tier (a bare TTY with no box drawing in its font) wants.

#include <format>
#include <string>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/select.hpp"

using namespace termforge;

class FormsDemo final : public App {
 public:
  FormsDemo() {
    m_telemetry.set_label("Enable telemetry");
    m_telemetry.set_checked(true);
    m_telemetry.on_change([this](bool on) {
      set_status(std::format("Telemetry: {}", on ? "on" : "off"));
    });

    m_wrap.set_label("Wrap long lines");
    m_wrap.on_change([this](bool on) {
      set_status(std::format("Wrap: {}", on ? "on" : "off"));
    });

    m_theme.set_options({"Dark", "Light", "High contrast"});
    m_theme.on_change([this](int i) {
      set_status(std::format("Theme: {} ({})", m_theme.selected_text(), i));
    });

    m_driver.set_options({"kitty", "ansi-rgb", "fallback"});
    m_driver.set_selected(1);
    m_driver.on_change([this](int, const std::string& name) {
      set_status(std::format("Driver: {}", name));
    });

    m_ok.set_label("OK");
    m_ok.on_activate([this] { set_status("OK"); });
    m_cancel.set_label("Cancel");
    m_cancel.on_activate([this] { set_status("Cancel"); });

    // Constant presentation state lives here, not in on_render (#42 item 5):
    // the frame title and the section labels never change, and the render
    // path used to re-run these setters ~10x/second for nothing. The status
    // bar's colors are constant too -- only its text varies (set_status).
    m_frame.set_title("Settings");
    m_theme_label.set_text("Theme");
    m_theme_label.set_colors(Rgb{0x80, 0x80, 0xA0}, Rgb{0x0A, 0x0A, 0x14});
    m_driver_label.set_text("Driver");
    m_driver_label.set_colors(Rgb{0x80, 0x80, 0xA0}, Rgb{0x0A, 0x0A, 0x14});
    m_status.set_colors(Rgb{0x80, 0x80, 0x80}, Rgb{0x10, 0x10, 0x20});

    // Focus order = mouse-routing order below. The Select is LAST in both, so
    // its open dropdown wins over the widgets it overlays.
    m_ring.add(&m_telemetry);
    m_ring.add(&m_wrap);
    m_ring.add(&m_theme);
    m_ring.add(&m_ok);
    m_ring.add(&m_cancel);
    m_ring.add(&m_driver);

    set_status("Tab cycles | Space toggles | F1 border style | ESC quits");
  }

  auto on_event(const Event& ev) -> void override {
    if (const auto* m = std::get_if<MouseEvent>(&ev)) {
      // 1. A press that lands on nothing still needs an explicit close: focus
      //    only moves when the click hits a ring member, so click-away on the
      //    background would otherwise leave the dropdown open.
      if (m->pressed && m_driver.dropdown_open() &&
          !m_driver.hit_test(m->x, m->y))
        m_driver.close_dropdown();

      // 2. The open dropdown must win the hit. focus_at and route_mouse both
      //    iterate last-added-first, so listing the Select last is enough
      //    here; this pre-route is the general answer for an app whose
      //    z-order does not line up that way.
      if (m_driver.dropdown_open() && m_driver.hit_test(m->x, m->y)) {
        m_driver.on_event(ev);
        return;
      }

      if (m->pressed) m_ring.focus_at(m->x, m->y);
      route_mouse(*m, {&m_telemetry, &m_wrap, &m_theme, &m_ok, &m_cancel,
                       &m_driver});
      return;
    }

    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      if (k->key == Key::F1) {
        cycle_style();
        return;
      }
    }

    // The ring hands the key to the focused control and cycles on Tab. An open
    // Select closes and declines Tab, so one press both closes and moves on.
    if (m_ring.handle_key(ev)) return;

    App::on_event(ev);  // Escape quits
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();

    const int W = screen.cols();
    const int H = screen.rows();
    const int form_w = std::min(W, 46);

    m_frame.set_geometry({0, 0, form_w, H - 1});
    m_frame.draw(screen);
    const auto c = m_frame.content_rect();

    int y = c.y + 1;
    m_telemetry.set_geometry({c.x + 1, y++, c.w - 2, 1});
    m_telemetry.draw(screen);
    m_wrap.set_geometry({c.x + 1, y++, c.w - 2, 1});
    m_wrap.draw(screen);

    ++y;
    m_theme_label.set_geometry({c.x + 1, y++, c.w - 2, 1});
    m_theme_label.draw(screen);
    m_theme.set_geometry(
        {c.x + 1, y, c.w - 2, static_cast<int>(m_theme.option_count())});
    m_theme.draw(screen);
    y += static_cast<int>(m_theme.option_count()) + 1;

    m_driver_label.set_geometry({c.x + 1, y, 8, 1});
    m_driver_label.draw(screen);
    // Geometry now, draw at the very end: the open dropdown has to land on top
    // of the buttons and the status bar below it.
    // width_for() rather than a hardcoded number: the control owns its chrome.
    m_driver.set_geometry({c.x + 10, y, Select::width_for(10), 1});
    y += 2;

    m_ok.set_geometry({c.x + 1, y, 10, 1});
    m_ok.draw(screen);
    m_cancel.set_geometry({c.x + 13, y, 10, 1});
    m_cancel.draw(screen);

    m_status.set_text(m_status_text);
    m_status.set_geometry({0, H - 1, W, 1});
    m_status.draw(screen);

    // The Select is drawn LAST so its open dropdown overlays the buttons and
    // the status bar, matching the routing order in on_event.
    m_driver.draw(screen);
  }

 private:
  auto set_status(std::string msg) -> void { m_status_text = std::move(msg); }

  // One style for the whole app — the frame AND every control. The same
  // BorderStyle enum picks the box-drawing family and the mark family, which
  // is why #19's glyphs went into glyphs.hpp next to #20's instead of getting
  // an enum of their own.
  auto cycle_style() -> void {
    static constexpr BorderStyle kOrder[] = {
        BorderStyle::Single, BorderStyle::Double, BorderStyle::Rounded,
        BorderStyle::Heavy, BorderStyle::Ascii};
    static constexpr const char* kNames[] = {"Single", "Double", "Rounded",
                                             "Heavy", "ASCII (bare-TTY tier)"};
    m_style_index = (m_style_index + 1) % 5;
    const BorderStyle style = kOrder[m_style_index];
    m_frame.set_style(style);
    m_telemetry.set_style(style);
    m_wrap.set_style(style);
    m_theme.set_style(style);
    m_driver.set_style(style);
    set_status(std::format("Style: {}", kNames[m_style_index]));
  }

  Frame m_frame;
  Label m_theme_label, m_driver_label, m_status;
  Checkbox m_telemetry, m_wrap;
  RadioGroup m_theme;
  Select m_driver;
  Button m_ok, m_cancel;
  FocusRing m_ring;

  std::string m_status_text;
  int m_style_index{0};
};

auto main() -> int {
  FormsDemo app;
  return app.run();
}
