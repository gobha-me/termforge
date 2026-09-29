// TermForge example: an application-owned viewport with a standalone bar.
// The content is ordinary application data; Scrollbar knows only units and
// reports offset changes. No child registry or pointer capture is involved.

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/widgets/scrollbar.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

class ScrollbarDemo final : public App {
 public:
  ScrollbarDemo() {
    for (int row = 0; row < 80; ++row)
      m_lines.push_back(std::format("Row {:03}  caller-owned content", row));
    m_bar.on_change([this](int offset) {
      m_offset = offset;
      request_render();
    });
    m_bar.set_focused(true); // this is the app's single keyboard control
    set_mouse_mode(MouseMode::Click);
  }

 protected:
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int width = screen.cols();
    const int height = screen.rows();
    if (width < 4 || height < 4) {
      m_bar.set_geometry({});
      screen.write_text(0, 0, "Resize", theme::kFg, theme::kBg);
      return;
    }

    const int visible = height - 2;
    const int total = static_cast<int>(m_lines.size());
    m_offset = std::clamp(m_offset, 0, std::max(0, total - visible));
    if (const auto configured = m_bar.set_viewport({total, m_offset, visible});
        !configured) {
      screen.write_text(0, 0, configured.error().message, theme::kFg,
                        theme::kBg);
      return;
    }
    m_bar.set_geometry({width - 1, 1, 1, visible});

    screen.write_text(0, 0,
                      "Scrollbar: arrows / PgUp / PgDn / bar wheel or click",
                      theme::kFg, theme::kBg);
    for (int row = 0; row < visible && m_offset + row < total; ++row)
      screen.write_text(0, row + 1,
                        m_lines[static_cast<std::size_t>(m_offset) +
                                static_cast<std::size_t>(row)],
                        theme::kFg, theme::kBg);
    m_bar.draw(screen);
    screen.write_text(0, height - 1,
                      std::format("Offset {} / {} | F1 ASCII | Esc quit",
                                  m_offset, std::max(0, total - visible)),
                      theme::kDim, theme::kBg);
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->key == Key::F1 && key->action != KeyAction::Release) {
      m_ascii = !m_ascii;
      m_bar.set_style(m_ascii ? BorderStyle::Ascii : BorderStyle::Single);
      return;
    }
    if (m_bar.on_event(event)) return;
    App::on_event(event);
  }

 private:
  std::vector<std::string> m_lines;
  Scrollbar m_bar;
  int m_offset{0};
  bool m_ascii{false};
};

auto main() -> int {
  ScrollbarDemo app;
  return app.run();
}
