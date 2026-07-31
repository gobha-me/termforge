// Pre-encoded image payloads (#163) — EncodedImage and the f=100 path.
//
// The premise of this suite is that the library is a COURIER for these bytes.
// It does not encode them, does not decode them, and does not have an opinion
// about what is inside them; the application's asset pipeline owns all of
// that, which is the only reason a compressed wire format can exist here at
// all without violating the stdlib-only rule.
//
// So the assertions are about the envelope, not the contents: which f= value
// went out, that the payload arrives whole and in order on the other side of
// the chunker, that identity is keyed on the bytes rather than on anything we
// would have to parse to know, and that a tier which cannot carry a format
// says so instead of guessing.
//
// All offline. set_output redirects every driver away from stdout, so nothing
// here needs a tty.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "detail/base64.hpp"
#include "support/image.hpp"
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
using termforge::Rect;
using termforge::Rgb;
using termforge::Severity;
using termforge::TerminalDriver;
using tfsupport::checker;
using tfsupport::solid;

namespace {

// ── a real PNG ──────────────────────────────────────────────────────────────
//
// 2x2, colour type 3 (4-entry palette), zlib-compressed IDAT. Baked offline
// and checked in as bytes on purpose: the library never parses this, so
// generating it at runtime would mean ~100 lines of CRC-32 and Adler-32 in
// test support buying realism that no assertion in this file can observe.
// What it is for is the cases that want a payload a real terminal would
// actually accept, so that what the driver emits is a thing kitty can be
// asked about in tools/png_repro.sh.
constexpr std::array<unsigned char, 95> kPng2x2{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x08, 0x03, 0x00, 0x00, 0x00, 0x45, 0x68, 0xFD, 0x16,
    0x00, 0x00, 0x00, 0x0C, 0x50, 0x4C, 0x54, 0x45, 0xFF, 0x00, 0x00,
    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xD6, 0x02,
    0x8F, 0x7B, 0x00, 0x00, 0x00, 0x0E, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xDA, 0x63, 0x60, 0x60, 0x64, 0x60, 0x62, 0x06, 0x00, 0x00, 0x11,
    0x00, 0x07, 0x83, 0xCA, 0x64, 0x64, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

auto png_bytes() -> std::span<const std::byte> {
  return std::as_bytes(std::span{kPng2x2});
}

auto as_span(const std::vector<std::byte>& v) -> std::span<const std::byte> {
  return std::span<const std::byte>{v};
}

// A payload of `n` bytes that is not all one value, so a chunker that drops or
// reorders a chunk cannot pass by accident.
auto blob(std::size_t n, unsigned seed = 0) -> std::vector<std::byte> {
  std::vector<std::byte> v;
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    v.push_back(static_cast<std::byte>((i * 31U + seed * 7U + 13U) & 0xFFU));
  }
  return v;
}

// ── reading the wire back ───────────────────────────────────────────────────

struct Apc {
  std::string keys;     // everything before the ';', e.g. "a=t,t=d,f=100,..."
  std::string payload;  // everything after it (empty for a keys-only command)
  bool has_payload{false};
};

// Every APC (ESC _ G ... ESC \) sequence in `out`, in emission order. Safe to
// scan for naively: the base64 alphabet contains no ESC and no ';', and the
// interleaved cursor positioning is CSI, not APC.
auto apcs(std::string_view out) -> std::vector<Apc> {
  std::vector<Apc> found;
  for (std::size_t at = 0; (at = out.find("\033_G", at)) != std::string_view::npos;) {
    const std::size_t body = at + 3;
    const std::size_t end = out.find("\033\\", body);
    REQUIRE(end != std::string_view::npos);  // an unterminated APC is a bug
    const std::string_view seq = out.substr(body, end - body);
    const std::size_t semi = seq.find(';');
    Apc a;
    if (semi == std::string_view::npos) {
      a.keys = std::string{seq};
    } else {
      a.keys = std::string{seq.substr(0, semi)};
      a.payload = std::string{seq.substr(semi + 1)};
      a.has_payload = true;
    }
    found.push_back(std::move(a));
    at = end + 2;
  }
  return found;
}

// The transmit chunks only: a=t opens a transmission, and the continuation
// chunks that follow carry m= and nothing else.
auto transmit_chunks(const std::vector<Apc>& all) -> std::vector<Apc> {
  std::vector<Apc> out;
  for (const Apc& a : all) {
    const bool opener = a.keys.find("a=t") != std::string::npos;
    const bool continuation = a.has_payload && a.keys.starts_with("m=");
    if (opener || continuation) out.push_back(a);
  }
  return out;
}

// Independent base64 DECODER, deliberately not detail::base64_encode run
// backwards. Asserting that the emitted payload equals base64_encode(input)
// would put the driver and the test on the same side of the same function; a
// decoder makes the assertion "a terminal reading this gets the bytes the
// application handed us", which is the actual contract. base64 correctness
// itself is test/07base64's job.
auto b64_decode(std::string_view s) -> std::vector<std::byte> {
  auto sextet = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding, or junk
  };
  std::vector<std::byte> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char c : s) {
    const int v = sextet(c);
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::byte>((acc >> bits) & 0xFFU));
    }
  }
  return out;
}

// The payloads the terminal reassembles, in order, concatenated.
//
// Decoded PER TRANSMISSION, not over one joined string: each a=t opens an
// independent base64 stream with its own padding, so running two of them
// together through one decoder shifts everything after the first stream's
// pad by a couple of bits. That is a bug in this helper's arithmetic, not in
// the driver -- a terminal decodes each transmission on its own.
auto reassemble(std::string_view out) -> std::vector<std::byte> {
  std::vector<std::byte> all;
  std::string stream;
  auto finish = [&] {
    if (stream.empty()) return;
    const auto decoded = b64_decode(stream);
    all.insert(all.end(), decoded.begin(), decoded.end());
    stream.clear();
  };
  for (const Apc& a : transmit_chunks(apcs(out))) {
    if (a.keys.find("a=t") != std::string::npos) finish();  // a new upload
    stream += a.payload;
  }
  finish();
  return all;
}

auto count_of(std::string_view hay, std::string_view needle) -> int {
  int n = 0;
  for (std::size_t at = 0; (at = hay.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    ++n;
  }
  return n;
}

// A driver written against the pre-#163 interface: it overrides the four pure
// virtuals and knows nothing about EncodedImage. Its existence IS the test in
// "a legacy driver still compiles" below.
class LegacyDriver final : public TerminalDriver {
 public:
  auto init() -> std::expected<void, ErrorEvent> override { return {}; }
  auto draw_text(int, int, std::string_view, Rgb, Rgb, Attr) -> void override {}
  auto draw_image(Rect, const Image&)
      -> std::expected<void, ErrorEvent> override {
    m_drew_image = true;
    return {};
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

 private:
  bool m_drew_image{false};
};

constexpr Pixel kP1{10, 20, 30, 255};
constexpr Pixel kP2{200, 150, 100, 255};

}  // namespace

// ── the format actually on the wire ─────────────────────────────────────────

TEST_CASE("encoded: a PNG rides f=100, and nothing rides f=32",
          "[encoded][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const EncodedImage img{ImageFormat::Png, png_bytes(), Extent{2, 2}};
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img));
  d.flush();

  // Both halves matter. Asserting only that f=100 appears would stay green if
  // the driver emitted the transmission twice, once in each format; asserting
  // only that f=32 is absent would stay green if it emitted no transmission
  // at all.
  CHECK(count_of(out, "f=100") == 1);
  CHECK(count_of(out, "f=32") == 0);
}

TEST_CASE("encoded: the payload the terminal reassembles is the one we gave it",
          "[encoded][kitty]") {
  SECTION("a small PNG, one chunk") {
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    const EncodedImage img{ImageFormat::Png, png_bytes(), Extent{2, 2}};
    REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img));
    d.flush();

    const std::vector<std::byte> got = reassemble(out);
    const std::vector<std::byte> want(png_bytes().begin(), png_bytes().end());
    CHECK(got == want);
  }

  SECTION("a payload past the chunk boundary") {
    // 5000 raw bytes -> 6668 base64 chars -> two chunks. The reassembly is
    // what proves the split is lossless; the m= flags below prove the
    // terminal is told to expect the second half.
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    const auto big = blob(5000);
    const EncodedImage img{ImageFormat::Png, as_span(big), Extent{50, 25}};
    REQUIRE(d.draw_image(Rect{0, 0, 8, 4}, img));
    d.flush();

    CHECK(reassemble(out) == big);
  }
}

TEST_CASE("encoded: chunking obeys the protocol's framing rules",
          "[encoded][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto big = blob(5000);
  const EncodedImage img{ImageFormat::Png, as_span(big), Extent{50, 25}};
  REQUIRE(d.draw_image(Rect{0, 0, 8, 4}, img));
  d.flush();

  const std::vector<Apc> chunks = transmit_chunks(apcs(out));
  REQUIRE(chunks.size() >= 2);

  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const bool last = (i + 1 == chunks.size());
    const Apc& c = chunks[i];

    // m=1 on every chunk but the last. A terminal that is never told the
    // transmission ended holds the image incomplete and draws nothing.
    CHECK(c.keys.find(last ? "m=0" : "m=1") != std::string::npos);

    if (!last) {
      // The protocol requires non-final chunk sizes to be a multiple of 4:
      // otherwise the decoder resynchronises mid-quantum and everything after
      // the seam is garbage. transmit() static_asserts this on its constant;
      // this asserts it on the bytes.
      CHECK(c.payload.size() % 4 == 0);
    }
    if (i > 0) {
      // Continuation chunks carry m and optionally q, and nothing else --
      // repeating the transmission keys is an error, not a redundancy.
      CHECK(c.keys.find("f=") == std::string::npos);
      CHECK(c.keys.find("a=t") == std::string::npos);
      CHECK(c.keys.find("s=") == std::string::npos);
    }
  }
}

TEST_CASE("encoded: s=/v= carry the DECLARED extent, because we never parse",
          "[encoded][kitty]") {
  // The payload is a genuine 2x2 PNG whose header says so. The caller declares
  // 640x480. We ship 640x480, transmit the payload untouched, and raise
  // nothing: having an opinion here would mean owning a PNG decoder, which is
  // the dependency the whole design exists to avoid. This test exists to pin
  // that non-behaviour, which is otherwise indistinguishable from an oversight.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const EncodedImage lying{ImageFormat::Png, png_bytes(), Extent{640, 480}};
  REQUIRE(d.draw_image(Rect{0, 0, 4, 4}, lying));
  d.flush();

  CHECK(out.find("s=640,v=480") != std::string::npos);
  const std::vector<std::byte> want(png_bytes().begin(), png_bytes().end());
  CHECK(reassemble(out) == want);
}

// ── identity: what makes two payloads the same payload ──────────────────────

TEST_CASE("encoded: an unchanged payload costs nothing to redraw",
          "[encoded][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const EncodedImage img{ImageFormat::Png, png_bytes(), Extent{2, 2}};

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img));
  d.flush();
  REQUIRE(d.last_frame_bytes().image_transmit > 0);

  const std::size_t after_first = out.size();
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, img));
  d.flush();
  CHECK(out.size() == after_first);
  CHECK(d.last_frame_bytes().total() == 0);
}

TEST_CASE("encoded: two different payloads of the same size are two images",
          "[encoded][kitty]") {
  // The tempting cheap hash is the declared extent -- it is a handful of
  // bytes and it is right there. It is also wrong: every plate an application
  // bakes to a fixed size hashes identically, so the second one silently
  // never uploads and the first stays on screen.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto a = blob(256, 1);
  const auto b = blob(256, 2);
  REQUIRE(a.size() == b.size());
  REQUIRE(a != b);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 4},
                       EncodedImage{ImageFormat::Png, as_span(a), Extent{8, 8}}));
  d.flush();
  REQUIRE(d.last_frame_bytes().image_transmit > 0);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 4},
                       EncodedImage{ImageFormat::Png, as_span(b), Extent{8, 8}}));
  d.flush();
  CHECK(d.last_frame_bytes().image_transmit > 0);
  CHECK(reassemble(out).size() == a.size() + b.size());
}

TEST_CASE("encoded: the same bytes in a different format are a different image",
          "[encoded][kitty]") {
  // 64 bytes is a legal 4x4 RGBA buffer and also, as far as this library is
  // concerned, a legal (if nonsensical) PNG payload -- we do not parse it. The
  // terminal decodes the two completely differently, so a hash that ignored
  // the format would skip the second upload and leave the first rendering.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto bytes = blob(4 * 4 * 4);

  REQUIRE(d.draw_image(
      Rect{0, 0, 4, 4},
      EncodedImage{ImageFormat::Rgba32, as_span(bytes), Extent{4, 4}}));
  d.flush();
  REQUIRE(d.last_frame_bytes().image_transmit > 0);

  REQUIRE(d.draw_image(
      Rect{0, 0, 4, 4},
      EncodedImage{ImageFormat::Png, as_span(bytes), Extent{4, 4}}));
  d.flush();
  CHECK(d.last_frame_bytes().image_transmit > 0);
  CHECK(count_of(out, "f=32") == 1);
  CHECK(count_of(out, "f=100") == 1);
}

// ── the Rgba32 path is the old path ─────────────────────────────────────────

TEST_CASE("encoded: Rgba32 emits exactly what the Image overload emits",
          "[encoded][kitty][ansi_rgb][fallback]") {
  // The refactor that made room for f=100 moved every tier's image path onto
  // a byte span. If it changed a single byte of output for the format that
  // already worked, that is a regression in the flagship feature, not a
  // detail -- so this asserts equality of the whole frame on all three tiers.
  const Image art = checker(8, 6, kP1, kP2);
  const EncodedImage same{ImageFormat::Rgba32, std::as_bytes(art.pixels()),
                          Extent{art.width(), art.height()}};
  const Rect dest{1, 2, 4, 3};

  SECTION("kitty") {
    std::string via_image;
    std::string via_span;
    KittyDriver a;
    a.set_output(&via_image);
    REQUIRE(a.draw_image(dest, art));
    a.flush();
    KittyDriver b;
    b.set_output(&via_span);
    REQUIRE(b.draw_image(dest, same));
    b.flush();
    CHECK(via_image == via_span);
    CHECK(count_of(via_span, "f=32") == 1);
  }

  SECTION("ansi_rgb") {
    std::string via_image;
    std::string via_span;
    AnsiRgbDriver a;
    a.set_output(&via_image);
    REQUIRE(a.draw_image(dest, art));
    a.flush();
    AnsiRgbDriver b;
    b.set_output(&via_span);
    REQUIRE(b.draw_image(dest, same));
    b.flush();
    CHECK(via_image == via_span);
    CHECK_FALSE(via_span.empty());
  }

  SECTION("fallback") {
    std::string via_image;
    std::string via_span;
    FallbackDriver a;
    a.set_output(&via_image);
    REQUIRE(a.draw_image(dest, art));
    a.flush();
    FallbackDriver b;
    b.set_output(&via_span);
    REQUIRE(b.draw_image(dest, same));
    b.flush();
    CHECK(via_image == via_span);
    CHECK_FALSE(via_span.empty());
  }
}

TEST_CASE("encoded: an Rgba32 payload that disagrees with its extent warns",
          "[encoded][failure]") {
  // The one format whose length is derivable is the one format where this
  // mistake is visible at all. Left unchecked it is an out-of-bounds read that
  // gets base64'd to the terminal.
  const auto bytes = blob(4 * 4 * 4);  // a 4x4, declared as 8x8

  SECTION("kitty") {
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    auto r = d.draw_image(
        Rect{0, 0, 4, 4},
        EncodedImage{ImageFormat::Rgba32, as_span(bytes), Extent{8, 8}});
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "kitty");
    d.flush();
    CHECK(out.empty());
  }

  SECTION("ansi_rgb") {
    AnsiRgbDriver d;
    std::string out;
    d.set_output(&out);
    auto r = d.draw_image(
        Rect{0, 0, 4, 4},
        EncodedImage{ImageFormat::Rgba32, as_span(bytes), Extent{8, 8}});
    REQUIRE_FALSE(r);
    CHECK(r.error().source == "ansi_rgb");
    d.flush();
    CHECK(out.empty());
  }

  SECTION("fallback") {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    auto r = d.draw_image(
        Rect{0, 0, 4, 4},
        EncodedImage{ImageFormat::Rgba32, as_span(bytes), Extent{8, 8}});
    REQUIRE_FALSE(r);
    CHECK(r.error().source == "fallback");
    d.flush();
    CHECK(out.empty());
  }
}

// ── degradation on the tiers that cannot carry a payload ────────────────────

TEST_CASE("encoded: a tier that cannot decode PNG warns and emits nothing",
          "[encoded][failure]") {
  const EncodedImage img{ImageFormat::Png, png_bytes(), Extent{2, 2}};

  SECTION("ansi_rgb") {
    AnsiRgbDriver d;
    std::string out;
    d.set_output(&out);
    auto r = d.draw_image(Rect{0, 0, 2, 2}, img);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "ansi_rgb");
    d.flush();
    // Emitting *something* would be worse than emitting nothing: the payload
    // is not text, and half-blocks derived from bytes we cannot interpret are
    // noise painted over the cell grid.
    CHECK(out.empty());
  }

  SECTION("fallback") {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    auto r = d.draw_image(Rect{0, 0, 2, 2}, img);
    REQUIRE_FALSE(r);
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().source == "fallback");
    d.flush();
    CHECK(out.empty());
  }
}

TEST_CASE("encoded: supports_image_format answers before anything is drawn",
          "[encoded]") {
  // An application choosing an art set at cold start needs the answer at cold
  // start. A Warning returned from every frame, forever, after the decision
  // was already made is not an answer.
  KittyDriver k;
  CHECK(k.supports_image_format(ImageFormat::Rgba32));
  CHECK(k.supports_image_format(ImageFormat::Png));

  AnsiRgbDriver a;
  CHECK(a.supports_image_format(ImageFormat::Rgba32));
  CHECK_FALSE(a.supports_image_format(ImageFormat::Png));

  FallbackDriver f;
  CHECK(f.supports_image_format(ImageFormat::Rgba32));
  CHECK_FALSE(f.supports_image_format(ImageFormat::Png));

  // And it agrees with what actually happens. A capability query that can
  // disagree with the emit path is worse than none.
  std::string out;
  a.set_output(&out);
  const EncodedImage png{ImageFormat::Png, png_bytes(), Extent{2, 2}};
  CHECK(a.draw_image(Rect{0, 0, 2, 2}, png).has_value() ==
        a.supports_image_format(ImageFormat::Png));
}

// ── the empty guards, unchanged across the overload ─────────────────────────

TEST_CASE("encoded: empty is a warning on every tier, not a crash",
          "[encoded][failure]") {
  const EncodedImage none{};
  const EncodedImage some{ImageFormat::Png, png_bytes(), Extent{2, 2}};

  auto check = [&](auto& d, std::string_view source) {
    auto empty_payload = d.draw_image(Rect{0, 0, 2, 2}, none);
    REQUIRE_FALSE(empty_payload);
    CHECK(empty_payload.error().severity == Severity::Warning);
    CHECK(empty_payload.error().source == source);

    // Bytes but no extent is just as empty: nothing downstream can use a
    // payload it cannot size.
    auto no_extent =
        d.draw_image(Rect{0, 0, 2, 2},
                     EncodedImage{ImageFormat::Png, png_bytes(), Extent{}});
    REQUIRE_FALSE(no_extent);
    CHECK(no_extent.error().severity == Severity::Warning);

    auto empty_rect = d.draw_image(Rect{0, 0, 0, 0}, some);
    REQUIRE_FALSE(empty_rect);
    CHECK(empty_rect.error().severity == Severity::Warning);
  };

  KittyDriver k;
  std::string ko;
  k.set_output(&ko);
  check(k, "kitty");

  AnsiRgbDriver a;
  std::string ao;
  a.set_output(&ao);
  check(a, "ansi_rgb");

  FallbackDriver f;
  std::string fo;
  f.set_output(&fo);
  check(f, "fallback");
}

// ── the extensibility promise ───────────────────────────────────────────────

TEST_CASE("encoded: a driver written before #163 still compiles and degrades",
          "[encoded][drivers]") {
  // This is the whole argument for the new virtual being non-pure, and it is
  // not an argument that can be made in prose: nothing else in test/ derives
  // from TerminalDriver, so before this case a pure virtual would have sailed
  // through CI and broken every out-of-tree driver on upgrade.
  static_assert(termforge::DriverImpl<LegacyDriver>);

  LegacyDriver legacy;
  TerminalDriver& base = legacy;

  const EncodedImage img{ImageFormat::Png, png_bytes(), Extent{2, 2}};
  auto r = base.draw_image(Rect{0, 0, 2, 2}, img);
  REQUIRE_FALSE(r);
  CHECK(r.error().severity == Severity::Warning);
  CHECK(r.error().source == "driver");
  CHECK_FALSE(legacy.drew_image());

  // The inherited default is honest about itself, so an application asks and
  // gets a usable answer rather than discovering it a frame later.
  CHECK_FALSE(base.supports_image_format(ImageFormat::Png));
  CHECK(base.supports_image_format(ImageFormat::Rgba32));

  // And the overload it DOES implement is untouched.
  REQUIRE(base.draw_image(Rect{0, 0, 2, 2}, solid(2, 2, kP1)));
  CHECK(legacy.drew_image());
}

// ── the cell footprint of something never decoded ───────────────────────────

TEST_CASE("encoded: image_cell_extent answers from an extent alone",
          "[encoded]") {
  // An application holding an EncodedImage has pixel dimensions and no Image.
  // Before the overload it could not ask how many cells the thing occupies,
  // and the alternative -- re-deriving a footprint from capability flags --
  // is the mistake #100 removed from both examples.
  const Image art = solid(16, 9, kP1);
  const Extent px{art.width(), art.height()};

  KittyDriver k;
  CHECK(k.image_cell_extent(px) == k.image_cell_extent(art));

  AnsiRgbDriver a;
  CHECK(a.image_cell_extent(px) == a.image_cell_extent(art));
  CHECK(a.image_cell_extent(px) == Extent{16, 5});  // two pixel rows per cell

  FallbackDriver f;
  CHECK(f.image_cell_extent(px) == f.image_cell_extent(art));
  CHECK(f.image_cell_extent(px) == Extent{16, 9});  // one glyph per cell

  // An empty extent is empty, matching the Image overload on an empty image.
  CHECK(f.image_cell_extent(Extent{}) == Extent{});
  CHECK(f.image_cell_extent(Image{}) == Extent{});
}
