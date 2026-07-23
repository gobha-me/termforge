// TermForge example: widgets
//
// Showcases all primitive widgets in a single app. Demonstrates the focus
// model (Tab cycles through widgets), MenuBar navigation, and live updates.
//
// Layout:
//   ┌─ TermForge Widgets ──────────────────────────────────────┐
//   │ File  Edit  View                          [MenuBar]      │
//   ├─ Left Frame ──────────┬─ Right Frame ───────────────────┤
//   │ Label                 │ ListWidget                      │
//   │ TextInput             │  item 1                         │
//   │ [Button] [Button]     │  item 2                         │
//   │ ProgressBar           │  ...                            │
//   ├─ Waveform ──────────────────────────────────────────────┤
//   │ (sine wave)                                             │
//   └─ Status bar ────────────────────────────────────────────┘
//
// Keyboard: Tab cycles focus, ESC quits.

#include <cmath>
#include <format>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using namespace termforge;

// Focus targets in tab order.
enum Focus { kMenu = 0, kInput, kBtnOk, kBtnCancel, kList, kFocusCount };

class WidgetsDemo final : public App {
 public:
  WidgetsDemo() : m_wave{256} {
    // Menu bar.
    m_menu.add_menu({"File",
                     {{"New", [this] { set_status("File > New"); }},
                      {"Open", [this] { set_status("File > Open"); }},
                      {"Save", [this] { set_status("File > Save"); }},
                      {"Quit", [this] { quit(); }}}});
    m_menu.add_menu({"Edit",
                     {{"Cut", [this] { set_status("Edit > Cut"); }},
                      {"Copy", [this] { set_status("Edit > Copy"); }},
                      {"Paste", [this] { set_status("Edit > Paste"); }}}});
    m_menu.add_menu({"View",
                     {{"Zoom In", [this] { set_status("View > Zoom In"); }},
                      {"Zoom Out", [this] { set_status("View > Zoom Out"); }},
                      {"Reset", [this] { set_status("View > Reset"); }}}});

    // Buttons.
    m_btn_ok.set_label("[ OK ]");
    m_btn_ok.on_activate([this] {
      set_status(std::format("OK pressed — input: \"{}\"", m_input.text()));
    });
    m_btn_cancel.set_label("[ Cancel ]");
    m_btn_cancel.on_activate([this] {
      m_input.set_text("");
      set_status("Cancel — input cleared");
    });

    // List.
    for (int i = 1; i <= 15; ++i)
      m_list.add_item(std::format("Item {:2} — selectable list entry", i));
    m_list.on_select([this](int idx, const std::string& text) {
      set_status(std::format("Selected: {} (index {})", text, idx));
    });

    // Input.
    m_input.set_placeholder("Type something...");
    m_input.on_change([this](const std::string& t) {
      set_status(std::format("Input: \"{}\"", t));
    });

    // Progress.
    m_progress.set_label("Loading...");

    set_status("Tab to cycle focus | ESC to quit");
  }

  auto on_event(const Event& ev) -> void override {
    // Menu dropdown consumes all keys when open.
    if (m_menu.dropdown_open()) {
      if (m_menu.on_event(ev)) return;
    }

    // Route to focused widget.
    bool consumed = false;
    switch (m_focus) {
      case kMenu: consumed = m_menu.on_event(ev); break;
      case kInput: consumed = m_input.on_event(ev); break;
      case kBtnOk: consumed = m_btn_ok.on_event(ev); break;
      case kBtnCancel: consumed = m_btn_cancel.on_event(ev); break;
      case kList: consumed = m_list.on_event(ev); break;
      default: break;
    }

    // Tab cycles forward, Shift+Tab cycles reverse.
    if (!consumed) {
      if (const auto* k = std::get_if<KeyEvent>(&ev)) {
        if (k->key == Key::Tab) {
          if (k->shift) {
            m_focus = (m_focus - 1 + kFocusCount) % kFocusCount;
          } else {
            m_focus = (m_focus + 1) % kFocusCount;
          }
          update_focus();
          return;
        }
      }
    }

    App::on_event(ev);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();

    const int W = screen.cols();
    const int H = screen.rows();

    // Simulate live data.
    const float t = static_cast<float>(m_frame) * 0.05f;
    m_wave.push(std::sin(t) * 0.5f + 0.5f);
    const auto pct = m_frame % 100;
    m_progress.set_value(static_cast<float>(pct) / 100.0f);
    m_progress.set_label(std::format("{}%", pct));

    // Layout.
    const int menu_h = 1;
    const int wave_h = 6;
    const int status_h = 1;
    const int content_h = H - menu_h - wave_h - status_h - 2;
    const int left_w = W / 2;
    const int right_w = W - left_w;

    // Left frame.
    m_left_frame.set_title(" Controls ");
    m_left_frame.set_geometry({0, menu_h, left_w, content_h});
    m_left_frame.draw(screen);
    const auto li = m_left_frame.content_rect();

    // Label.
    m_label.set_text("Widget Showcase");
    m_label.set_align(Label::Align::Center);
    m_label.set_colors(Rgb{0x00, 0xFF, 0xFF}, Rgb{0x0A, 0x0A, 0x14});
    m_label.set_geometry({li.x, li.y, li.w, 1});
    m_label.draw(screen);

    // TextInput.
    m_input.set_geometry({li.x + 1, li.y + 2, li.w - 2, 1});
    m_input.draw(screen);

    // Buttons (side by side).
    const int btn_w = (li.w - 4) / 2;
    m_btn_ok.set_geometry({li.x + 1, li.y + 4, btn_w, 1});
    m_btn_ok.draw(screen);
    m_btn_cancel.set_geometry({li.x + 3 + btn_w, li.y + 4, btn_w, 1});
    m_btn_cancel.draw(screen);

    // ProgressBar.
    m_progress.set_geometry({li.x + 1, li.y + 6, li.w - 2, 1});
    m_progress.draw(screen);

    // Right frame.
    m_right_frame.set_title(" ListWidget ");
    m_right_frame.set_geometry({left_w, menu_h, right_w, content_h});
    m_right_frame.draw(screen);
    m_list.set_geometry(m_right_frame.content_rect());
    m_list.draw(screen);

    // Waveform (full width).
    m_wave_frame.set_title(" Signal ");
    m_wave_frame.set_geometry({0, menu_h + content_h, W, wave_h + 1});
    m_wave_frame.draw(screen);
    m_wave.set_geometry(m_wave_frame.content_rect());
    m_wave.draw(screen);
    render_pixel_regions(m_wave);

    // Status bar.
    m_status.set_text(m_status_text);
    m_status.set_colors(Rgb{0x80, 0x80, 0x80}, Rgb{0x10, 0x10, 0x20});
    m_status.set_geometry({0, H - 1, W, 1});
    m_status.draw(screen);

    // Focus indicator in title.
    static const char* names[] = {"Menu", "Input", "OK", "Cancel", "List"};
    screen.write_text(W - 18, 0, std::format(" Focus: {:6}", names[m_focus]),
                      Rgb{0x60, 0x60, 0x80}, Rgb{0x20, 0x20, 0x40});

    // Menu bar drawn LAST so the dropdown overlays all other content.
    m_menu.set_geometry({0, 0, W, menu_h});
    m_menu.draw(screen);

    ++m_frame;
  }

 private:
  auto set_status(std::string msg) -> void { m_status_text = std::move(msg); }

  auto update_focus() -> void {
    m_input.set_focused(m_focus == kInput);
    m_btn_ok.set_focused(m_focus == kBtnOk);
    m_btn_cancel.set_focused(m_focus == kBtnCancel);
  }

  MenuBar m_menu;
  Frame m_left_frame, m_right_frame, m_wave_frame;
  Label m_label, m_status;
  TextInput m_input;
  Button m_btn_ok, m_btn_cancel;
  ProgressBar m_progress;
  ListWidget m_list;
  WaveformWidget m_wave;

  std::string m_status_text;
  int m_focus{0};
  int m_frame{0};
};

auto main() -> int {
  WidgetsDemo app;
  return app.run();
}
