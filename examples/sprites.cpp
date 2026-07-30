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

#include <cmath>
#include <cstdio>
#include <format>
#include <vector>

#include "termforge/core/terminal.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

namespace {

constexpr int kSprite = 8;   // each atlas slot is 8x8
constexpr int kSlots = 4;    // ... and there are four of them
constexpr int kSceneW = 48;  // the composed scene, in image pixels
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
  Image atlas{kSlots * kSprite, kSprite,
              std::vector<Pixel>(static_cast<std::size_t>(kSlots) * kSprite *
                                 kSprite)};

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

}  // namespace

auto main() -> int {
  const Image atlas = build_atlas();

  // Slice once, up front — this is the authoring path. The frame loop of a real
  // game would instead use the source-rect overload of blend() and allocate
  // nothing at all; one placement below does exactly that, to show both.
  std::vector<Image> sprites;
  sprites.reserve(kSlots);
  for (int s = 0; s < kSlots; ++s) sprites.push_back(atlas.sub(slot_rect(s)));

  Image scene = build_scene();

  // The four alpha regimes, side by side.
  scene.blend(sprites[kBlock], 2, 2);
  scene.blend(sprites[kDisc], 12, 2);
  scene.blend(sprites[kHalf], 22, 2);
  scene.blend(sprites[kClear], 32, 2);

  // Clipping at all four edges. Each of these is mostly outside the scene; the
  // library clips rather than throwing or wrapping.
  scene.blend(sprites[kDisc], -4, 10);       // off the left
  scene.blend(sprites[kDisc], kSceneW - 4, 10);  // off the right
  scene.blend(sprites[kDisc], 40, -4);       // off the top
  scene.blend(sprites[kDisc], 12, kSceneH - 4);  // off the bottom

  // Straight from the atlas, no intermediate Image — the allocation-free path.
  scene.blend(atlas, slot_rect(kDisc), 26, 10);

  Terminal term;
  if (auto res = term.enter_raw(); !res) {
    std::fprintf(stderr, "%s\n",
                 std::format("Failed to enter raw mode: {}", res.error().message)
                     .c_str());
    return 1;
  }
  term.enter_screen();

  auto caps = term.query_capabilities();
  if (!caps) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Capability probe failed: {}", caps.error().message).c_str());
    return 1;
  }

  auto driver = term.select_driver(*caps);
  if (auto res = driver->init(); !res) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Driver init failed: {}", res.error().message).c_str());
    return 1;
  }

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

  // Drain the kitty APC acknowledgements before waiting for a real keypress,
  // exactly as examples/image.cpp does.
  term.set_read_timeout(1);
  char buf[256];
  while (term.read_input(buf, sizeof(buf)) > 0) {
    // discard driver ack responses
  }
  term.set_read_blocking();
  while (term.read_input(buf, sizeof(buf)) <= 0) {
    // keep waiting for a real keypress
  }

  term.leave_screen();
  return 0;
}
