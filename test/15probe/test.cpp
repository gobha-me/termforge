#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "detail/probe.hpp"
#include "drivers/select_driver.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/types.hpp"

using namespace termforge;

// ── #8: capability-probe reply classification (pure) ────────────────────────
//
// The probe writes a kitty graphics query (id i=31) then a DA1 request, and
// reads the concatenated reply. These predicates decide support offline.

TEST_CASE("probe_kitty_ok: OK graphics response before DA1 counts as support",
          "[probe][kitty]") {
  REQUIRE(detail::probe_kitty_ok("\033_Gi=31;OK\033\\\033[?62;4;22c"));
}

TEST_CASE("probe_kitty_ok: an error status is NOT support (#8.4 regression)",
          "[probe][kitty][regression]") {
  // The audit's core defect: a terminal answering with an error was still
  // classified kitty_graphics=true and handed the KittyDriver, whose
  // transmissions then silently fail into empty cells.
  REQUIRE_FALSE(
      detail::probe_kitty_ok("\033_Gi=31;ENOTSUPPORTED\033\\\033[?62c"));
}

TEST_CASE("probe_kitty_ok: response missing our probe id is not support",
          "[probe][kitty]") {
  REQUIRE_FALSE(detail::probe_kitty_ok("\033_Gi=99;OK\033\\\033[?62c"));
}

TEST_CASE("probe_kitty_ok: a graphics reply after DA1 does not count",
          "[probe][kitty]") {
  // Ordering guard: a genuine graphics response precedes DA1.
  REQUIRE_FALSE(detail::probe_kitty_ok("\033[?62c\033_Gi=31;OK\033\\"));
}

TEST_CASE("probe_kitty_ok: an unterminated APC response is not support",
          "[probe][kitty]") {
  REQUIRE_FALSE(detail::probe_kitty_ok("\033_Gi=31;OK"));  // no ST yet
}

TEST_CASE("probe_da1_complete: false until the DA1 terminator arrives",
          "[probe][da1]") {
  // Drives read_available's early exit — the reader stops the moment this
  // flips true instead of burning the whole 150ms window.
  REQUIRE_FALSE(detail::probe_da1_complete(""));
  REQUIRE_FALSE(detail::probe_da1_complete("\033[?62;4"));  // mid-report
  REQUIRE(detail::probe_da1_complete("\033[?62;4;22c"));
  REQUIRE(detail::probe_da1_complete("\033_Gi=31;OK\033\\\033[?62c"));
}

TEST_CASE("probe_sixel: DA1 advertises attribute 4", "[probe][sixel]") {
  REQUIRE(detail::probe_sixel("\033[?62;4;22c"));
  REQUIRE(detail::probe_sixel("\033[?4c"));
  REQUIRE_FALSE(detail::probe_sixel("\033[?62;22c"));
}

// ── #8: single probe → driver selection is a pure caps → driver mapping ──────

TEST_CASE("select_driver_for: kitty caps select the KittyDriver",
          "[probe][select]") {
  Capabilities caps;
  caps.kitty_graphics = true;
  auto d = select_driver_for(caps);
  REQUIRE(d != nullptr);
  REQUIRE(d->capabilities().kitty_graphics);
}

TEST_CASE("select_driver_for: truecolor caps select the ANSI RGB driver",
          "[probe][select]") {
  Capabilities caps;
  caps.truecolor = true;
  auto d = select_driver_for(caps);
  REQUIRE(d != nullptr);
  REQUIRE_FALSE(d->capabilities().kitty_graphics);
  REQUIRE(d->capabilities().truecolor);
}

TEST_CASE("select_driver_for: empty caps degrade to the fallback driver",
          "[probe][select]") {
  auto d = select_driver_for(Capabilities{});
  REQUIRE(d != nullptr);
  REQUIRE_FALSE(d->capabilities().kitty_graphics);
  REQUIRE_FALSE(d->capabilities().truecolor);
}

// ── #8.3: a late CSI device report must not leak into the input stream ───────

namespace {

// Count events of each kind produced by decoding `bytes`.
struct Counts {
  int chars{0}, unknown{0}, total{0};
};
auto count_events(std::string_view bytes) -> Counts {
  Input in;
  Counts c;
  for (const auto& ev : in.decode(bytes)) {
    ++c.total;
    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      if (k->key == Key::Char) ++c.chars;
      if (k->key == Key::Unknown) ++c.unknown;
    }
  }
  return c;
}

}  // namespace

TEST_CASE("Input: a late DA1 report is swallowed, not exploded into chars",
          "[probe][input][regression]") {
  // Old behavior: ESC[?62;4;22c -> Key::Unknown + Char('6'),('2'),(';')…, i.e.
  // spurious keypresses delivered as if the user typed the DA1 digits.
  const auto c = count_events("\033[?62;4;22c");
  REQUIRE(c.chars == 0);
  REQUIRE(c.unknown == 0);
  REQUIRE(c.total == 0);  // a device report is not user input
}

TEST_CASE("Input: DA2 and DECRPM private-marker reports are also dropped",
          "[probe][input]") {
  REQUIRE(count_events("\033[>0;276;0c").total == 0);   // DA2
  REQUIRE(count_events("\033[?2026;2$y").total == 0);    // DECRPM
}

TEST_CASE("Input: a real arrow key still decodes after the hardening",
          "[probe][input]") {
  Input in;
  auto ev = in.decode("\033[A");
  REQUIRE(ev.size() == 1);
  const auto* k = std::get_if<KeyEvent>(&ev.front());
  REQUIRE(k != nullptr);
  REQUIRE(k->key == Key::Up);
}

TEST_CASE("Input: a DA1 report split across two feeds decodes as one drop",
          "[probe][input][regression]") {
  // The read that carries a late reply can split it on any byte, including the
  // leading ESC. feed() must fold the pieces back together and still drop it.
  Input in;
  in.feed("\033");
  in.feed("[?62;4;22c");
  in.flush();
  REQUIRE(in.poll().empty());
}
