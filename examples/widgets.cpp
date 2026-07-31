// TermForge example: widgets
//
// Showcases all primitive widgets in a single app. Demonstrates the focus
// model (Tab cycles through widgets), MenuBar navigation, live updates, and the
// five border families (Border menu).
//
// Layout:
//   File  Edit  View  Border                        [MenuBar]
//   ┌┤ Controls ├───────────┐┌┤ Views ├─────────────────────────┐
//   │ Label                 ││ ▸All Items  Even  Odd  Reversed ›│  [TabBar]
//   │ TextInput             ││  item 1                         │
//   │ [Button] [Button]     ││  item 2                         │
//   │ ProgressBar           ││  ...                            │
//   └───────────────────────┘└─────────────────────────────────┘
//   ┌┤ Signal ├──────────────────────────────────────────────────┐
//   │ (sine wave)                                                │
//   └────────────────────────────────────────────────────────────┘
//   Status bar
//
// Keyboard: Tab cycles focus, ESC quits. View > Progress mode swaps the bar
// between a determinate percentage and an indeterminate pulse.
//
// It is also the reference for the tick split at widget level (#69): the demo
// data is advanced in on_tick against the wall clock, and the widgets that
// animate themselves are handed the same dt via tick_widgets. Nothing here
// counts frames, so the bar and the wave run at the same speed whatever the
// frame budget is.
//
// The TabBar (#22) shows the division of labour that widget insists on: it owns
// the strip and reports an index, and repopulating the list below it is the
// app's job (apply_view). Five tabs need more columns than half an 80-column
// terminal, so it is also where the overflow indicators are visible — narrow
// the window and the strip scrolls with ‹ ›, the wheel, or Left/Right.
//
// The Border menu is also the answer to "how do I style a whole app?": there is
// no global default style — the app holds one BorderStyle and hands it to each
// frame (see set_border below). Border > ASCII is what an app on the
// FallbackDriver tier wants.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/tab_bar.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using namespace termforge;

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
                      {"Reset", [this] { set_status("View > Reset"); }},
                      {"Progress mode", [this] { toggle_progress_mode(); }}}});
    m_menu.add_menu(
        {"Border",
         {{"Single", [this] { set_border(BorderStyle::Single, "Single"); }},
          {"Double", [this] { set_border(BorderStyle::Double, "Double"); }},
          {"Rounded", [this] { set_border(BorderStyle::Rounded, "Rounded"); }},
          {"Heavy", [this] { set_border(BorderStyle::Heavy, "Heavy"); }},
          {"ASCII", [this] {
             set_border(BorderStyle::Ascii, "ASCII (bare-TTY tier)");
           }}}});

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

    // Tabs over the list pane. Populated BEFORE m_ring.add below: focusable()
    // is dynamic on the tab count, and FocusRing::add only grants initial focus
    // to a member that is focusable at add time.
    m_tabs.set_tabs({"All Items", "Even", "Odd", "Reversed", "Empty"});
    m_tabs.on_change([this](int view) {
      apply_view(view);
      set_status(std::format("View: {}", m_tabs.title(view)));
    });

    // List. The TabBar owns the strip and nothing else — filling the pane under
    // it is this app's job, which is the whole point of #22's scope.
    apply_view(0);
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

    // Focus order. Same order as route_mouse below so a click and a Tab agree
    // on which widget is "topmost"; the ring owns focus and Tab-cycling, and a
    // click on any member moves focus to it (focus_at). Input takes initial
    // focus (the first focusable member added).
    m_ring.add(&m_input);
    m_ring.add(&m_btn_ok);
    m_ring.add(&m_btn_cancel);
    m_ring.add(&m_tabs);
    m_ring.add(&m_list);
    m_ring.add(&m_menu);

    set_status("Tab to cycle focus | ESC to quit");
  }

  auto on_event(const Event& ev) -> void override {
    // Mouse events: a press moves focus to the widget under the cursor
    // (focus_at) and routes the click. The menu is listed last (= topmost) in
    // both the ring and route_mouse so its open dropdown wins over the widgets
    // it overlays. A press outside the menu closes the dropdown first.
    if (const auto* m = std::get_if<MouseEvent>(&ev)) {
      if (m->pressed && m_menu.dropdown_open() && !m_menu.hit_test(m->x, m->y))
        m_menu.close_dropdown();
      if (m->pressed) m_ring.focus_at(m->x, m->y);
      route_mouse(*m, {&m_input, &m_btn_ok, &m_btn_cancel, &m_tabs, &m_list,
                       &m_menu});
      return;
    }

    // Menu dropdown consumes all keys when open.
    if (m_menu.dropdown_open()) {
      if (m_menu.on_event(ev)) return;
    }

    // The ring delivers the key to the focused widget and cycles on Tab /
    // Shift+Tab.
    if (m_ring.handle_key(ev)) return;

    App::on_event(ev);
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override {
    m_elapsed += dt.count();

    // Simulate live data. Both rates are per SECOND — 1.5 rad/s for the wave
    // and 30%/s for the bar, which is what the old frame counter happened to
    // produce at the default 33ms budget, now stated in the units it meant.
    // (One sample per tick, not per frame: identical here, since this app
    // leaves set_tick_hz at its variable-timestep default.)
    m_wave.push(std::sin(m_elapsed * 1.5) * 0.5f + 0.5f);
    if (!m_progress.indeterminate()) {
      // Reduce before the cast, not after: m_elapsed grows without bound, and
      // double -> int is UB once it passes INT_MAX. Same rule the library
      // follows for the pulse.
      const auto pct = static_cast<int>(std::fmod(m_elapsed * 30.0, 100.0));
      m_progress.set_value(static_cast<float>(pct) / 100.0f);
      m_progress.set_label(std::format("{}%", pct));
    }

    // The widgets that animate themselves get the same dt. App keeps no
    // registry, so this list is the app's job — the same deal as route_mouse
    // above. Miss a widget out and it simply stops moving.
    tick_widgets(dt, {&m_progress, &m_btn_ok, &m_btn_cancel});
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();

    const int W = screen.cols();
    const int H = screen.rows();

    // Layout.
    const int menu_h = 1;
    const int wave_h = 6;
    const int status_h = 1;
    const int content_h = H - menu_h - wave_h - status_h - 2;
    const int left_w = W / 2;
    const int right_w = W - left_w;

    // Left frame.
    m_left_frame.set_title("Controls");
    m_left_frame.set_geometry({0, menu_h, left_w, content_h});
    m_left_frame.draw(screen);
    const auto li = m_left_frame.content_rect();

    // Label.
    m_label.set_text("Widget Showcase");
    m_label.set_align(Label::Align::Center);
    m_label.set_colors(Rgb{0x00, 0xFF, 0xFF}, theme::kBg);
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

    // Right frame: the tab strip on its first row, the selected view below it.
    m_right_frame.set_title("Views");
    m_right_frame.set_geometry({left_w, menu_h, right_w, content_h});
    m_right_frame.draw(screen);
    // content_rect() clamps its height to 0 rather than going negative, and
    // splitting it must not undo that: at H <= 12 the interior is empty, and an
    // unguarded ri.y would put the strip on the frame's bottom border.
    const auto ri = m_right_frame.content_rect();
    if (ri.h > 0) {
      m_tabs.set_geometry({ri.x, ri.y, ri.w, 1});
      m_tabs.draw(screen);
      m_list.set_geometry({ri.x, ri.y + 1, ri.w, std::max(0, ri.h - 1)});
      m_list.draw(screen);
    }

    // Waveform (full width).
    m_wave_frame.set_title("Signal");
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
    screen.write_text(W - 18, 0, std::format(" Focus: {:6}", focus_name()),
                      Rgb{0x60, 0x60, 0x80}, Rgb{0x20, 0x20, 0x40});

    // Menu bar drawn LAST so the dropdown overlays all other content
    // (and listed last in route_mouse above so it's topmost for clicks).
    m_menu.set_geometry({0, 0, W, menu_h});
    m_menu.draw(screen);
  }

 private:
  auto set_status(std::string msg) -> void { m_status_text = std::move(msg); }

  // The indeterminate pulse is the widget-level tick made visible: it moves at
  // a rate in cells per second, so it sweeps at the same speed at any frame
  // budget — and it stands still if the app stops forwarding ticks.
  auto toggle_progress_mode() -> void {
    if (m_progress.indeterminate()) {
      m_progress.set_value(0.0f);  // set_value returns it to determinate
      set_status("Progress: determinate (percentage)");
    } else {
      m_progress.set_indeterminate();
      m_progress.set_label("Working...");
      set_status("Progress: indeterminate (wall-clock pulse)");
    }
  }

  // What each tab shows. The strip reports an index; deciding what that index
  // means, and refilling the pane, is the application's half of the deal.
  auto apply_view(int view) -> void {
    std::vector<std::string> items;
    for (int i = 1; i <= 15; ++i) {
      if (view == 1 && i % 2 != 0) continue;
      if (view == 2 && i % 2 == 0) continue;
      if (view == 4) break;
      items.push_back(std::format("Item {:2} — selectable list entry", i));
    }
    if (view == 3) std::ranges::reverse(items);
    m_list.set_items(std::move(items));
  }

  // One style, applied to every frame the app owns. There is no global default
  // in the library on purpose (widgets/glyphs.hpp) — this three-line helper is
  // what "style the whole app" looks like instead.
  auto set_border(BorderStyle style, const char* name) -> void {
    m_left_frame.set_style(style);
    m_right_frame.set_style(style);
    m_wave_frame.set_style(style);
    // Not frames, but they draw from the same glyph family: without these the
    // Ascii entry leaves a Unicode ▸ in the list (#72) and in the open menu
    // (#76), and the demo contradicts the tier it is advertising.
    m_list.set_style(style);
    m_menu.set_style(style);
    m_tabs.set_style(style);
    set_status(std::format("Border: {}", name));
  }

  // Name of the currently-focused widget, for the title indicator.
  [[nodiscard]] auto focus_name() const -> const char* {
    const Widget* c = m_ring.current();
    if (c == &m_input) return "Input";
    if (c == &m_btn_ok) return "OK";
    if (c == &m_btn_cancel) return "Cancel";
    if (c == &m_tabs) return "Tabs";
    if (c == &m_list) return "List";
    if (c == &m_menu) return "Menu";
    return "-";
  }

  MenuBar m_menu;
  Frame m_left_frame, m_right_frame, m_wave_frame;
  Label m_label, m_status;
  TextInput m_input;
  Button m_btn_ok, m_btn_cancel;
  ProgressBar m_progress;
  TabBar m_tabs;
  ListWidget m_list;
  WaveformWidget m_wave;
  FocusRing m_ring;

  std::string m_status_text;
  double m_elapsed{0.0};
};

auto main() -> int {
  WidgetsDemo app;
  return app.run();
}
