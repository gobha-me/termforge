// TermForge example: sprites
//
// Demonstrates #63's region ops end to end: a sprite atlas built in code,
// sliced with Image::sub(), and composited over a background with
// Image::blend(). No asset on disk — the atlas is generated at startup, so this
// runs anywhere the library builds.
//
// The scene is deliberately static rather than animated, so that a capture of
// the escape stream is deterministic and can be checked pixel by pixel.
//
// Shows:
//   - Image::fill()  — the background, and clearing a scratch buffer
//   - Image::sub()   — slicing four 8x8 sprites out of one 32x8 atlas
//   - Image::blend() — straight-alpha source-over, including a graduated rim
//   - Image::blend() with a source rect — reading the atlas with no copy
//   - clipping at all four edges: sprites placed partly outside the scene
//   - every driver tier: kitty pixels, ANSI half-blocks, ASCII ramp
//
// What to look for, on any tier:
//   * the opaque block has hard edges; the disc's rim fades into the
//     background instead of stopping abruptly (that fade is straight-alpha
//     compositing working — get the convention wrong and the rim goes dark)
//   * the 50% square shows the checker through it, tinted
//   * the transparent slot leaves the checker completely untouched
//   * the four edge sprites are cut off cleanly with nothing wrapping around
//
// Lifecycle (#320): every fallible step -- capability probe, driver init --
// runs BEFORE enter_screen(), so no failure path strands the user's terminal
// on the alt-screen; an RAII guard then runs driver->shutdown() and
// leave_screen() on every exit path, exceptions included; and the exit wait
// goes through the Input decoder, so early-typed keys are preserved and
// terminal control-plane replies reach the driver instead of being
// discarded raw.

#include <cmath>
#include <cstdio>
#include <format>
#include <vector>

#include "termforge/core/input.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

namespace {

// End-of-session teardown as an RAII guard (#320), so cleanup runs on the
// exception path too and not just on a normal return. shutdown() is the
// driver's explicit cleanup handoff (#148): it writes what the driver owes
// the terminal (kitty freeing its resident images) through the still-alive
// output sink -- a driver's destructor never writes. leave_screen() then
// drops the alt-screen. Declared after enter_screen() so destruction unwinds
// setup in reverse order.
struct ScreenGuard {
  TerminalDriver& driver;
  Terminal& term;
  ~ScreenGuard() {
    driver.shutdown();
    term.leave_screen();
  }
};

constexpr int kSprite = 8;  // each atlas slot is 8x8
constexpr int kSlots = 4;   // ... and there are four of them
constexpr int kSceneW = 48; // the composed scene, in image pixels
constexpr int kSceneH = 16;

// Slot indices, in atlas order.
enum Slot { kBlock = 0, kDisc = 1, kHalf = 2, kClear = 3 };

// The rect covering one slot in the atlas. This is the sprite-sheet mapping,
// and the only place it lives.
[[nodiscard]] auto slot_rect(int slot) -> Rect {
  return Rect{slot * kSprite, 0, kSprite, kSprite};
}

// One 32x8 atlas holding four sprites, each exercising a different alpha
// regime so that a compositing bug has somewhere to show itself.
[[nodiscard]] auto build_atlas() -> Image {
  Image atlas{
      kSlots * kSprite, kSprite,
      std::vector<Pixel>(static_cast<std::size_t>(kSlots) * kSprite * kSprite)};

  // 0: fully opaque — the a=255 replace path.
  atlas.fill(slot_rect(kBlock), Pixel{0xFF, 0xC0, 0x20, 255});

  // 1: a disc, opaque in the middle with a graduated-alpha rim. The rim is the
  //    antialiased edge that goes visibly wrong under any premultiplied /
  //    straight-alpha confusion, which is why it is here.
  atlas.fill(slot_rect(kDisc), Pixel{0, 0, 0, 0});
  {
    const double centre = (kSprite - 1) / 2.0;
    const double solid_r = 2.2, edge_r = 3.6;
    const Rect r = slot_rect(kDisc);
    for (int y = 0; y < kSprite; ++y) {
      for (int x = 0; x < kSprite; ++x) {
        const double dx = x - centre, dy = y - centre;
        const double d = std::sqrt(dx * dx + dy * dy);
        int a = 0;
        if (d <= solid_r) {
          a = 255;
        } else if (d < edge_r) {
          a = static_cast<int>(255.0 * (edge_r - d) / (edge_r - solid_r));
        }
        if (a > 0) {
          atlas.fill(Rect{r.x + x, r.y + y, 1, 1},
                     Pixel{0x40, 0xE0, 0xFF, static_cast<std::uint8_t>(a)});
        }
      }
    }
  }

  // 2: uniformly 50% translucent — the exact midpoint the suite pins, made
  //    visible.
  atlas.fill(slot_rect(kHalf), Pixel{0xFF, 0x40, 0xFF, 128});

  // 3: fully transparent — blending it must be a no-op.
  atlas.fill(slot_rect(kClear), Pixel{0xFF, 0xFF, 0xFF, 0});

  return atlas;
}

// The conventional transparency checker. A SOLID background could not
// distinguish blend from blit — every result would look plausible — so the
// background varies.
[[nodiscard]] auto build_scene() -> Image {
  Image scene{kSceneW, kSceneH,
              std::vector<Pixel>(static_cast<std::size_t>(kSceneW) * kSceneH)};
  constexpr int kSquare = 4;
  const Pixel light{0x60, 0x60, 0x68, 255}, dark{0x38, 0x38, 0x40, 255};
  for (int y = 0; y < kSceneH; y += kSquare) {
    for (int x = 0; x < kSceneW; x += kSquare) {
      const bool even = ((x / kSquare) + (y / kSquare)) % 2 == 0;
      scene.fill(Rect{x, y, kSquare, kSquare}, even ? light : dark);
    }
  }
  return scene;
}

} // namespace

auto main() -> int {
  const Image atlas = build_atlas();

  // Slice once, up front — this is the authoring path. The frame loop of a real
  // game would instead use the source-rect overload of blend() and allocate
  // nothing at all; one placement below does exactly that, to show both.
  std::vector<Image> sprites;
  sprites.reserve(kSlots);
  for (int s = 0; s < kSlots; ++s)
    sprites.push_back(atlas.sub(slot_rect(s)));

  Image scene = build_scene();

  // The four alpha regimes, side by side.
  scene.blend(sprites[kBlock], 2, 2);
  scene.blend(sprites[kDisc], 12, 2);
  scene.blend(sprites[kHalf], 22, 2);
  scene.blend(sprites[kClear], 32, 2);

  // Clipping at all four edges. Each of these is mostly outside the scene; the
  // library clips rather than throwing or wrapping.
  scene.blend(sprites[kDisc], -4, 10);          // off the left
  scene.blend(sprites[kDisc], kSceneW - 4, 10); // off the right
  scene.blend(sprites[kDisc], 40, -4);          // off the top
  scene.blend(sprites[kDisc], 12, kSceneH - 4); // off the bottom

  // Straight from the atlas, no intermediate Image — the allocation-free path.
  scene.blend(atlas, slot_rect(kDisc), 26, 10);

  Terminal term;
  if (auto res = term.enter_raw(); !res) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Failed to enter raw mode: {}", res.error().message)
            .c_str());
    return 1;
  }

  // Everything fallible happens before enter_screen() (#320): a failure on
  // either of these paths returns with the terminal still on its normal
  // screen.
  auto caps = term.query_capabilities();
  if (!caps) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Capability probe failed: {}", caps.error().message)
            .c_str());
    return 1;
  }

  auto driver = term.select_driver(*caps);
  if (auto res = driver->init(); !res) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Driver init failed: {}", res.error().message).c_str());
    return 1;
  }

  // Enter alt-screen, then arm the guard that undoes it (and ends the
  // driver's session) on every exit path from here on.
  term.enter_screen();
  const ScreenGuard cleanup{*driver, term};

  const auto dcaps = driver->capabilities();
  const char* tier = dcaps.kitty_graphics ? "Kitty graphics"
                     : dcaps.truecolor    ? "ANSI truecolor half-blocks"
                                          : "ASCII fallback";

  const Rgb white{0xFF, 0xFF, 0xFF}, bg = theme::kBg;
  const Rgb cyan{0x00, 0xFF, 0xFF}, green{0x00, 0xFF, 0x80};

  driver->draw_text(0, 0, "TermForge Sprite Compositing Demo (#63)", cyan, bg,
                    Attr::Bold);
  driver->draw_text(0, 1, std::format("Driver tier: {}", tier), green, bg,
                    Attr::None);
  driver->draw_text(
      0, 2,
      std::format("Atlas {}x{} -> 4 sprites, composited into a {}x{} scene",
                  atlas.width(), atlas.height(), scene.width(), scene.height()),
      white, bg, Attr::None);
  driver->draw_text(0, 3,
                    "Row 1: opaque | graduated rim | 50% | transparent."
                    "  Row 2: clipped at all four edges.",
                    white, bg, Attr::Dim);

  // The driver knows how many cells the scene occupies at its native
  // resolution; ask it, and draw into exactly that rect (#83/#100). The old
  // form here re-derived it from capability flags and got the fallback tier
  // wrong, dropping the prompt on top of the scene.
  constexpr int kImageRow = 5;
  const Extent extent = driver->image_cell_extent(scene);
  const Rect dest{0, kImageRow, extent.w, extent.h};
  if (auto res = driver->draw_image(dest, scene); !res) {
    driver->draw_text(0, kImageRow,
                      "Scene render failed: " + res.error().message,
                      Rgb{0xFF, 0x40, 0x40}, bg, Attr::None);
  }

  const int prompt_row = dest.y + dest.h + 1;
  driver->draw_text(0, prompt_row, "Press any key to exit...", white, bg,
                    Attr::Dim);
  driver->flush();

  // Wait for a keypress through the Input decoder (#320), exactly as
  // examples/image.cpp does. This demo uploads raw RGBA with no ack
  // requested, so the raw drain loop this replaces was not discarding
  // "driver ack responses" -- it was discarding keys the user typed early.
  // Feed every chunk until a read comes back empty, flush() only at that
  // drained boundary (a held lone ESC resolves to Escape there, while split
  // sequences stay held), route control-plane replies to the driver, and
  // exit on the first decoded key press.
  Input input;
  term.set_read_timeout(1); // 100ms poll
  bool running = true;
  while (running) {
    char buf[256];
    while (true) {
      const int n = term.read_input(buf, sizeof(buf));
      if (n <= 0) break;
      input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
    }
    input.flush();

    // Control-plane records are not keypresses: offer real replies to the
    // driver (base-class consume_reply is a no-op; KittyDriver overrides it
    // to consume its acks), and let a malformed-APC ErrorEvent pass silently
    // rather than surfacing it as input.
    for (auto& record : input.poll_replies()) {
      if (auto* reply = std::get_if<TerminalReply>(&record))
        driver->consume_reply(*reply);
    }

    for (auto& ev : input.poll()) {
      std::visit(
          [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, KeyEvent>) {
              if (e.action == KeyAction::Press) running = false;
            }
          },
          ev);
    }
  }

  // Cleanup runs in the ScreenGuard destructor: driver->shutdown() while the
  // output sink is still alive, then leave_screen().
  return 0;
}
