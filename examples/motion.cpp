// TermForge example: motion
//
// The on_tick(dt) contract, made visible. A box moves at a speed expressed in
// cells per SECOND, integrated in on_tick and merely drawn in on_render — so it
// crosses the screen in the same wall-clock time at every frame budget. Change
// the budget with -/+ and watch the fps readout move while the box does not.
//
// It also demonstrates the two things that are easy to get wrong by hand:
//
//   * the fixed timestep (F). At a constant dt the simulation is deterministic
//     and replayable; the ticks/frame readout shows the accumulator delivering
//     0, 1 or several ticks per frame while the motion stays smooth.
//   * the stall clamp (S). S freezes the process for two seconds. Without the
//     clamp the next frame would integrate one 2-second step and the box would
//     tunnel straight through a wall; with it, the box simply resumes.
//
// Keyboard: F fixed/variable timestep, S stall 2s, -/+ frame budget, ESC quits.

#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <variant>

#include "termforge/core/app.hpp"

using namespace termforge;

class MotionDemo final : public App {
 public:
  MotionDemo() { set_frame_ms(33); }

  auto on_event(const Event& ev) -> void override {
    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      switch (k->ch) {
        case 'f':
        case 'F':
          // Toggle between a constant dt and the real measured one.
          set_tick_hz(tick_hz() > 0 ? 0 : 120);
          return;
        case 's':
        case 'S':
          // A stall the clamp has to absorb. A busy-wait rather than a sleep so
          // it models a wedged frame, not a blocked thread — and because the
          // library itself no longer sleeps anywhere.
          busy_wait(std::chrono::seconds{2});
          return;
        case '-':
          set_frame_ms(frame_ms() + 8);
          return;
        case '+':
        case '=':
          set_frame_ms(frame_ms() > 8 ? frame_ms() - 8 : 0);
          return;
        default:
          break;
      }
    }
    App::on_event(ev);  // ESC / Ctrl+C quit
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override {
    ++m_ticks_this_frame;
    m_last_dt = dt.count();
    m_elapsed += dt.count();

    const double sec = dt.count();
    m_x += m_vx * sec;
    m_y += m_vy * sec;

    // Bounds come from the Screen, which the resize dispatched before this tick
    // has already resized — that is why the tick runs after the resize check.
    const auto max_x = static_cast<double>(screen().cols() - kBoxW);
    const auto max_y = static_cast<double>(screen().rows() - kBoxH - 2);
    if (m_x < 0.0) { m_x = -m_x; m_vx = -m_vx; ++m_bounces; }
    if (m_y < 1.0) { m_y = 2.0 - m_y; m_vy = -m_vy; ++m_bounces; }
    if (m_x > max_x) { m_x = 2.0 * max_x - m_x; m_vx = -m_vx; ++m_bounces; }
    if (m_y > max_y) { m_y = 2.0 * max_y - m_y; m_vy = -m_vy; ++m_bounces; }
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int H = screen.rows();

    const std::string mode =
        tick_hz() > 0 ? std::format("fixed {}Hz", tick_hz()) : "variable";
    screen.write_text(0, 0,
                      std::format(" on_tick: {} | budget {}ms | dt {:6.1f}ms | "
                                  "ticks/frame {} | bounces {} ",
                                  mode, frame_ms(), m_last_dt * 1000.0,
                                  m_ticks_this_frame, m_bounces),
                      Rgb{0xFF, 0xFF, 0xFF}, Rgb{0x20, 0x40, 0x80});

    // The box: drawn where the tick put it, nothing more. It is made of an
    // ASCII glyph rather than coloured blank cells, so it is still visible on
    // the FallbackDriver tier — a bare TTY has no colour to draw it with, and a
    // box of blank spaces there is a box you cannot see.
    const int bx = static_cast<int>(std::lround(m_x));
    const int by = static_cast<int>(std::lround(m_y));
    for (int r = 0; r < kBoxH; ++r)
      screen.write_text(bx, by + r, std::string(kBoxW, '#'),
                        Rgb{0x40, 0xC0, 0x60}, Rgb{0x10, 0x30, 0x18});

    screen.write_text(
        0, H - 1,
        std::format(" F: fixed/variable | S: stall 2s | -/+: budget | ESC: quit "
                    "   {:.1f}s elapsed ",
                    m_elapsed),
        Rgb{0x80, 0x80, 0x80}, Rgb{0x10, 0x10, 0x20});

    m_ticks_this_frame = 0;  // per-frame counter, reset once it has been shown
  }

 private:
  static constexpr int kBoxW{6};
  static constexpr int kBoxH{3};

  static auto busy_wait(std::chrono::steady_clock::duration d) -> void {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
    }
  }

  // Position in cells, velocity in cells per second — the units are the point.
  double m_x{4.0};
  double m_y{4.0};
  double m_vx{18.0};
  double m_vy{7.0};

  double m_elapsed{0.0};
  double m_last_dt{0.0};
  int m_ticks_this_frame{0};
  int m_bounces{0};
};

auto main() -> int {
  MotionDemo app;
  return app.run();
}
