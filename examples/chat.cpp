// TermForge example: chat
//
// Demonstrates the TextBox widget for multi-line scrollable text. Shows how to:
//   - Use TextBox::append() to add lines
//   - TextBox::scroll() for manual scrolling
//   - TextBox::draw() to render the widget
//   - Keyboard shortcuts (Ctrl+L to clear, arrows to scroll)

#include <format>

#include "termforge/core/app.hpp"
#include "termforge/widgets/text_box.hpp"

using namespace termforge;

class ChatApp final : public App {
 public:
  ChatApp() {
    m_textbox.append("Welcome to TermForge Chat Demo!");
    m_textbox.append("");
    m_textbox.append("This is a scrollable text box widget.");
    m_textbox.append("Use arrow keys or mouse wheel to scroll.");
    m_textbox.append("Press Ctrl+L to clear the text.");
    m_textbox.append("");

    // Add some sample lines
    for (int i = 1; i <= 20; ++i) {
      m_textbox.append(std::format("Sample message #{}", i));
    }
  }

  auto on_event(const Event& ev) -> void override {
    std::visit([this](const auto& e) { this->handle(e); }, ev);
    App::on_event(ev);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();

    // Draw textbox in most of the screen
    const int W = screen.cols();
    const int H = screen.rows();
    m_textbox.set_geometry({0, 1, W, H - 2});
    m_textbox.draw(screen);

    // Header
    screen.write_text(0, 0, "TermForge Chat Demo", Rgb{0xFF, 0xFF, 0xFF}, Rgb{0x20, 0x20, 0x40});

    // Footer with instructions
    const std::string footer = "Arrows: scroll | Ctrl+L: clear | ESC: quit";
    screen.write_text(0, H - 1, footer, Rgb{0x80, 0x80, 0x80}, {});
  }

 private:
  auto handle(const KeyEvent& k) -> void {
    if (k.ctrl && k.ch == 'l') {
      m_textbox.clear();
      m_textbox.append("Text cleared.");
      return;
    }

    if (k.key == Key::Up) {
      m_textbox.scroll(-1);
    } else if (k.key == Key::Down) {
      m_textbox.scroll(1);
    } else if (k.key == Key::PageUp) {
      m_textbox.scroll(-10);
    } else if (k.key == Key::PageDown) {
      m_textbox.scroll(10);
    } else if (k.key == Key::Home) {
      m_textbox.scroll(-static_cast<int>(m_textbox.line_count()));
    } else if (k.key == Key::End) {
      m_textbox.scroll_to_bottom();
    }
  }

  auto handle(const MouseEvent& m) -> void {
    if (m.scroll_up) {
      m_textbox.scroll(-3);
    } else if (m.scroll_down) {
      m_textbox.scroll(3);
    }
  }

  auto handle(const PasteEvent& p) -> void {
    m_textbox.append(std::format("[pasted {} chars]", p.text.size()));
  }

  auto handle(const ResizeEvent&) -> void {
    // TextBox will be repositioned in on_render
  }

  auto handle(const ErrorEvent&) -> void {
    // Ignore errors in this demo
  }

  TextBox m_textbox;
};

auto main() -> int {
  ChatApp app;
  return app.run();
}
