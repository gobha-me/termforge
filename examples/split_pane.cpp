// TermForge example: two application-owned panes and one non-owning divider.
// Mouse capture is explicitly forwarded by the app while the divider drags.

#include <algorithm>
#include <format>
#include <stdexcept>

#include "termforge/core/app.hpp"
#include "termforge/widgets/split_pane.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

class SplitPaneDemo final : public App {
 public:
  SplitPaneDemo() {
    if (const auto configured =
            m_split.configure({SplitDirection::LeftRight, 8, 12, 24});
        !configured)
      throw std::runtime_error(configured.error().message);
    m_split.on_change([this](SplitPaneState) { request_render(); });
    m_split.set_focused(true); // the app's only keyboard control
    set_mouse_mode(MouseMode::Drag);
  }

 protected:
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int width = screen.cols();
    const int height = screen.rows();
    screen.write_text(
        0, 0, "SplitPane: arrows resize | Home/End collapse | Enter restore",
        theme::kFg, theme::kBg);
    if (width < 4 || height < 4) {
      m_split.set_geometry({});
      screen.write_text(0, 1, "Resize terminal", theme::kDim, theme::kBg);
      return;
    }
    m_split.set_geometry({1, 2, width - 2, height - 4});
    const auto panes = m_split.layout();
    if (!panes) {
      screen.write_text(0, 1, panes.error().message, theme::kDim, theme::kBg);
      return;
    }

    const auto fill_pane = [&](Rect r, Rgb bg, const char* label) {
      screen.fill_rect(r.x, r.y, r.w, r.h, theme::kFg, bg);
      if (r.w >= 5 && r.h > 0)
        screen.write_text(r.x, r.y, label, theme::kFg, bg);
    };
    fill_pane(panes->first, {20, 25, 45}, "Left");
    fill_pane(panes->second, {20, 40, 35}, "Right");
    m_split.draw(screen); // divider last; it does not own either pane
    screen.write_text(0, height - 1,
                      std::format("Preferred first: {} | drag divider | Esc "
                                  "cancel | F1 ASCII | Esc quit",
                                  m_split.state().preferred_first),
                      theme::kDim, theme::kBg);
  }

  auto on_event(const Event& event) -> void override {
    if (std::holds_alternative<ResizeEvent>(event)) m_split.stop_drag();
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->key == Key::F1 && key->action != KeyAction::Release) {
      m_ascii = !m_ascii;
      m_split.set_style(m_ascii ? BorderStyle::Ascii : BorderStyle::Single);
      return;
    }
    if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
      if (m_split.dragging()) {
        (void)m_split.on_event(event);
        return;
      }
      (void)route_mouse(*mouse, {&m_split});
      return;
    }
    if (m_split.on_event(event)) return;
    App::on_event(event);
  }

 private:
  SplitPane m_split;
  bool m_ascii{false};
};

auto main() -> int {
  SplitPaneDemo app;
  return app.run();
}
