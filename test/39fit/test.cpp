// PlacementFit (#137) — opting out of stretch-to-fill.
//
// #83 made stretch-to-fill the contract, and for content an application
// GENERATES that is right: it can re-rasterize at preferred_pixel_extent. For
// content an application SHIPS it is a silent corruption -- a QR module grid
// stretched by 1.0125 still looks approximately right and stops scanning, and
// an ordered dither resampled at a non-integer ratio beats against its own
// period into moiré.
//
// So the assertions here are mostly about ABSENCE: that c=/r= are not on the
// placement, that a cell outside the image's cover is not painted, that a
// refused call emitted nothing at all. Absence is where a test can most easily
// be green for the wrong reason, which is why this suite PARSES the escape
// stream (support/apc.hpp) instead of running out.find() over it -- a bare
// find("c=") cannot tell a placement key from the same two characters
// somewhere else in the same buffer.
//
// All offline. set_output redirects every driver away from stdout.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::Capabilities;
using termforge::EncodedImage;
using termforge::ErrorEvent;
using termforge::Extent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::ImageFormat;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::PlacementFit;
using termforge::Rect;
using termforge::Rgb;
using termforge::Severity;
using termforge::TerminalDriver;
using tfsupport::Apc;
using tfsupport::apcs;
using tfsupport::checker;
using tfsupport::count_of;
using tfsupport::has_key;
using tfsupport::key_value;
using tfsupport::LegacyDriver;
using tfsupport::placements;
using tfsupport::reassemble;
using tfsupport::solid;

namespace {

constexpr Pixel kA{10, 20, 30, 255};
constexpr Pixel kB{200, 150, 100, 255};

// Kitty's nominal cell is 8x16 until App pushes real geometry (and no test
// here pushes any), so every rect below is sized against that.

// A driver that CLAIMS Exact but never implements it: it overrides the query
// and not the emit path. The base must still refuse -- see test 9.
class FitClaimingDriver final : public TerminalDriver {
 public:
  auto init() -> std::expected<void, ErrorEvent> override { return {}; }
  auto draw_text(int, int, std::string_view, Rgb, Rgb, Attr) -> void override {}
  auto draw_image(Rect, const Image&)
      -> std::expected<void, ErrorEvent> override {
    m_drew_image = true;
    return {};
  }
  // The #163-era EncodedImage overload, and nothing more -- this driver is a
  // stand-in for one that shipped a pre-encoded path before #169 existed. No
  // `using TerminalDriver::draw_image;`: every call in this suite goes through
  // TerminalDriver&, and unhiding would change what the cases exercise.
  auto draw_image(Rect, const EncodedImage&)
      -> std::expected<void, ErrorEvent> override {
    m_drew_encoded = true;
    return {};
  }
  [[nodiscard]] auto supports_placement_fit(PlacementFit) const noexcept
      -> bool override {
    return true;  // a lie, and the base must not take its word for it
  }
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override {
    return Extent{cells.w, cells.h};
  }
  auto flush() -> void override { tally_frame(0); }
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override {
    return Capabilities{};
  }
  [[nodiscard]] auto drew_image() const -> bool { return m_drew_image; }
  [[nodiscard]] auto drew_encoded() const -> bool { return m_drew_encoded; }

 private:
  bool m_drew_image{false};
  bool m_drew_encoded{false};
};

// Bytes for the cases that never reach a wire. Their content is irrelevant --
// the drivers below refuse before looking -- but they must be non-empty in
// both fields, or EncodedImage::empty() would refuse them for the wrong
// reason and the message assertions would pass vacuously.
constexpr std::array<unsigned char, 4> kOpaque{0xDE, 0xAD, 0xBE, 0xEF};

auto opaque() -> EncodedImage {
  return EncodedImage{ImageFormat::Png, std::as_bytes(std::span{kOpaque}),
                      Extent{2, 2}};
}

// One kitty frame's output for a single draw.
auto kitty_frame(Rect dest, const Image& img, PlacementFit fit,
                 std::string* out) -> std::expected<void, ErrorEvent> {
  KittyDriver d;
  d.set_output(out);
  auto r = d.draw_image(dest, img, fit);
  d.flush();
  return r;
}

}  // namespace

// ── 1. the placement escape omits c= and r= under Exact ─────────────────────

TEST_CASE("Exact omits c= and r= from the placement") {
  // 30x30 px into a 4x2 cell rect: 32x32 px of room at the nominal cell, so
  // the image fits and is deliberately NOT a whole number of cells. A test
  // image that happened to be an exact multiple could not distinguish the two
  // fits at all.
  const Image img = checker(30, 30, kA, kB);
  std::string out;
  REQUIRE(kitty_frame(Rect{0, 0, 4, 2}, img, PlacementFit::Exact, &out));

  const auto places = placements(out);
  REQUIRE(places.size() == 1);
  // The whole ticket, in two lines.
  CHECK_FALSE(has_key(places[0], "c"));
  CHECK_FALSE(has_key(places[0], "r"));
  // ...and the rest of the placement is intact, so "no c=/r=" was not achieved
  // by emitting a broken or empty command.
  CHECK(has_key(places[0], "a"));
  CHECK(key_value(places[0], "a") == "p");
  CHECK(has_key(places[0], "i"));
  CHECK(has_key(places[0], "p"));
  CHECK(key_value(places[0], "C") == "1");
  CHECK(key_value(places[0], "q") == "2");
}

TEST_CASE("Stretch still carries c= and r= naming the destination rect") {
  const Image img = checker(30, 30, kA, kB);
  std::string out;
  REQUIRE(kitty_frame(Rect{0, 0, 4, 2}, img, PlacementFit::Stretch, &out));

  const auto places = placements(out);
  REQUIRE(places.size() == 1);
  CHECK(key_value(places[0], "c") == "4");
  CHECK(key_value(places[0], "r") == "2");
}

// ── 2. the payload is untouched by the fit ──────────────────────────────────

TEST_CASE("Exact transmits the source pixels whole, at the declared extent") {
  // This is the test that must NOT be relied on for test 1. A mutation that
  // makes place_classic emit c=/r= under Exact leaves this one green: the
  // pixels are transmitted correctly and only then scaled at placement time.
  // The assertion that bites has to be on the placement escape.
  const Image img = checker(30, 30, kA, kB);
  std::string out;
  REQUIRE(kitty_frame(Rect{0, 0, 4, 2}, img, PlacementFit::Exact, &out));

  const auto want = std::as_bytes(img.pixels());
  const auto got = reassemble(out);
  REQUIRE(got.size() == want.size());
  CHECK(std::equal(got.begin(), got.end(), want.begin()));

  // s=/v= are the SOURCE's extent, not the destination's -- under Exact that
  // is the whole basis on which the terminal decides how big to draw it.
  const auto all = apcs(out);
  const auto* opener = std::ranges::find_if(all, [](const Apc& a) {
    return a.keys.find("a=t") != std::string::npos;
  }).base();
  REQUIRE(opener != all.data() + all.size());
  CHECK(key_value(*opener, "s") == "30");
  CHECK(key_value(*opener, "v") == "30");
}

// ── 3. Stretch is byte-for-byte what the two-argument overload emits ────────

TEST_CASE("the 2-arg overload and Stretch emit identical bytes, every tier") {
  const Image img = checker(30, 17, kA, kB);
  const Rect dest{2, 1, 5, 3};

  SECTION("kitty") {
    std::string a, b;
    KittyDriver da;
    da.set_output(&a);
    REQUIRE(da.draw_image(dest, img));
    da.flush();

    KittyDriver db;
    db.set_output(&b);
    REQUIRE(db.draw_image(dest, img, PlacementFit::Stretch));
    db.flush();
    CHECK(a == b);
    CHECK_FALSE(a.empty());
  }
  SECTION("ansi_rgb") {
    std::string a, b;
    AnsiRgbDriver da;
    da.set_output(&a);
    REQUIRE(da.draw_image(dest, img));
    da.flush();

    AnsiRgbDriver db;
    db.set_output(&b);
    REQUIRE(db.draw_image(dest, img, PlacementFit::Stretch));
    db.flush();
    CHECK(a == b);
    CHECK_FALSE(a.empty());
  }
  SECTION("fallback") {
    std::string a, b;
    FallbackDriver da;
    da.set_output(&a);
    REQUIRE(da.draw_image(dest, img));
    da.flush();

    FallbackDriver db;
    db.set_output(&b);
    REQUIRE(db.draw_image(dest, img, PlacementFit::Stretch));
    db.flush();
    CHECK(a == b);
    CHECK_FALSE(a.empty());
  }
}

// ── 4. an image too large for the rect is refused, and nothing is emitted ───

TEST_CASE("Exact refuses an image larger than the rect, emitting nothing") {
  // 40x40 px needs 5x3 cells at 8x16; a 2x1 rect holds 16x16.
  const Image img = checker(40, 40, kA, kB);
  const Rect dest{0, 0, 2, 1};

  SECTION("kitty: no transmit AND no placement") {
    std::string out;
    KittyDriver d;
    d.set_output(&out);
    const auto r = d.draw_image(dest, img, PlacementFit::Exact);
    d.flush();

    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "kitty");
    // The ordering assertion. validate_fit running AFTER draw_payload would
    // still refuse -- and would already have paid for the upload, which is the
    // most expensive possible way to draw nothing.
    CHECK(count_of(out, "a=t") == 0);
    CHECK(count_of(out, "a=p") == 0);
    CHECK(out.empty());
  }
  SECTION("ansi_rgb") {
    std::string out;
    AnsiRgbDriver d;
    d.set_output(&out);
    const auto r = d.draw_image(dest, img, PlacementFit::Exact);
    d.flush();
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "ansi_rgb");
    CHECK(out.empty());
  }
  SECTION("fallback") {
    std::string out;
    FallbackDriver d;
    d.set_output(&out);
    const auto r = d.draw_image(dest, img, PlacementFit::Exact);
    d.flush();
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "fallback");
    CHECK(out.empty());
  }
  SECTION("the message names both extents, so a caller can act on it") {
    std::string out;
    KittyDriver d;
    d.set_output(&out);
    const auto r = d.draw_image(dest, img, PlacementFit::Exact);
    REQUIRE_FALSE(r);
    const std::string msg = r.error().message;
    CHECK(msg.find("40x40") != std::string::npos);     // what was asked
    CHECK(msg.find("16x16") != std::string::npos);     // what there was room for
    CHECK(msg.find("Exact") != std::string::npos);
  }
  SECTION("it fires EVERY frame, not once") {
    // Unlike the 297-cell clamp, which latches: a clamp still draws, degraded,
    // so repeating it is noise. A refusal draws nothing, so every frame it
    // happens is a real hole in the UI.
    std::string out;
    KittyDriver d;
    d.set_output(&out);
    for (int i = 0; i < 3; ++i) {
      const auto r = d.draw_image(dest, img, PlacementFit::Exact);
      REQUIRE_FALSE(r);
      CHECK(r.error().severity == Severity::Warning);
      d.flush();
    }
  }
}

// ── 5. at an exact cell multiple the two fits agree ─────────────────────────

TEST_CASE("Exact and Stretch coincide when the image is a whole cell rect") {
  SECTION("kitty: same transmit, placement differs only by c=/r=") {
    const Image img = checker(32, 32, kA, kB);  // 4x2 cells at 8x16
    const Rect dest{0, 0, 4, 2};
    std::string s, e;
    REQUIRE(kitty_frame(dest, img, PlacementFit::Stretch, &s));
    REQUIRE(kitty_frame(dest, img, PlacementFit::Exact, &e));

    CHECK(reassemble(s) == reassemble(e));
    const auto ps = placements(s);
    const auto pe = placements(e);
    REQUIRE(ps.size() == 1);
    REQUIRE(pe.size() == 1);
    CHECK(has_key(ps[0], "c"));
    CHECK_FALSE(has_key(pe[0], "c"));
    CHECK(key_value(ps[0], "i") == key_value(pe[0], "i"));
  }
  SECTION("ansi_rgb: byte-identical output") {
    // The half-block tier's pixel grid for 4x2 cells is 4x4.
    const Image img = checker(4, 4, kA, kB);
    const Rect dest{0, 0, 4, 2};
    std::string s, e;
    AnsiRgbDriver ds;
    ds.set_output(&s);
    REQUIRE(ds.draw_image(dest, img, PlacementFit::Stretch));
    ds.flush();
    AnsiRgbDriver de;
    de.set_output(&e);
    REQUIRE(de.draw_image(dest, img, PlacementFit::Exact));
    de.flush();
    CHECK(s == e);
    CHECK_FALSE(s.empty());
  }
  SECTION("fallback: byte-identical output") {
    const Image img = checker(4, 2, kA, kB);
    const Rect dest{0, 0, 4, 2};
    std::string s, e;
    FallbackDriver ds;
    ds.set_output(&s);
    REQUIRE(ds.draw_image(dest, img, PlacementFit::Stretch));
    ds.flush();
    FallbackDriver de;
    de.set_output(&e);
    REQUIRE(de.draw_image(dest, img, PlacementFit::Exact));
    de.flush();
    CHECK(s == e);
    CHECK_FALSE(s.empty());
  }
}

// ── 6. the slot cache must not swallow a fit change ─────────────────────────

TEST_CASE("changing only the fit re-places, without retransmitting") {
  // The bug the issue body does not mention. region_key is the destination
  // geometry and payload_hash is the content, so the SAME image to the SAME
  // rect under a new fit matches both -- and without slot.fit the driver would
  // emit nothing at all on the second frame. The opt-out would silently fail
  // to take, which is indistinguishable from the bug it exists to fix.
  const Image img = checker(30, 30, kA, kB);
  const Rect dest{0, 0, 4, 2};

  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(dest, img, PlacementFit::Stretch));
  d.flush();
  REQUIRE(count_of(out, "a=t") == 1);
  REQUIRE(placements(out).size() == 1);
  REQUIRE(key_value(placements(out)[0], "c") == "4");

  out.clear();
  REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));
  d.flush();

  // Something was emitted at all -- the assertion that kills the whole bug.
  CHECK_FALSE(out.empty());
  const auto places = placements(out);
  REQUIRE(places.size() == 1);
  CHECK_FALSE(has_key(places[0], "c"));
  CHECK_FALSE(has_key(places[0], "r"));
  // The old classic placement is deleted first: kitty does not refresh one in
  // place, so re-placing without deleting leaves both live.
  CHECK(count_of(out, "a=d,d=i") == 1);
  // And NOT by retransmitting. Folding the fit into payload_hash would also
  // make the placement come out right, at the cost of re-uploading the payload
  // to change a ~30-byte escape -- and would bill those bytes to
  // image_transmit, lying to #139's meter.
  CHECK(count_of(out, "a=t") == 0);
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit > 0);
}

TEST_CASE("the fit change is detected in both directions") {
  const Image img = checker(30, 30, kA, kB);
  const Rect dest{0, 0, 4, 2};
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));
  d.flush();
  out.clear();

  REQUIRE(d.draw_image(dest, img, PlacementFit::Stretch));
  d.flush();
  const auto places = placements(out);
  REQUIRE(places.size() == 1);
  CHECK(key_value(places[0], "c") == "4");
  CHECK(key_value(places[0], "r") == "2");
  CHECK(count_of(out, "a=t") == 0);
}

TEST_CASE("an unchanged fit still emits nothing on a repeat draw") {
  // The other half of the contract: slot.fit must not make every frame
  // re-place. Emit-on-change is what keeps an idle frame free.
  const Image img = checker(30, 30, kA, kB);
  const Rect dest{0, 0, 4, 2};
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));
  d.flush();
  out.clear();
  REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));
  d.flush();
  CHECK(out.empty());
}

// ── 7. supports_placement_fit, and that it agrees with what happens ─────────

TEST_CASE("supports_placement_fit answers for each tier") {
  KittyDriver k;
  AnsiRgbDriver a;
  FallbackDriver f;
  CHECK(k.supports_placement_fit(PlacementFit::Stretch));
  CHECK(k.supports_placement_fit(PlacementFit::Exact));
  CHECK(a.supports_placement_fit(PlacementFit::Stretch));
  CHECK(a.supports_placement_fit(PlacementFit::Exact));
  CHECK(f.supports_placement_fit(PlacementFit::Stretch));
  CHECK(f.supports_placement_fit(PlacementFit::Exact));
}

TEST_CASE("the query and the emit path cannot disagree") {
  // validate_fit asks the driver's own supports_placement_fit, so a tier that
  // says it cannot do something structurally also refuses to try. Removing
  // that gate is the mutation this case exists for.
  const Image img = checker(8, 16, kA, kB);
  const Rect dest{0, 0, 1, 1};

  KittyDriver k;
  std::string out;
  k.set_output(&out);
  CHECK(k.draw_image(dest, img, PlacementFit::Exact).has_value() ==
        k.supports_placement_fit(PlacementFit::Exact));

  k.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  out.clear();
  CHECK(k.draw_image(dest, img, PlacementFit::Exact).has_value() ==
        k.supports_placement_fit(PlacementFit::Exact));
}

TEST_CASE("Exact is refused under Unicode placeholders, and says so") {
  const Image img = checker(8, 16, kA, kB);
  const Rect dest{0, 0, 1, 1};
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  CHECK_FALSE(d.supports_placement_fit(PlacementFit::Exact));
  CHECK(d.supports_placement_fit(PlacementFit::Stretch));

  const auto r = d.draw_image(dest, img, PlacementFit::Exact);
  d.flush();
  REQUIRE_FALSE(r);
  CHECK(r.error().severity == Severity::Warning);
  CHECK(r.error().source == "kitty");
  CHECK(r.error().message.find("Exact") != std::string::npos);
  // Nothing at all: no virtual placement, and no placeholder cells. A grid
  // painted for a placement that was never created is worse than no draw.
  CHECK(out.empty());
  CHECK(count_of(out, "U=1") == 0);
  CHECK(count_of(out, "\xF4\x8F\xBB\xAE") == 0);

  // Stretch in the same mode still works, so the refusal is about the fit and
  // not about the mode being broken.
  out.clear();
  REQUIRE(d.draw_image(dest, img, PlacementFit::Stretch));
  d.flush();
  CHECK(count_of(out, "U=1") == 1);
}

TEST_CASE("the answer tracks set_placement_mode at runtime") {
  // An application cannot infer this from the driver's type, which is why it
  // is a query and not a compile-time property.
  const Image img = checker(8, 16, kA, kB);
  const Rect dest{0, 0, 1, 1};
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.supports_placement_fit(PlacementFit::Exact));
  REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));

  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  CHECK_FALSE(d.supports_placement_fit(PlacementFit::Exact));
  CHECK_FALSE(d.draw_image(dest, img, PlacementFit::Exact));

  d.set_placement_mode(KittyDriver::PlacementMode::Classic);
  CHECK(d.supports_placement_fit(PlacementFit::Exact));
  CHECK(d.draw_image(dest, img, PlacementFit::Exact));
}

// ── 8. a driver written before #137 still compiles, and gets an honest no ───

TEST_CASE("a legacy driver still compiles and is refused honestly") {
  // If the new virtual were PURE this file would not compile, on the
  // LegacyDriver line -- which is the entire argument for the design. Nothing
  // else in test/ derives from TerminalDriver, so without this class a pure
  // virtual would sail through CI and break every out-of-tree driver.
  LegacyDriver legacy;
  static_assert(termforge::DriverImpl<LegacyDriver>);
  TerminalDriver& base = legacy;
  const Image img = solid(2, 2, kA);
  const Rect dest{0, 0, 2, 2};

  SECTION("Stretch delegates to the driver's OWN two-argument override") {
    // Not merely "returns success": the base default must route through the
    // pure virtual the legacy driver actually implements, so its existing
    // behaviour is what runs.
    REQUIRE(base.draw_image(dest, img, PlacementFit::Stretch));
    CHECK(legacy.drew_image());
  }
  SECTION("Exact is refused, and does not reach the driver") {
    const auto r = base.draw_image(dest, img, PlacementFit::Exact);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "driver");
    CHECK_FALSE(legacy.drew_image());
  }
  SECTION("the inherited query is Stretch-only") {
    CHECK(base.supports_placement_fit(PlacementFit::Stretch));
    CHECK_FALSE(base.supports_placement_fit(PlacementFit::Exact));
  }
}

// ── 9. claiming Exact is not implementing it ────────────────────────────────

TEST_CASE("a driver that claims Exact but does not implement it still warns") {
  // The base default tests the ENUM, not supports_placement_fit(). Routing it
  // through the query would mean this driver silently gets a STRETCH -- the
  // exact bug #137 exists to remove, reintroduced one level up. That is why
  // this one place deliberately departs from #163's shared-branch rule.
  FitClaimingDriver claimer;
  TerminalDriver& base = claimer;
  const Image img = solid(2, 2, kA);

  CHECK(base.supports_placement_fit(PlacementFit::Exact));  // it lies
  const auto r = base.draw_image(Rect{0, 0, 2, 2}, img, PlacementFit::Exact);
  REQUIRE_FALSE(r);
  CHECK(r.error().severity == Severity::Warning);
  CHECK_FALSE(claimer.drew_image());
}

// ── 10. the identity map on the resampling tiers ────────────────────────────

TEST_CASE("Exact maps source to destination 1:1 on the fallback tiers") {
  SECTION("fallback: an image smaller than the rect covers only its own area") {
    // 3x2 source into a 6x4 cell rect. Under Stretch every one of the 24 cells
    // is painted; under Exact only 6 are, and the rest are left alone.
    const Image img = checker(3, 2, kA, kB);
    const Rect dest{0, 0, 6, 4};
    std::string s, e;

    FallbackDriver ds;
    ds.set_output(&s);
    REQUIRE(ds.draw_image(dest, img, PlacementFit::Stretch));
    ds.flush();

    FallbackDriver de;
    de.set_output(&e);
    REQUIRE(de.draw_image(dest, img, PlacementFit::Exact));
    de.flush();

    // The painted glyph grid: one string per emitted row, escapes stripped.
    auto grid = [](std::string_view v) {
      std::vector<std::string> rows;
      for (std::size_t i = 0; i < v.size();) {
        if (v[i] == '\033') {
          const std::size_t h = v.find('H', i);
          REQUIRE(h != std::string_view::npos);
          i = h + 1;
          rows.emplace_back();
        } else {
          REQUIRE_FALSE(rows.empty());
          rows.back().push_back(v[i]);
          ++i;
        }
      }
      return rows;
    };

    const auto gs = grid(s);
    const auto ge = grid(e);
    // How much was painted: the whole rect under Stretch, only the image's own
    // area under Exact.
    REQUIRE(gs.size() == 4);
    CHECK(gs[0].size() == 6);
    REQUIRE(ge.size() == 2);
    CHECK(ge[0].size() == 3);

    // ...and WHICH glyphs. Counting alone is not enough: a resample paints the
    // right NUMBER of wrong pixels, and this assertion is what catches it.
    //
    // Stated as the checker's own parity rather than against a hardcoded ramp
    // table -- re-deriving the expected glyph through luminance_char would put
    // both sides of the assertion on one function and prove nothing.
    REQUIRE(ge[0][0] != ge[0][1]);  // adjacent source pixels differ at all
    CHECK(ge[0][2] == ge[0][0]);    // (0,0) and (2,0) are the same parity
    CHECK(ge[1][0] == ge[0][1]);    // the next row is the opposite parity
    CHECK(ge[1][1] == ge[0][0]);
    CHECK(ge[1][2] == ge[0][1]);
  }

  SECTION("ansi_rgb: a smaller source is placed 1:1, not spread over the rect") {
    // 2x2 source into a 4x2 rect, whose half-block pixel grid is 4x4. Under
    // Exact only the top-left 2x2 is painted and each destination pixel is its
    // own source pixel; under a resample every one of them would come from
    // source column 0, so the checker's second colour never reaches the wire.
    const Image img = checker(2, 2, kA, kB);
    std::string out;
    AnsiRgbDriver d;
    d.set_output(&out);
    REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img, PlacementFit::Exact));
    d.flush();

    CHECK(out.find("38;2;10;20;30") != std::string::npos);
    CHECK(out.find("38;2;200;150;100") != std::string::npos);
    // One row-pair, two columns.
    CHECK(count_of(out, "\xE2\x96\x80") == 2);
  }

  SECTION("ansi_rgb: a checker survives at 1:1 and is not resampled") {
    // A checker, not a solid: a solid cannot detect a stride or pairing bug
    // because every wrong pixel looks like every right one (#101).
    //
    // 4x4 source into a 4x2 rect is exactly this tier's grid, so Exact and
    // Stretch must agree; the case above already pins the smaller-source path.
    const Image img = checker(4, 4, kA, kB);
    const Rect dest{0, 0, 4, 2};
    std::string s, e;
    AnsiRgbDriver ds;
    ds.set_output(&s);
    REQUIRE(ds.draw_image(dest, img, PlacementFit::Stretch));
    ds.flush();
    AnsiRgbDriver de;
    de.set_output(&e);
    REQUIRE(de.draw_image(dest, img, PlacementFit::Exact));
    de.flush();
    CHECK(s == e);
  }

  SECTION("ansi_rgb: an odd-height source pads rather than duplicating a row") {
    // 2x3 into a 2x2 rect (a 2x4 pixel grid). The bottom cell's upper half is
    // source row 2; its lower half is outside the image. Clamping to row 1
    // would DUPLICATE a source row, which is the artifact Exact exists to
    // prevent -- so the lower half is transparent black.
    Image img = checker(2, 3, kA, kB);
    const Rect dest{0, 0, 2, 2};
    std::string out;
    AnsiRgbDriver d;
    d.set_output(&out);
    REQUIRE(d.draw_image(dest, img, PlacementFit::Exact));
    d.flush();

    // The last row's background must be black (0,0,0), and must NOT be the
    // colour of source row 1 that a clamp would have duplicated.
    const std::size_t last_row = out.rfind("\033[2;1H");
    REQUIRE(last_row != std::string::npos);
    const std::string tail = out.substr(last_row);
    CHECK(tail.find("48;2;0;0;0") != std::string::npos);
  }
}

// ── 11. the ceiling that was a hazard is the guardrail ──────────────────────

TEST_CASE("image_cell_extent sizes a rect that Exact always accepts") {
  // The issue's nicest consequence, as an assertion. image_cell_extent rounds
  // UP, so the rect it names always has at least as many pixels as the image
  // -- the same ceiling that GUARANTEED a stretch under the old contract now
  // guarantees a fit under the new one. Under Stretch this helper was a
  // hazard; under Exact it is the documented safe call site.
  const int sizes[][2] = {{30, 30}, {33, 17}, {1, 1},  {8, 9},
                          {300, 1}, {7, 15},  {16, 32}};

  for (const auto& wh : sizes) {
    const Image img = checker(wh[0], wh[1], kA, kB);
    INFO("image " << wh[0] << "x" << wh[1]);

    {
      KittyDriver d;
      std::string out;
      d.set_output(&out);
      const Extent ext = d.image_cell_extent(img);
      CHECK(d.draw_image(Rect{0, 0, ext.w, ext.h}, img, PlacementFit::Exact));
    }
    {
      AnsiRgbDriver d;
      std::string out;
      d.set_output(&out);
      const Extent ext = d.image_cell_extent(img);
      CHECK(d.draw_image(Rect{0, 0, ext.w, ext.h}, img, PlacementFit::Exact));
    }
    {
      FallbackDriver d;
      std::string out;
      d.set_output(&out);
      const Extent ext = d.image_cell_extent(img);
      CHECK(d.draw_image(Rect{0, 0, ext.w, ext.h}, img, PlacementFit::Exact));
    }
  }
}

TEST_CASE("the guardrail holds at a non-nominal cell size too") {
  // The kitty tier answers from what it BELIEVES the cell size is, which is
  // the nominal 8x16 until App pushes TIOCGWINSZ geometry. The point is that
  // image_cell_extent and validate_fit consult the SAME belief, so the safe
  // call site stays safe whatever it currently is.
  const Image img = checker(30, 30, kA, kB);
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  d.set_cell_pixel_size(Extent{4, 3});
  const Extent ext = d.image_cell_extent(img);
  CHECK(ext.w == 8);   // ceil(30/4)
  CHECK(ext.h == 10);  // ceil(30/3)
  CHECK(d.draw_image(Rect{0, 0, ext.w, ext.h}, img, PlacementFit::Exact));

  // One cell short in either axis is refused, which is what makes the case
  // above an assertion rather than a tautology.
  CHECK_FALSE(d.draw_image(Rect{0, 0, ext.w - 1, ext.h}, img,
                           PlacementFit::Exact));
  CHECK_FALSE(d.draw_image(Rect{0, 0, ext.w, ext.h - 1}, img,
                           PlacementFit::Exact));
}

// ── 12. the base default on the EncodedImage overload (#169) ────────────────
//
// These two cases assert the BASE, not any driver, and they were written to
// pass against the base-class commit alone -- which is what makes them an
// argument that the default is right independently of who overrides it.

TEST_CASE("encoded: a driver written before #169 still compiles and degrades") {
  // If the new virtual were PURE this file would not compile, on the
  // LegacyDriver line. That is the whole reason this class exists, and it is
  // the only mutation in the suite whose failure mode is a build error.
  LegacyDriver legacy;
  static_assert(termforge::DriverImpl<LegacyDriver>);
  TerminalDriver& base = legacy;
  const EncodedImage img = opaque();
  const Rect dest{0, 0, 2, 2};

  // The two sections are told apart by the MESSAGE, not by the failure. Both
  // refuse, and a test that only checked `REQUIRE_FALSE` would stay green if
  // the Stretch branch stopped delegating -- so the message is what pins that
  // Stretch went THROUGH the #163 overload and Exact never did.
  SECTION("Stretch delegates into the driver's own two-argument path") {
    const auto r = base.draw_image(dest, img, PlacementFit::Stretch);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "driver");
    CHECK(r.error().message ==
          "draw_image: this tier cannot transmit a pre-encoded image");
  }
  SECTION("Exact is refused by the new default, before any delegation") {
    const auto r = base.draw_image(dest, img, PlacementFit::Exact);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "driver");
    CHECK(r.error().message ==
          "draw_image: this tier cannot place with PlacementFit::Exact");
  }
  SECTION("the overload it DOES implement is untouched") {
    REQUIRE(base.draw_image(dest, solid(2, 2, kA), PlacementFit::Stretch));
    CHECK(legacy.drew_image());
  }
}

TEST_CASE("encoded: claiming Exact is not implementing it here either") {
  // The #163-era driver that overrode the two-argument EncodedImage overload
  // and the query, but not the new one. Same doctrine as case 9: the base
  // default branches on the ENUM, so this driver's lie cannot turn Exact into
  // a silent Stretch through a path it never implemented.
  const EncodedImage img = opaque();
  const Rect dest{0, 0, 2, 2};

  SECTION("Stretch reaches the driver, so the next section is not vacuous") {
    FitClaimingDriver claimer;
    TerminalDriver& base = claimer;
    CHECK(base.supports_placement_fit(PlacementFit::Exact));  // it lies
    REQUIRE(base.draw_image(dest, img, PlacementFit::Stretch));
    CHECK(claimer.drew_encoded());
  }
  SECTION("Exact is refused, and does not reach the driver") {
    FitClaimingDriver claimer;
    TerminalDriver& base = claimer;
    const auto r = base.draw_image(dest, img, PlacementFit::Exact);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK_FALSE(claimer.drew_encoded());
  }
}

// ── the empty guards are unchanged by the new parameter ─────────────────────

TEST_CASE("empty image and empty rect still refuse under either fit") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = checker(4, 4, kA, kB);

  for (const PlacementFit fit : {PlacementFit::Stretch, PlacementFit::Exact}) {
    const auto empty_img = d.draw_image(Rect{0, 0, 2, 2}, Image{}, fit);
    REQUIRE_FALSE(empty_img);
    CHECK(empty_img.error().message == "draw_image: empty image");

    const auto empty_rect = d.draw_image(Rect{0, 0, 0, 0}, img, fit);
    REQUIRE_FALSE(empty_rect);
    CHECK(empty_rect.error().message == "draw_image: empty destination rect");
  }
  d.flush();
  CHECK(out.empty());
}
