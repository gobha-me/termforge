// TermForge example: pinned
//
// A resident image (#109) placed from App's second window (#191). This is the
// correct call site for `pin_image`/`draw_pinned` inside an `App`, and it is
// the whole reason `on_pixels` exists — so read the two hooks below against
// each other rather than reading either alone.
//
// The idea #109 exists for: the terminal keeps the pixels, and a frame that
// moves the sprite sends only a placement. One upload at startup, then a few
// dozen bytes per frame no matter how big the image is — instead of the whole
// payload every time it moves. On the wire an image's lifetime and a
// placement's lifetime are different things, and the difference is one letter:
// `d=I` frees the data, `d=i` retires one placement.
//
// WHERE YOU DRAW IT DECIDES WHETHER IT WORKS, and this example is built to make
// that visible. Press W to move the draw into `on_render` and watch the sprite
// start blinking:
//
//   on_render          <- window 1. Runs BEFORE the cell diff is written.
//   [ the frame's first write ]
//   on_pixels          <- window 2. Runs after it, with App's own pixel
//                         regions, and everything here shares one write.
//   [ the frame's second write ]
//
// A driver is told where a WRITE ends and never where a FRAME does. It infers
// the frame boundary from App's first write having drawn nothing (#187) — and
// an image drawn in window 1 makes that false, so the two writes' collections
// each destroy what the other drew: the sprite is retired in one write and
// re-placed in the next, which is a visible blink, and an unpinned image is
// re-uploaded in full every frame.
//
// What to look for, on a terminal with kitty graphics:
//   * the sprite moves smoothly and stays solid
//   * press W: it starts flickering, and the byte counter climbs much faster
//   * press W again: it goes solid again
//
// On any other tier there is nothing to see: pinning is refused honestly (with
// an ErrorEvent, never silently) and the cell fallback below is what renders.

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;

namespace {

constexpr int kSprite = 48;  // px; two or three cells on a typical terminal

// A soft green disc on transparent, generated so the example needs no asset.
auto make_sprite() -> Image {
  std::vector<Pixel> px(static_cast<std::size_t>(kSprite) * kSprite);
  const double r = kSprite / 2.0;
  for (int y = 0; y < kSprite; ++y) {
    for (int x = 0; x < kSprite; ++x) {
      const double dx = (x + 0.5) - r;
      const double dy = (y + 0.5) - r;
      const double d = std::sqrt(dx * dx + dy * dy);
      // A two-pixel feathered rim: straight-alpha, source-over.
      const double a = d >= r ? 0.0 : (d > r - 2.0 ? (r - d) / 2.0 : 1.0);
      px[static_cast<std::size_t>(y) * kSprite + x] =
          Pixel{40, 220, 120, static_cast<std::uint8_t>(a * 255.0)};
    }
  }
  return Image{kSprite, kSprite, std::move(px)};
}

// A plate the app draws through App's OWN pixel-region path -- rasterized once
// and handed back unchanged forever, which is the shape #187's dedup exists
// for. It matters here for one reason: its draw lands in window 2 every frame,
// so with the sprite in window 1 the frame's image draws STRADDLE both windows.
// That is the shape #191 is about, and one window alone cannot produce it.
class Plate final : public Widget {
 public:
  Plate() {
    std::vector<Pixel> px(static_cast<std::size_t>(kPlate) * kPlate);
    for (int y = 0; y < kPlate; ++y)
      for (int x = 0; x < kPlate; ++x)
        px[static_cast<std::size_t>(y) * kPlate + x] =
            ((x / 8 + y / 8) % 2) ? Pixel{60, 60, 110, 255}
                                  : Pixel{35, 35, 70, 255};
    m_cache = Image{kPlate, kPlate, std::move(px)};
  }

  auto draw(Screen&) -> void override {}
  auto pixel_regions() -> std::vector<Rect> override { return {rect()}; }
  auto draw_pixels(Rect, Extent) -> const Image* override { return &m_cache; }

 private:
  static constexpr int kPlate = 96;
  Image m_cache;
};

}  // namespace

class PinnedDemo final : public App {
 public:
  explicit PinnedDemo(bool start_wrong) : m_wrong_window(start_wrong) {
    set_frame_ms(16);
  }

  // The upload happens once, here, after capability negotiation and before the
  // first frame — which is exactly what on_start is for (#97). A refusal is not
  // fatal: m_pin stays empty and every draw below is skipped, so a terminal
  // without graphics runs this example with cells only.
  auto on_start() -> void override {
    if (auto pinned = driver().pin_image(make_sprite())) {
      m_pin = *pinned;
    } else {
      m_why = pinned.error().message;
    }
  }

  // Unpin on the way out, for the shape rather than for the effect. Be honest
  // about which is which: `unpin_image` QUEUES its `d=I` like every other draw,
  // and nothing flushes after on_stop(), so on this path the escape never
  // reaches the terminal -- ~KittyDriver's own `a=d,d=A` is what actually frees
  // it. What the call does buy here is the driver-side budget (one of the 239
  // resident slots), which is the thing that matters to a long-lived session
  // that pins and unpins as views come and go. That session should unpin when a
  // view CLOSES, not at exit.
  auto on_stop() noexcept -> void override {
    if (m_pin) (void)driver().unpin_image(m_pin);
  }

  auto on_event(const Event& ev) -> void override {
    if (const auto* k = std::get_if<KeyEvent>(&ev);
        k != nullptr && (k->ch == 'w' || k->ch == 'W')) {
      m_wrong_window = !m_wrong_window;
      return;
    }
    App::on_event(ev);  // ESC / Ctrl+C quit
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override {
    m_t += dt.count();
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    m_plate.set_geometry(Rect{1, 7, 12, 6});
    const auto bytes = driver().total_bytes();
    screen.write_text(
        0, 0,
        std::format(" pinned image | drawing from {} | W switches | {} bytes ",
                    m_wrong_window ? "on_render  (WRONG -- watch it blink)"
                                   : "on_pixels  (correct)",
                    bytes.total()),
        kFg, kBg);
    if (!m_pin) {
      screen.write_text(0, 2, " no resident images on this terminal: " + m_why,
                        kFg, kBg);
      screen.write_text(0, 3, " (the cell tier has nothing to show here)", kFg,
                        kBg);
    }
    screen.write_text(0, 6, " the plate below is an ordinary pixel region ",
                      kFg, kBg);
    screen.write_text(0, screen.rows() - 1, " ESC quits ", kFg, kBg);

    // App's own path. It only RECORDS here; the draw_image happens later, in
    // window 2 -- which is what makes the sprite's window the whole experiment.
    render_pixel_regions(m_plate);

    // The demonstration. This is the call site that does NOT work, and it is
    // here so the example can show the failure rather than describe it.
    if (m_wrong_window) place(driver());
  }

  // The correct call site. Everything drawn here shares the frame's second
  // write with App's own pixel regions, so the driver sees one frame boundary
  // and the placement survives untouched from frame to frame.
  auto on_pixels(TerminalDriver& driver) -> void override {
    if (!m_wrong_window) place(driver);
  }

 private:
  auto place(TerminalDriver& driver) -> void {
    if (!m_pin) return;
    const int w = 3;
    const int h = 2;
    const auto span = static_cast<double>(screen().cols() - w);
    // A triangle wave, so the sprite crosses the screen and comes back without
    // needing any state beyond the elapsed time.
    const double phase = std::fmod(m_t * 0.25, 2.0);
    const double u = phase < 1.0 ? phase : 2.0 - phase;
    const Rect at{static_cast<int>(u * span), 4, w, h};
    // The result is yours here, unlike App's own region draws: a refusal is a
    // real event (a rect off-screen, a placeholder conflict) and dropping it is
    // how a UI ends up with a hole nobody can explain.
    if (auto ok = driver.draw_pinned(at, m_pin); !ok) m_last_error = ok.error();
  }

  Plate m_plate;
  PinnedImage m_pin{};
  std::string m_why{"driver refused"};
  std::optional<ErrorEvent> m_last_error;
  double m_t{0.0};
  bool m_wrong_window;

  static constexpr Rgb kFg{0xE0, 0xE0, 0xF0};
  static constexpr Rgb kBg{0x10, 0x10, 0x18};
};

// `--on-render` starts in the broken window instead of toggling into it. It is
// here so a measurement of either arm is one command rather than a keystroke
// sent at the right moment -- the numbers in docs/pixel-regions.md come from
// running this twice.
auto main(int argc, char** argv) -> int {
  bool wrong = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--on-render") wrong = true;
  }
  PinnedDemo app{wrong};
  return app.run();
}
