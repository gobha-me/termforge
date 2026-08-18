// TermForge example: chat
//
// A complete chat-shaped composition: TextBox owns bounded scrollback and the
// Composer owns a growing multiline draft, UTF-8 cursor, paste and history.
// Enter submits through the parent; Shift/Alt+Enter adds a line when the
// terminal can report the modifier. On a Legacy terminal that chord is
// indistinguishable from Enter and therefore takes the submit fallback.

#include <algorithm>
#include <string>
#include <string_view>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/widgets/composer.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/text_box.hpp"

using namespace termforge;

class ChatApp final : public App {
 public:
  ChatApp() {
    m_log.set_retention(
        TextBoxRetention{.max_entries = 200, .max_bytes = 64 * 1024});
    m_log.append("Welcome to the TermForge Composer demo.");
    m_log.append("Enter submits; Shift/Alt+Enter adds a line when supported.");
    m_log.append("Up/Down move through the draft, then through history.");
    m_composer.set_max_height(4);
    m_ring.add(&m_composer);
  }

  auto on_event(const Event& ev) -> void override {
    if (const auto* mouse = std::get_if<MouseEvent>(&ev)) {
      if (mouse->pressed) (void)m_ring.focus_at(mouse->x, mouse->y);
      route_mouse(*mouse, {&m_log, &m_composer});
      return;
    }

    if (m_ring.handle_key(ev)) return;

    if (const auto* key = std::get_if<KeyEvent>(&ev)) {
      if (key->ctrl && key->key == Key::Char && key->ch == U'l') {
        m_log.clear();
        m_log.append("Scrollback cleared.");
        return;
      }
      if (key->key == Key::Enter) {
        submit();
        return;
      }
      if (key->key == Key::PageUp) {
        m_log.scroll(-10);
        return;
      }
      if (key->key == Key::PageDown) {
        m_log.scroll(10);
        return;
      }
    }

    App::on_event(ev);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int cols = screen.cols();
    const int rows = screen.rows();
    if (cols <= 0 || rows <= 0) return;

    screen.write_text(0, 0, "TermForge Chat — Composer", theme::kFg,
                      Rgb{0x20, 0x20, 0x40});

    const int body_rows = std::max(0, rows - 2);
    const int composer_rows =
        std::min(body_rows, m_composer.preferred_height(cols));
    const int log_rows = body_rows - composer_rows;
    m_log.set_geometry({0, 1, cols, log_rows});
    m_composer.set_geometry({0, 1 + log_rows, cols, composer_rows});
    m_log.draw(screen);
    m_composer.draw(screen);

    screen.write_text(
        0, rows - 1,
        "Enter submit | Shift/Alt+Enter newline | PgUp/PgDn log | Ctrl+L clear",
        theme::kDim, theme::kBg);
  }

 private:
  auto submit() -> void {
    const std::string draft = m_composer.text();
    if (draft.empty()) return;

    std::size_t start = 0;
    bool first = true;
    while (true) {
      const std::size_t newline = draft.find('\n', start);
      const std::string_view line = std::string_view{draft}.substr(
          start, newline == std::string::npos ? newline : newline - start);
      m_log.append(std::string{first ? "You: " : "     "} + std::string{line});
      first = false;
      if (newline == std::string::npos) break;
      start = newline + 1;
    }

    m_composer.push_history(draft);
    m_composer.clear();
    m_log.scroll_to_bottom();
  }

  TextBox m_log;
  Composer m_composer;
  FocusRing m_ring;
};

auto main() -> int {
  ChatApp app;
  return app.run();
}
