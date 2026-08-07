// TermForge example: a persistent 320x180 software framebuffer (#195).
//
// The simulation and pixel mutation happen in on_tick. on_render only lays
// out the surface, paints its always-present ASCII fallback, and submits the
// same owned Image through App's normal pixel-region window. Terminal resizes
// change the destination cell rect, never the logical framebuffer.
//
// This is deliberately not a game engine and not yet the stable resident
// replacement path from #196: changed frames are ordinary region submissions.
// Space pauses the producer; ESC quits.

#include "termforge/widgets/pixel_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <variant>

#include "termforge/core/app.hpp"

using namespace termforge;

class PixelSurfaceDemo final : public App {
 public:
  PixelSurfaceDemo() {
    set_frame_ms(33);  // requested ~30 FPS display cadence
    render_frame();
  }

  auto on_event(const Event& ev) -> void override {
    if (const auto* key = std::get_if<KeyEvent>(&ev);
        key != nullptr && key->ch == ' ') {
      m_paused = !m_paused;
      return;
    }
    App::on_event(ev);
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override {
    if (m_paused) return;
    m_time += dt.count();
    render_frame();
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    screen.write_text(
        0, 0,
        std::format(" PixelSurface 320x180 | {} | Space pause | ESC quit ",
                    m_paused ? "paused" : "running"),
        Rgb{0xF0, 0xF0, 0xF0}, Rgb{0x20, 0x40, 0x80});

    const int w = std::max(0, screen.cols() - 2);
    const int h = std::max(0, screen.rows() - 3);
    m_surface.set_geometry({1, 2, w, h});
    m_surface.draw(screen);           // Baseline: ASCII luminance
    render_pixel_regions(m_surface);  // Kitty/ANSI: owned RGBA image
  }

 private:
  auto render_frame() -> void {
    auto pixels = m_surface.pixels();
    constexpr int w = 320;
    constexpr int h = 180;
    const double cx = 160.0 + std::sin(m_time * 1.3) * 100.0;
    const double cy = 90.0 + std::cos(m_time * 0.9) * 55.0;

    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const double dx = x - cx;
        const double dy = y - cy;
        const bool disc = dx * dx + dy * dy < 24.0 * 24.0;
        const auto r = static_cast<std::uint8_t>((x * 255) / (w - 1));
        const auto g = static_cast<std::uint8_t>((y * 255) / (h - 1));
        const auto b = static_cast<std::uint8_t>(
            40 + 30 * std::sin((x + y) * 0.045 + m_time));
        pixels[static_cast<std::size_t>(y) * w + x] =
            disc ? Pixel{255, 225, 80, 255} : Pixel{r, g, b, 255};
      }
    }
  }

  PixelSurface m_surface{Extent{320, 180}};
  double m_time{0.0};
  bool m_paused{false};
};

auto main() -> int {
  PixelSurfaceDemo app;
  return app.run();
}
