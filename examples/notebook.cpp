// Borrowed page roots: a form, retained transcript and persistent framebuffer.
#include <algorithm>
#include <chrono>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/widgets/notebook.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

class NotebookDemo final : public App {
 public:
  NotebookDemo() {
    m_name.set_text("Edit me; switching tabs keeps this text");
    for (int i = 0; i < 40; ++i)
      m_log.append("Retained transcript line " + std::to_string(i + 1));
    auto pixels = m_pixels.pixels();
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        pixels[static_cast<std::size_t>(y * 64 + x)] =
            Pixel{static_cast<std::uint8_t>(x * 4),
                  static_cast<std::uint8_t>(y * 8), 100, 255};
    (void)m_book.add_page("Form", &m_name);
    (void)m_book.add_page("Transcript", &m_log);
    (void)m_book.add_page("Pixels", &m_pixels);
    m_book.set_focused(true);
  }
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    m_book.set_geometry({0, 0, screen.cols(), std::max(0, screen.rows() - 1)});
    m_book.draw(screen);
    render_pixel_regions(m_book);
    screen.write_text(0, screen.rows() - 1,
                      "Tab: page | Shift+Tab: strip | Ctrl+Tab: switch | F1: "
                      "ASCII | Esc: quit",
                      theme::kFg, theme::kBg);
  }
  auto on_event(const Event& event) -> void override {
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->key == Key::F1 && key->action != KeyAction::Release) {
      m_ascii = !m_ascii;
      m_book.set_style(m_ascii ? BorderStyle::Ascii : BorderStyle::Single);
      m_log.set_style(m_ascii ? BorderStyle::Ascii : BorderStyle::Single);
      return;
    }
    if (!m_book.on_event(event)) App::on_event(event);
  }
  auto on_tick(std::chrono::duration<double> dt) -> void override {
    m_book.on_tick(dt);
  }

 private:
  TextInput m_name;
  TextBox m_log;
  PixelSurface m_pixels{{64, 32}};
  Notebook m_book;
  bool m_ascii{false};
};

auto main() -> int {
  NotebookDemo app;
  return app.run();
}
