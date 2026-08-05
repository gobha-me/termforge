// Offline driver tests: probe failure modes and rendering correctness without
// needing a live terminal (drivers write to an in-memory sink).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::DriverImpl;
using termforge::ErrorEvent;
using termforge::Extent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using termforge::Rgb;
using termforge::Severity;
using tfsupport::checker;
using tfsupport::solid;

// Most of this file asserts with out.find(), which cannot tell `i=1` from
// `i=16`. Where an assertion is about a COUNT or about the absence of an id,
// that is a false green waiting to happen, so those go through the shared
// parser in test/support/apc.hpp instead. Used by the LRU case below; the older
// substring checks are left alone rather than swept in an unrelated cut.
using tfsupport::data_deletes_of;
using tfsupport::ids_named;

// The DriverImpl concept must hold for concrete drivers (compile-time check).
static_assert(DriverImpl<AnsiRgbDriver>);
static_assert(DriverImpl<FallbackDriver>);
static_assert(DriverImpl<KittyDriver>);

TEST_CASE("AnsiRgbDriver: empty image is a warning event, not silent", "[drivers][failure]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(Rect{0, 0, 1, 1}, Image{});
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
  // One cell tall: this tier packs two pixel rows into it.
  REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, img).has_value());
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
  auto img = solid(2, 2, Pixel{255, 0, 0, 255});
  REQUIRE(d.draw_image(Rect{0, 0, 2, 1}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{0, 0, 1, 2}, img).has_value());
  d.flush();
  // two half-block cells rendered
  int blocks = 0;
  size_t pos = 0;
  while ((pos = out.find("\xE2\x96\x80", pos)) != std::string::npos) { ++blocks; pos += 3; }
  REQUIRE(blocks == 2);
  // Restated for #83. The destination is 2 cells = 4 sample rows, always
  // even, so the odd final row is no longer paired with a transparent lower
  // half — it is *sampled* like every other row. Blue therefore lands in a
  // background rather than a foreground. What the case actually guards is
  // unchanged: no source row disappears.
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);  // red survives
  REQUIRE(out.find("38;2;0;255;0") != std::string::npos);  // green survives
  REQUIRE(out.find("48;2;0;0;255") != std::string::npos);  // blue survives
}

TEST_CASE("FallbackDriver: image degrades to ASCII luminance", "[drivers]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // bright white pixel -> brightest ramp char '@'; black -> ' '
  Image img{2, 1, {Pixel{255, 255, 255, 255}, Pixel{0, 0, 0, 255}}};
  REQUIRE(d.draw_image(Rect{0, 0, 2, 1}, img).has_value());
  d.flush();
  REQUIRE(out.find('@') != std::string::npos);  // white -> '@'
}

TEST_CASE("FallbackDriver: empty image warns", "[drivers][failure]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(Rect{0, 0, 1, 1}, Image{});
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
  auto r = d.draw_image(Rect{0, 0, 1, 1}, Image{});
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
  REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{0, 0, 2, 1}, img).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_image(Rect{0, 0, 2, 1}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{3, 4, 2, 2}, img).has_value());
  d.flush();
  // Transmit + cursor-positioned placement scaled to the cell grid.
  REQUIRE(out.find("a=t") != std::string::npos);
  REQUIRE(out.find("\033[5;4H") != std::string::npos);  // cursor to (3,4) 1-based
  REQUIRE(out.find("a=p") != std::string::npos);
  REQUIRE(out.find("C=1") != std::string::npos);
  // Restated for #83: c= and r= are the DESTINATION RECT's 2x2, not the
  // image's 2x2 pixels. The assertion is unchanged and the two numbers still
  // agree here only because this case happens to be 1:1 — see the scaling
  // cases below, where they no longer do.
  REQUIRE(out.find("c=2") != std::string::npos);
  REQUIRE(out.find("r=2") != std::string::npos);
  // No virtual placement, no placeholder cells.
  REQUIRE(out.find("U=1") == std::string::npos);
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") == std::string::npos);

  out.clear();
  REQUIRE(d.draw_image(Rect{3, 4, 2, 2}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, red).has_value());
  d.flush();
  REQUIRE(out.find("i=1") != std::string::npos);

  out.clear();
  REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, green).has_value());
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
  //
  // NO flush inside the loop, and that is the whole test. With one, every
  // region is collected before the next is drawn, the map never reaches the
  // cap, and the LRU scan in region_slot() is never entered — so this case
  // asserted on the COLLECTION for its entire life while claiming eviction.
  // It passed either way, which is what made it invisible.
  for (int i = 0; i < 17; ++i) {
    REQUIRE(d.draw_image(Rect{i, 0, 1, 1}, img).has_value());
  }
  d.flush();
  // Region 1 was the least-recently-drawn, so it is the victim.
  REQUIRE(data_deletes_of(out, 1) == 1);
  // Evicted ids are recycled, so ids stay within the one-byte range the
  // placeholder path's 38;5;<id> foreground encoding requires. Asserted as a
  // bound over every id on the wire rather than by grepping for two arbitrary
  // large ones: `out.find("i=256")` is a spot check that 17 regions could never
  // have reached anyway.
  // 17 distinct regions, but only 16 ids: the 17th recycles the evicted one.
  // kFirstPinnedImageId is the public spelling of "one past the region pool",
  // and a region id at or above it is one that would collide with a pin (#109).
  for (const std::uint32_t id : ids_named(out)) {
    INFO("id " << id);
    CHECK(id < KittyDriver::kFirstPinnedImageId);
  }
}

TEST_CASE("KittyDriver: oversized destination is clamped to the placeholder limit",
          "[drivers][kitty][failure]") {
  // Restated for #83, and the axis is the whole point. This used to crop the
  // IMAGE to 297x297 pixels, which was the same thing when a pixel was a
  // cell. The limit belongs to the diacritic table that indexes *cells*, so
  // it now clamps the destination rect and the image transmits whole.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  Image img{10, 10, std::vector<Pixel>(100, Pixel{255, 0, 0, 255})};
  auto r = d.draw_image(Rect{0, 0, 300, 1}, img);
  d.flush();
  // Clamped to 297 cells and surfaced as a warning (no silent downgrade).
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(out.find("c=297") != std::string::npos);
  REQUIRE(out.find("r=1") != std::string::npos);
  // The image itself is untouched — this is the assertion that pins "clamps
  // the placement, does not discard authored pixels".
  REQUIRE(out.find("s=10") != std::string::npos);
  REQUIRE(out.find("v=10") != std::string::npos);
  int ph_count = 0;
  for (std::size_t p = out.find("\xF4\x8F\xBB\xAE"); p != std::string::npos;
       p = out.find("\xF4\x8F\xBB\xAE", p + 4))
    ++ph_count;
  REQUIRE(ph_count == 297);
  // The warning fires once, not every frame.
  REQUIRE(d.draw_image(Rect{0, 0, 300, 1}, img).has_value());
}

TEST_CASE("KittyDriver: an image wider than 297px into a small rect is legal",
          "[drivers][kitty]") {
  // The case that was impossible to express before #83: 300 image pixels are
  // no longer 300 cells, so nothing is over any limit and nothing is warned
  // about. This is exactly what the old code cropped.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  Image img{300, 1, std::vector<Pixel>(300, Pixel{255, 0, 0, 255})};
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();
  REQUIRE(out.find("s=300") != std::string::npos);  // transmitted whole
  REQUIRE(out.find("c=4") != std::string::npos);
  REQUIRE(out.find("r=2") != std::string::npos);
  int ph_count = 0;
  for (std::size_t p = out.find("\xF4\x8F\xBB\xAE"); p != std::string::npos;
       p = out.find("\xF4\x8F\xBB\xAE", p + 4))
    ++ph_count;
  REQUIRE(ph_count == 8);
}

TEST_CASE("KittyDriver: large image chunks at 4096 bytes", "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  // 64x64 RGBA = 16384 bytes raw -> ~21848 base64 chars -> 6 chunks at 4096.
  auto img = solid(64, 64, Pixel{0xAB, 0xCD, 0xEF, 0xFF});
  REQUIRE(d.draw_image(Rect{0, 0, 64, 64}, img).has_value());
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
  d.draw_text(0, 0, "Hi", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0xFF},
              Attr::None);
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
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{0, 0, 3, 1}, img).has_value());
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
  REQUIRE(d.draw_image(Rect{0, 0, 200, 1}, img).has_value());
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

  d.draw_image(Rect{0, 0, 1, 1}, a);   // region 1 (persists)
  d.draw_image(Rect{5, 0, 1, 1}, b);   // region 2 (will disappear)
  d.flush();
  // Two transmits happened; region 2 got image id 2.
  REQUIRE(out.find("i=2") != std::string::npos);

  out.clear();
  d.draw_image(Rect{0, 0, 1, 1}, a);   // only region 1 redrawn this frame
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
  for (int i = 0; i < 20; ++i) d.draw_image(Rect{i, 0, 1, 1}, img);
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

  d.draw_image(Rect{0, 0, 2, 2}, img);   // classic placement (default mode)
  d.flush();
  REQUIRE(out.find("U=1") == std::string::npos);  // classic: no virtual placement

  out.clear();
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  // The classic placement is torn down; the delete is buffered and reaches
  // the terminal on the next flush.
  d.flush();
  REQUIRE(out.find("a=d,d=I,i=1") != std::string::npos);

  out.clear();
  d.draw_image(Rect{0, 0, 2, 2}, img);   // now must emit a virtual placement + cells
  d.flush();
  REQUIRE(out.find("U=1") != std::string::npos);        // virtual placement created
  REQUIRE(out.find("\xF4\x8F\xBB\xAE") != std::string::npos);  // placeholder cells
}

// ── #83: the destination is a cell rect ─────────────────────────────────────
//
// Every case below was impossible to express before #83: the destination cell
// count WAS the image's pixel count, so "not 1:1" had no spelling.

TEST_CASE("KittyDriver: c=/r= follow the destination rect, not the image",
          "[drivers][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  SECTION("image larger than its destination") {
    auto img = solid(4, 4, Pixel{255, 0, 0, 255});
    REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img).has_value());
    d.flush();
    REQUIRE(out.find("s=4") != std::string::npos);   // transmitted at 4x4
    REQUIRE(out.find("v=4") != std::string::npos);
    REQUIRE(out.find("c=2") != std::string::npos);   // placed across 2x2 cells
    REQUIRE(out.find("r=2") != std::string::npos);
  }

  SECTION("image smaller than its destination") {
    auto img = solid(2, 2, Pixel{255, 0, 0, 255});
    REQUIRE(d.draw_image(Rect{0, 0, 8, 4}, img).has_value());
    d.flush();
    REQUIRE(out.find("s=2") != std::string::npos);
    REQUIRE(out.find("c=8") != std::string::npos);
    REQUIRE(out.find("r=4") != std::string::npos);
  }

  SECTION("non-square destination catches an axis swap") {
    auto img = solid(4, 4, Pixel{255, 0, 0, 255});
    REQUIRE(d.draw_image(Rect{0, 0, 7, 3}, img).has_value());
    d.flush();
    REQUIRE(out.find("c=7") != std::string::npos);
    REQUIRE(out.find("r=3") != std::string::npos);
    REQUIRE(out.find("c=3") == std::string::npos);
  }
}

TEST_CASE("FallbackDriver: samples the image into the destination rect",
          "[drivers]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // Left half white, right half black, 4 px wide.
  Image img{4, 1,
            {Pixel{255, 255, 255, 255}, Pixel{255, 255, 255, 255},
             Pixel{0, 0, 0, 255}, Pixel{0, 0, 0, 255}}};

  SECTION("downscale: 4 px into 2 cells keeps both halves") {
    REQUIRE(d.draw_image(Rect{0, 0, 2, 1}, img).has_value());
    d.flush();
    // sample_index(0,4,2)=0 -> white '@'; sample_index(1,4,2)=2 -> black ' '.
    REQUIRE(out.find("@ ") != std::string::npos);
  }

  SECTION("upscale: 4 px into 8 cells duplicates each source pixel") {
    REQUIRE(d.draw_image(Rect{0, 0, 8, 1}, img).has_value());
    d.flush();
    REQUIRE(out.find("@@@@    ") != std::string::npos);
  }

  SECTION("non-square destination emits exactly h rows of w glyphs") {
    // Named flat, not solid: `auto solid = solid(...)` would name the
    // variable under construction, not tfsupport::solid.
    auto flat = solid(4, 4, Pixel{255, 255, 255, 255});
    REQUIRE(d.draw_image(Rect{0, 0, 7, 3}, flat).has_value());
    d.flush();
    int rows = 0;
    for (std::size_t p = out.find("\033["); p != std::string::npos;
         p = out.find("\033[", p + 2))
      ++rows;
    REQUIRE(rows == 3);
    REQUIRE(out.find("@@@@@@@") != std::string::npos);
    REQUIRE(out.find("@@@@@@@@") == std::string::npos);  // not 8 wide
  }
}

TEST_CASE("AnsiRgbDriver: samples the image into the destination rect",
          "[drivers]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // Top half red, bottom half blue, 4 px tall.
  Image img{1, 4,
            {Pixel{255, 0, 0, 255}, Pixel{255, 0, 0, 255},
             Pixel{0, 0, 255, 255}, Pixel{0, 0, 255, 255}}};

  SECTION("1 cell tall: 4 source rows sampled into 2") {
    REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, img).has_value());
    d.flush();
    int blocks = 0;
    for (std::size_t p = out.find("\xE2\x96\x80"); p != std::string::npos;
         p = out.find("\xE2\x96\x80", p + 3))
      ++blocks;
    REQUIRE(blocks == 1);
    // dst.h == 2: row 0 -> source 0 (red, fg), row 1 -> source 2 (blue, bg).
    REQUIRE(out.find("38;2;255;0;0") != std::string::npos);
    REQUIRE(out.find("48;2;0;0;255") != std::string::npos);
  }

  SECTION("upscaled: one block per destination cell") {
    REQUIRE(d.draw_image(Rect{0, 0, 3, 4}, img).has_value());
    d.flush();
    int blocks = 0;
    for (std::size_t p = out.find("\xE2\x96\x80"); p != std::string::npos;
         p = out.find("\xE2\x96\x80", p + 3))
      ++blocks;
    REQUIRE(blocks == 12);  // 3 cols x 4 rows
  }
}

// ── #101: the drivers meet a checkerboard ───────────────────────────────────
//
// Every image case above this line feeds a driver a solid, or a source whose
// rows are solid, and that leaves holes. Measured, not assumed — each of
// these was injected into a driver and the suite re-run:
//
//   fallback, rows emitted bottom-to-top   -> 31/31 cases above still PASS
//   ansi, columns emitted right-to-left    -> 31/31 cases above still PASS
//   ansi, half-block pairing swapped       -> 3 above fail (the 1x2 red/blue
//                                             source is not a solid)
//   fallback, columns right-to-left        -> 2 above fail (two substring
//                                             checks pin within-row order)
//
// So two of the four were invisible to the entire suite by construction: a
// solid cannot witness an ordering. checker() is what makes it observable,
// which is why #101 hoisted it out of test/28image — the suite that needed it
// least. All four fail against the cases below.
//
// Three rules the cases below follow, each one of them found by breaking a
// driver on purpose and watching what stayed green:
//   - the image is never square, and its width is EVEN. A checker row of odd
//     width is a palindrome ("@ @"), so a driver that emitted its columns
//     right-to-left passed the first draft of these cases. Width 4 makes each
//     row "@ @ " -- asymmetric -- and a non-square image kills an axis swap.
//   - the assertion is the WHOLE emitted string (or, for kitty, the whole
//     APC frame). out.find("▀") or out.find("a=t") passes on any image at
//     all and pins nothing.
//   - the destination is chosen so no two emitted rows are byte-identical.

TEST_CASE("FallbackDriver: a checkerboard comes out in the right order",
          "[drivers][image][order]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // 4x2 into 4x2 cells is 1:1 (sample_index(i, n, n) == i), so the emitted
  // picture is the image: white -> lum 255 -> ramp[9] == '@', black -> ' '.
  auto img = checker(4, 2, Pixel{255, 255, 255, 255}, Pixel{0, 0, 0, 255});
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();
  // Row 0 is "@ @ " (parity even at x=0), row 1 is " @ @", each preceded by
  // its own 1-based cursor address. Full equality: this is the entire frame.
  REQUIRE(out == "\033[1;1H@ @ \033[2;1H @ @");
}

TEST_CASE("AnsiRgbDriver: a checkerboard pairs upper->fg, lower->bg, row by row",
          "[drivers][image][order]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 4x6 into 4x2 cells, NOT 4x4 — and that is the whole trick. This tier
  // packs two pixel rows into one cell, so with a 1:1 checker every cell row
  // would pair source rows 2k and 2k+1, which have the same parity for every
  // k: all the cell rows would come out byte-identical and a driver that
  // emitted them in reverse order would pass. Sampling 6 source rows into
  // dst.h == 4 gives sample_index -> 0,1,3,4, so cell row 0 pairs rows 0/1
  // and cell row 1 pairs rows 3/4 — opposite parity, colour-inverted rows.
  auto img = checker(4, 6, Pixel{255, 0, 0, 255}, Pixel{0, 0, 255, 255});
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();

  const std::string kRed = "\033[38;2;255;0;0m";
  const std::string kBlueFg = "\033[38;2;0;0;255m";
  const std::string kRedBg = "\033[48;2;255;0;0m";
  const std::string kBlue = "\033[48;2;0;0;255m";
  const std::string kBlock = "\xE2\x96\x80";  // ▀
  // Both SGRs change at every cell, so the run-coalescer in the driver hides
  // nothing here — every colour it computes appears in the output.
  const std::string kUpper = kRed + kBlue + kBlock;      // red over blue
  const std::string kLower = kBlueFg + kRedBg + kBlock;  // blue over red
  // Row 1's FIRST cell carries no SGR at all: the coalescer's state survives
  // the cursor move, and row 0 ended on the same pair row 1 opens with. That
  // bare block is not noise to be papered over — it is only in that position
  // if both rows came out in the order and the phase they should have, so
  // asserting it is asserting the boundary between them.
  const std::string expected = "\033[1;1H" + kUpper + kLower + kUpper + kLower +
                               "\033[2;1H" + kBlock + kUpper + kLower + kUpper +
                               "\033[0m";
  REQUIRE(out == expected);
}

TEST_CASE("KittyDriver: the transmitted payload is row-major, and s=/v= match",
          "[drivers][kitty][image][order]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  // The terminal does the scaling here, so nothing about row order is
  // observable from the placement — the base64 payload is the only witness,
  // and the driver's job is to ship the buffer verbatim (4x2 RGBA = 32 bytes).
  auto img = checker(4, 2, Pixel{10, 20, 30, 255}, Pixel{200, 150, 100, 255});
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();
  // Asserting the whole frame, not just the payload: s=4,v=2 is what catches
  // a width/height transpose, which a payload match alone would let through
  // (a driver that mislabelled the dimensions would still ship these bytes).
  REQUIRE(out.find("\033_Ga=t,t=d,f=32,i=1,s=4,v=2,m=0,q=2;"
                   "ChQe/8iWZP8KFB7/yJZk/8iWZP8KFB7/yJZk/woUHv8=\033\\") !=
          std::string::npos);
}

// ── #83: preferred_pixel_extent ─────────────────────────────────────────────

TEST_CASE("Drivers: preferred_pixel_extent reflects each tier's packing",
          "[drivers]") {
  AnsiRgbDriver ansi;
  FallbackDriver fb;
  KittyDriver kitty;
  const Rect r{0, 0, 10, 4};

  REQUIRE(fb.preferred_pixel_extent(r) == Extent{10, 4});
  REQUIRE(ansi.preferred_pixel_extent(r) == Extent{10, 8});
  // Nominal 8x16 until a terminal says otherwise.
  REQUIRE(kitty.preferred_pixel_extent(r) == Extent{80, 64});

  // A degenerate rect is a legal input that produces no work, matching
  // Rect::empty()'s stance everywhere else.
  REQUIRE(fb.preferred_pixel_extent(Rect{0, 0, 0, 5}) == Extent{});
  REQUIRE(ansi.preferred_pixel_extent(Rect{0, 0, 3, -1}) == Extent{});
  REQUIRE(kitty.preferred_pixel_extent(Rect{}) == Extent{});
}

TEST_CASE("KittyDriver: cell geometry is pushed in, and 0 means nominal",
          "[drivers][kitty][failure]") {
  KittyDriver d;
  const Rect r{0, 0, 10, 4};
  REQUIRE(d.cell_pixel_size() == KittyDriver::kNominalCellPixels);

  d.set_cell_pixel_size(Extent{10, 20});
  REQUIRE(d.preferred_pixel_extent(r) == Extent{100, 80});

  // ws_xpixel == 0 is what tmux, the Linux console and several emulators
  // report. It must not propagate a zero into a divisor, and it must not be
  // an error — a nominal cell is a correctly-shaped guess.
  d.set_cell_pixel_size(Extent{0, 0});
  REQUIRE(d.cell_pixel_size() == KittyDriver::kNominalCellPixels);
  REQUIRE(d.preferred_pixel_extent(r) == Extent{80, 64});

  d.set_cell_pixel_size(Extent{8, -16});
  REQUIRE(d.preferred_pixel_extent(r) == Extent{80, 64});
}

TEST_CASE("Drivers: the base set_cell_pixel_size is a no-op for flat tiers",
          "[drivers]") {
  AnsiRgbDriver ansi;
  FallbackDriver fb;
  const Rect r{0, 0, 10, 4};
  ansi.set_cell_pixel_size(Extent{10, 20});
  fb.set_cell_pixel_size(Extent{10, 20});
  // Neither tier's packing depends on the font: half-blocks are half-blocks.
  REQUIRE(ansi.preferred_pixel_extent(r) == Extent{10, 8});
  REQUIRE(fb.preferred_pixel_extent(r) == Extent{10, 4});
}

// ── #100: ask the driver instead of guessing ────────────────────────────────

TEST_CASE("Drivers: image_cell_extent matches the rows actually emitted",
          "[drivers]") {
  // The assertion that would have caught the shipped bug: both examples used
  // to derive this from capability flags, which describe colour and not
  // packing, and got the fallback tier wrong.
  auto img = solid(8, 9, Pixel{255, 255, 255, 255});

  SECTION("fallback: one glyph per cell, so h rows of w glyphs") {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    const Extent e = d.image_cell_extent(img);
    REQUIRE(e == Extent{8, 9});
    REQUIRE(d.draw_image(Rect{0, 0, e.w, e.h}, img).has_value());
    d.flush();
    int rows = 0;
    for (std::size_t p = out.find("\033["); p != std::string::npos;
         p = out.find("\033[", p + 2))
      ++rows;
    REQUIRE(rows == e.h);
  }

  SECTION("ansi: two pixel rows per cell, rounding up") {
    AnsiRgbDriver d;
    std::string out;
    d.set_output(&out);
    const Extent e = d.image_cell_extent(img);
    REQUIRE(e == Extent{8, 5});  // 9 pixel rows -> 5 cells
    REQUIRE(d.draw_image(Rect{0, 0, e.w, e.h}, img).has_value());
    d.flush();
    int blocks = 0;
    for (std::size_t p = out.find("\xE2\x96\x80"); p != std::string::npos;
         p = out.find("\xE2\x96\x80", p + 3))
      ++blocks;
    REQUIRE(blocks == e.w * e.h);
  }

  SECTION("kitty: the terminal's cell geometry, rounding up") {
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    const Extent e = d.image_cell_extent(img);
    REQUIRE(e == Extent{1, 1});  // 8x9 px in a nominal 8x16 cell
    REQUIRE(d.draw_image(Rect{0, 0, e.w, e.h}, img).has_value());
    d.flush();
    REQUIRE(out.find("c=1") != std::string::npos);
    REQUIRE(out.find("r=1") != std::string::npos);

    d.set_cell_pixel_size(Extent{4, 3});
    REQUIRE(d.image_cell_extent(img) == Extent{2, 3});
  }

  SECTION("an empty image occupies nothing on every tier") {
    FallbackDriver fb;
    AnsiRgbDriver ansi;
    KittyDriver kitty;
    REQUIRE(fb.image_cell_extent(Image{}) == Extent{});
    REQUIRE(ansi.image_cell_extent(Image{}) == Extent{});
    REQUIRE(kitty.image_cell_extent(Image{}) == Extent{});
  }
}

TEST_CASE("Drivers: an empty destination rect is a warning, not a crash",
          "[drivers][failure]") {
  auto img = solid(2, 2, Pixel{255, 0, 0, 255});
  FallbackDriver fb;
  AnsiRgbDriver ansi;
  KittyDriver kitty;
  REQUIRE_FALSE(fb.draw_image(Rect{0, 0, 0, 4}, img).has_value());
  REQUIRE_FALSE(ansi.draw_image(Rect{0, 0, 4, 0}, img).has_value());
  REQUIRE_FALSE(kitty.draw_image(Rect{}, img).has_value());
}
