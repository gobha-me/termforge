// TermForge chat-scrollback demo — a live TextBox + input line.
// Type to compose, Enter posts to the scrollback, PageUp/PageDown scroll,
// ESC quits. Exercises the widget on the real interactive loop.

#include <string>

#include "termforge/core/app.hpp"
#include "termforge/widgets/text_box.hpp"

using namespace termforge;

class ChatDemo final : public App {
 public:
  ChatDemo() {
    m_box.append("Welcome to the TermForge scrollback demo.");
    m_box.append("Type a message and press Enter to post it.");
    m_box.append("PageUp/PageDown scroll history. ESC quits.");
    m_box.append("This is a long line that should wrap when the window is narrow enough to force word wrapping onto multiple display rows.");
  }

  auto on_event(const Event& ev) -> void override {
    // route scroll events to the textbox first
    if (m_box.on_event(ev)) return;

    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      if (k->key == Key::Enter) {
        if (!m_draft.empty()) {
          m_box.append("you: " + m_draft);
          // echo a fake reply so the scrollback grows
          m_box.append("bot: got it — \"" + m_draft + "\"");
          m_draft.clear();
        }
        return;
      }
      if (k->key == Key::Backspace) {
        if (!m_draft.empty()) m_draft.pop_back();
        return;
      }
      if (k->key == Key::Char && k->ch >= 0x20 && !k->ctrl) {
        // append UTF-8 for the code point
        char32_t cp = k->ch;
        if (cp < 0x80) m_draft += static_cast<char>(cp);
        else if (cp < 0x800) { m_draft += static_cast<char>(0xC0 | (cp >> 6)); m_draft += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { m_draft += static_cast<char>(0xE0 | (cp >> 12)); m_draft += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); m_draft += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { m_draft += static_cast<char>(0xF0 | (cp >> 18)); m_draft += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); m_draft += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); m_draft += static_cast<char>(0x80 | (cp & 0x3F)); }
        return;
      }
    }
    App::on_event(ev);  // ESC / Ctrl+C
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int W = screen.cols(), H = screen.rows();
    // textbox fills all but the last 2 rows
    m_box.set_geometry({0, 1, W, H - 3});
    screen.write_text(0, 0, "TermForge chat demo — ESC quits", Rgb{0x7A,0x7A,0x9A}, {});
    m_box.draw(screen);
    // input line
    screen.write_text(0, H - 2, std::string(W > 1 ? W - 1 : 1, '-'), Rgb{0x2A,0x2A,0x52}, {});
    screen.write_text(0, H - 1, "> " + m_draft + "_", Rgb{0x00,0xD4,0xFF}, {});
  }

 private:
  TextBox m_box;
  std::string m_draft;
};

auto main() -> int {
  ChatDemo app;
  return app.run();
}
