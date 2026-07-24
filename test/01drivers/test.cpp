// Offline driver tests: probe failure modes and rendering correctness without
// needing a live terminal (drivers write to an in-memory sink).

#include <catch2/catch_test_macros.hpp>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::DriverImpl;
using termforge::ErrorEvent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rgb;
using termforge::Severity;

// The DriverImpl concept must hold for concrete drivers (compile-time check).
static_assert(DriverImpl<AnsiRgbDriver>);
static_assert(DriverImpl<FallbackDriver>);
static_assert(DriverImpl<KittyDriver>);

namespace {

auto make_image(int w, int h, Pixel p) -> Image {
  return Image{w, h, std::vector<Pixel>(static_cast<std::size_t>(w) * h, p)};
}

}  // namespace

TEST_CASE("AnsiRgbDriver: empty image is a warning event, not silent", "[drivers][failure]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(0, 0, Image{});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(r.error().source == "ansi_rgb");
}

TEST_CASE("AnsiRgbDriver: half-block render emits upper-half block + colors", "[drivers]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 1x2 image: top red, bottom blue -> one ▀ with fg=red, bg=blue
  Image img{1, 2, {Pixel{255, 0, 0, 255}, Pixel{0, 0, 255, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  REQUIRE(out.find("\xE2\x96\x80") != std::string::npos);      // ▀
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);       // fg red
  REQUIRE(out.find("48;2;0;0;255") != std::string::npos);       // bg blue
  REQUIRE(out.find("\033[0m") != std::string::npos);            // reset
}

TEST_CASE("AnsiRgbDriver: identical color runs coalesce SGR sequences", "[drivers]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 2x2 solid red -> the fg SGR should be issued once, not per-cell
  auto img = make_image(2, 2, Pixel{255, 0, 0, 255});
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // solid color: fg SGR issued once, not per-cell
  REQUIRE(out.find("38;2;255;0;0") == out.rfind("38;2;255;0;0"));  // fg appears once
}


TEST_CASE("AnsiRgbDriver: odd-height image renders its last row (no dropped row)", "[drivers][failure]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 1x3 image: three rows must all render — the bug dropped row 3.
  Image img{1, 3, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255}, Pixel{0, 0, 255, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // two half-block cells rendered (rows 0-1, then row 2 + padding)
  int blocks = 0;
  size_t pos = 0;
  while ((pos = out.find("\xE2\x96\x80", pos)) != std::string::npos) { ++blocks; pos += 3; }
  REQUIRE(blocks == 2);
  // the third row's blue is present as a foreground color
  REQUIRE(out.find("38;2;0;0;255") != std::string::npos);
}

TEST_CASE("FallbackDriver: image degrades to ASCII luminance", "[drivers]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // bright white pixel -> brightest ramp char '@'; black -> ' '
  Image img{2, 1, {Pixel{255, 255, 255, 255}, Pixel{0, 0, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  REQUIRE(out.find('@') != std::string::npos);  // white -> '@'
}

TEST_CASE("FallbackDriver: empty image warns", "[drivers][failure]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(0, 0, Image{});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
}

TEST_CASE("Drivers: capabilities reflect their tier", "[drivers]") {
  AnsiRgbDriver ansi;
  FallbackDriver fb;
  KittyDriver kitty;
  REQUIRE(ansi.capabilities().truecolor);
  REQUIRE_FALSE(fb.capabilities().truecolor);
  REQUIRE(kitty.capabilities().kitty_graphics);
  REQUIRE(kitty.capabilities().truecolor);
}

// ── KittyDriver ─────────────────────────────────────────────────────────────

TEST_CASE("KittyDriver: empty image is a warning event", "[drivers][kitty][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(0, 0, Image{});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(r.error().source == "kitty");
}

TEST_CASE("KittyDriver: draw_image emits APC transmit + virtual placement + placeholders", "[drivers][kitty]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  Image img{1, 1, {Pixel{255, 0, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // Should contain an APC transmit sequence with our image data.
  REQUIRE(out.find("\033_G") != std::string::npos);      // APC opener
  REQUIRE(out.find("a=t") != std::string::npos);          // transmit only
  REQUIRE(out.find("t=d") != std::string::npos);          // direct medium
  REQUIRE(out.find("f=32") != std::string::npos);         // RGBA format
  REQUIRE(out.find("s=1") != std::string::npos);          // width
  REQUIRE(out.find("v=1") != std::string::npos);          // height
  REQUIRE(out.find("q=2") != std::string::npos);          // quiet (no ack)
  REQUIRE(out.find("\033\\") != std::string::npos);       // ST terminator
  // Should contain a virtual placement command.
  REQUIRE(out.find("a=p") != std::string::npos);          // place
  REQUIRE(out.find("U=1") != std::string::npos);          // virtual placement
  // Should contain the Unicode placeholder character (U+10EEEE).
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") != std::string::npos);
}

TEST_CASE("KittyDriver: unchanged region does not re-upload", "[drivers][kitty]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  Image img{2, 1, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // Same region, same content: no transmit (a=t), no new placement (a=p) —
  // only the placeholder cells are re-emitted.
  REQUIRE(out.find("a=t") == std::string::npos);
  REQUIRE(out.find("a=p") == std::string::npos);
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") != std::string::npos);
}

TEST_CASE("KittyDriver: classic placement is the default", "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  REQUIRE(d.placement_mode() == KittyDriver::PlacementMode::Classic);
  Image img{2, 2, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255},
                   Pixel{0, 0, 255, 255}, Pixel{255, 255, 0, 255}}};
  REQUIRE(d.draw_image(3, 4, img).has_value());
  d.flush();
  // Transmit + cursor-positioned placement scaled to the cell grid.
  REQUIRE(out.find("a=t") != std::string::npos);
  REQUIRE(out.find("\033[5;4H") != std::string::npos);  // cursor to (3,4) 1-based
  REQUIRE(out.find("a=p") != std::string::npos);
  REQUIRE(out.find("C=1") != std::string::npos);
  REQUIRE(out.find("c=2") != std::string::npos);
  REQUIRE(out.find("r=2") != std::string::npos);
  // No virtual placement, no placeholder cells.
  REQUIRE(out.find("U=1") == std::string::npos);
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") == std::string::npos);

  out.clear();
  REQUIRE(d.draw_image(3, 4, img).has_value());
  d.flush();
  // Unchanged frame: nothing at all to emit.
  REQUIRE(out.find("\033_G") == std::string::npos);
}

TEST_CASE("KittyDriver: changed content retransmits under the same image id",
          "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image red{1, 1, {Pixel{255, 0, 0, 255}}};
  Image green{1, 1, {Pixel{0, 255, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, red).has_value());
  d.flush();
  REQUIRE(out.find("i=1") != std::string::npos);

  out.clear();
  REQUIRE(d.draw_image(0, 0, green).has_value());
  d.flush();
  // New pixels, same region: retransmit with the SAME id, then recreate
  // the classic placement (kitty replaces the data but does not refresh
  // an existing classic placement). No second image id.
  REQUIRE(out.find("a=t") != std::string::npos);
  REQUIRE(out.find("i=1") != std::string::npos);
  REQUIRE(out.find("i=2") == std::string::npos);
  REQUIRE(out.find("a=d,d=i,i=1,p=1") != std::string::npos);
  REQUIRE(out.find("a=p") != std::string::npos);
}

TEST_CASE("KittyDriver: stale regions are LRU-evicted terminal-side",
          "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image img{1, 1, {Pixel{255, 0, 0, 255}}};
  // 16 slots is the cap; the 17th distinct region evicts one (a=d,d=I
  // frees the image data and its placements).
  for (int i = 0; i < 17; ++i) {
    REQUIRE(d.draw_image(i, 0, img).has_value());
    d.flush();  // advance the LRU clock between regions
  }
  REQUIRE(out.find("a=d,d=I,i=1") != std::string::npos);
  // Evicted ids are recycled, so ids stay within the one-byte range the
  // placeholder path's 38;5;<id> foreground encoding requires — nothing
  // beyond 255 is ever allocated even with many distinct regions.
  REQUIRE(out.find("i=256") == std::string::npos);
  REQUIRE(out.find("i=999") == std::string::npos);
}

TEST_CASE("KittyDriver: oversized image is cropped to the placeholder limit",
          "[drivers][kitty][failure]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  Image img{300, 1, std::vector<Pixel>(300, Pixel{255, 0, 0, 255})};
  auto r = d.draw_image(0, 0, img);
  d.flush();
  // Cropped to 297 cells and surfaced as a warning (no silent downgrade).
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(out.find("s=297") != std::string::npos);
  REQUIRE(out.find("c=297") != std::string::npos);
  // The warning fires once, not every frame.
  REQUIRE(d.draw_image(0, 0, img).has_value());
}

TEST_CASE("KittyDriver: large image chunks at 4096 bytes", "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  // 64x64 RGBA = 16384 bytes raw -> ~21848 base64 chars -> 6 chunks at 4096.
  auto img = make_image(64, 64, Pixel{0xAB, 0xCD, 0xEF, 0xFF});
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // First chunk has m=1 (more follow); intermediate chunks have m=1;
  // last chunk has m=0 (final).
  REQUIRE(out.find("m=1") != std::string::npos);
  REQUIRE(out.find("m=0") != std::string::npos);
  // Count APC openers to verify multiple chunks.
  int apc_count = 0;
  std::size_t pos = 0;
  while ((pos = out.find("\033_G", pos)) != std::string::npos) {
    ++apc_count;
    pos += 4;
  }
  REQUIRE(apc_count > 1);  // multiple chunks
}

TEST_CASE("KittyDriver: draw_text emits SGR colors", "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "Hi", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0xFF});
  d.flush();
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);   // fg red
  REQUIRE(out.find("48;2;0;0;255") != std::string::npos);   // bg blue
  REQUIRE(out.find("Hi") != std::string::npos);
}

TEST_CASE("KittyDriver: placeholder grid for 2x2 image", "[drivers][kitty]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  // 2x2 image → 2 rows × 2 cols of placeholder cells.
  Image img{2, 2, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255},
                   Pixel{0, 0, 255, 255}, Pixel{255, 255, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();

  // Count placeholder characters (U+10EEEE = F4 8F BB AE).
  int ph_count = 0;
  std::size_t pos = 0;
  while ((pos = out.find("\xF4\x8F\xBB\xAE", pos)) != std::string::npos) {
    ++ph_count;
    pos += 4;
  }
  REQUIRE(ph_count == 4);  // 2×2 grid

  // Virtual placement should specify c=2, r=2.
  REQUIRE(out.find("c=2") != std::string::npos);
  REQUIRE(out.find("r=2") != std::string::npos);

  // Placeholder cells carry the image id as a 256-color foreground —
  // kitty ignores the 24-bit form (observed: accepted, never rendered).
  REQUIRE(out.find("\033[38;5;1m") != std::string::npos);
  REQUIRE(out.find("38;2;0;0;1") == std::string::npos);

  // SGR reset after the grid.
  REQUIRE(out.find("\033[0m") != std::string::npos);
}

TEST_CASE("KittyDriver: diacritics present for non-zero row/col", "[drivers][kitty]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  // 3x1 image: 1 row, 3 cols. Per the kitty rowcolumn-diacritics table:
  // index 0 → U+0305, index 1 → U+030D, index 2 → U+030E.
  Image img{3, 1, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255},
                   Pixel{0, 0, 255, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();

  // Row 0 is explicit on every cell: U+0305 (CC 85).
  REQUIRE(out.find("\xCC\x85") != std::string::npos);  // row/col 0 diacritic
  // U+030D in UTF-8: CC 8D. U+030E in UTF-8: CC 8E.
  REQUIRE(out.find("\xCC\x8D") != std::string::npos);  // col 1 diacritic
  REQUIRE(out.find("\xCC\x8E") != std::string::npos);  // col 2 diacritic
  // The old (wrong) contiguous mapping U+0301/U+0302 must be gone.
  REQUIRE(out.find("\xCC\x81") == std::string::npos);
  REQUIRE(out.find("\xCC\x82") == std::string::npos);
}

TEST_CASE("KittyDriver: extended diacritic range for wide images", "[drivers][kitty]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  // 200x1 image: 1 row, 200 cols. Indices past the U+03xx run come from
  // later entries of the curated rowcolumn-diacritics table.
  Image img{200, 1, std::vector<Pixel>(200, Pixel{255, 0, 0, 255})};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();

  // Count placeholder characters — should be 200.
  int ph_count = 0;
  std::size_t pos = 0;
  while ((pos = out.find("\xF4\x8F\xBB\xAE", pos)) != std::string::npos) {
    ++ph_count;
    pos += 4;
  }
  REQUIRE(ph_count == 200);

  // Index 112 in the spec table is U+081B (UTF-8: E0 A0 9B) and
  // index 199 is U+20D1 (UTF-8: E2 83 91).
  REQUIRE(out.find("\xE0\xA0\x9B") != std::string::npos);
  REQUIRE(out.find("\xE2\x83\x91") != std::string::npos);
}

// ── #6 / #7: placement lifecycle ────────────────────────────────────────────

TEST_CASE("KittyDriver: a region that disappears is GC'd terminal-side (#6)",
          "[drivers][kitty]") {
  // A dialog thumbnail closes: its classic placement must not float above
  // the UI. Draw a persistent region plus a transient one, flush, then draw
  // only the persistent one and flush again — the transient region's image
  // must be deleted (a=d,d=I) in the second flush.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image a{1, 1, {Pixel{255, 0, 0, 255}}};
  Image b{1, 1, {Pixel{0, 255, 0, 255}}};

  d.draw_image(0, 0, a);   // region 1 (persists)
  d.draw_image(5, 0, b);   // region 2 (will disappear)
  d.flush();
  // Two transmits happened; region 2 got image id 2.
  REQUIRE(out.find("i=2") != std::string::npos);

  out.clear();
  d.draw_image(0, 0, a);   // only region 1 redrawn this frame
  d.flush();
  // Region 2 was not drawn this frame → GC deletes its image terminal-side.
  REQUIRE(out.find("a=d,d=I,i=2") != std::string::npos);
  // Region 1 is still alive: not deleted.
  REQUIRE(out.find("a=d,d=I,i=1") == std::string::npos);
}

TEST_CASE("KittyDriver: >16 regions in one frame all place (no same-frame thrash) (#7)",
          "[drivers][kitty]") {
  // Draw 20 distinct regions in a SINGLE frame (one flush). The per-draw LRU
  // clock must order them, so the 17th evicts region 1 (oldest draw), not a
  // region placed microseconds earlier in the same buffer.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image img{1, 1, {Pixel{255, 0, 0, 255}}};
  for (int i = 0; i < 20; ++i) d.draw_image(i, 0, img);
  d.flush();
  // The oldest four regions (ids 1..4) are evicted; the newest 16 survive.
  // Crucially, a region placed in this same flush is not among the evicted.
  REQUIRE(out.find("a=d,d=I,i=1") != std::string::npos);
  // Region 20 (id 20... or recycled) placed and was NOT deleted this frame:
  // every region got to emit its placement before any eviction, and the
  // deletions target only the four oldest draws.
  int deletions = 0;
  for (std::size_t p = out.find("a=d,d=I"); p != std::string::npos;
       p = out.find("a=d,d=I", p + 1))
    ++deletions;
  REQUIRE(deletions == 4);  // 20 drawn - 16 slots = 4 evicted, no more
}

TEST_CASE("KittyDriver: set_placement_mode resets placement state (#7)",
          "[drivers][kitty]") {
  // Place a region in Classic, then switch to UnicodePlaceholders. The old
  // classic placement must be deleted terminal-side and the region must
  // re-place as a virtual (U=1) placement — not reference a virtual
  // placement that was never created.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image img{2, 2, std::vector<Pixel>(4, Pixel{255, 0, 0, 255})};

  d.draw_image(0, 0, img);   // classic placement (default mode)
  d.flush();
  REQUIRE(out.find("U=1") == std::string::npos);  // classic: no virtual placement

  out.clear();
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  // The classic placement is torn down; the delete is buffered and reaches
  // the terminal on the next flush.
  d.flush();
  REQUIRE(out.find("a=d,d=I,i=1") != std::string::npos);

  out.clear();
  d.draw_image(0, 0, img);   // now must emit a virtual placement + cells
  d.flush();
  REQUIRE(out.find("U=1") != std::string::npos);        // virtual placement created
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") != std::string::npos);  // placeholder cells
}
