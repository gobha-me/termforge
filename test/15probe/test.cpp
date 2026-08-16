#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "detail/probe.hpp"
#include "drivers/select_driver.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/terminal.hpp"
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

TEST_CASE("probe_kitty_animation: only the dedicated action reply is support",
          "[probe][kitty][animation]") {
  constexpr std::string_view basic = "\033_Gi=31;OK\033\\";
  constexpr std::string_view animation =
      "\033_Gi=4294967295;OK\033\\";
  REQUIRE(detail::probe_kitty_animation(
      std::string{basic} + std::string{animation} + "\033[?62;4;22c"));
  REQUIRE_FALSE(detail::probe_kitty_animation(
      std::string{basic} + "\033_Gi=4294967295;ENOTSUPPORTED\033\\" +
      "\033[?62c"));
  REQUIRE_FALSE(detail::probe_kitty_animation(
      std::string{basic} + "\033[?62c"));
  REQUIRE_FALSE(detail::probe_kitty_animation(
      std::string{basic} + "\033[?62c" + std::string{animation}));
  REQUIRE_FALSE(detail::probe_kitty_animation(
      "\033_Gi=429496729;OK\033\\\033[?62c"));  // truncated id
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

TEST_CASE("probe_sync_updates: only a settable 2026 DECRPM is support",
          "[probe][sync]") {
  REQUIRE(detail::probe_sync_updates("\033[?2026;1$y"));
  REQUIRE(detail::probe_sync_updates("\033[?2026;2$y"));
  REQUIRE(detail::probe_sync_updates(
      "\033[?2026;2$y\033[?62;4;22c"));
  REQUIRE_FALSE(detail::probe_sync_updates(""));
  REQUIRE_FALSE(detail::probe_sync_updates("\033[?2026;0$y"));
  REQUIRE_FALSE(detail::probe_sync_updates("\033[?2026;3$y"));
  REQUIRE_FALSE(detail::probe_sync_updates("\033[?2026;4$y"));
  REQUIRE_FALSE(detail::probe_sync_updates("\033[?2026;1"));
  REQUIRE_FALSE(detail::probe_sync_updates("\033[?2026;12$y"));
}

// ── #60: the keyboard-flags reply shares DA1's "\033[?" prefix ──────────────

TEST_CASE("find_da1: a CSI ? report with another final byte is not DA1",
          "[probe][da1][keyboard]") {
  // The whole reason find_da1 exists: CSI ? <flags> u (the kitty keyboard
  // reply, #60) starts exactly like DA1 and must not be mistaken for it.
  REQUIRE(detail::find_da1("\033[?1u") == std::string_view::npos);
  REQUIRE_FALSE(detail::probe_da1_complete("\033[?1u"));
  // …and the real DA1 behind it is still found, at its own offset.
  REQUIRE(detail::find_da1("\033[?1u\033[?62;22c") == 5);
  REQUIRE(detail::probe_da1_complete("\033[?1u\033[?62;22c"));
}

TEST_CASE("probe_kitty_ok: a keyboard reply before the graphics reply is not DA1",
          "[probe][kitty][keyboard][regression]") {
  // Adding the keyboard query (#60) must not cost us the KittyDriver. With a
  // bare find("\033[?") as the DA1 locator, the keyboard reply lands first,
  // g < da1 fails, and kitty_graphics silently degrades to false.
  REQUIRE(detail::probe_kitty_ok(
      "\033[?1u\033_Gi=31;OK\033\\\033[?62;4;22c"));
  // The genuine late-graphics case still does not count.
  REQUIRE_FALSE(detail::probe_kitty_ok("\033[?1u\033[?62c\033_Gi=31;OK\033\\"));
}

TEST_CASE("probe_kitty_keyboard: any flags report means the protocol is there",
          "[probe][keyboard]") {
  REQUIRE(detail::probe_kitty_keyboard("\033[?1u"));
  REQUIRE(detail::probe_kitty_keyboard("\033[?0u"));  // 0 flags: still an answer
  REQUIRE(detail::probe_kitty_keyboard("\033[?27u\033[?62;22c"));
  REQUIRE(detail::probe_kitty_keyboard("\033_Gi=31;OK\033\\\033[?1u\033[?62c"));
  // Silence is the "no" — a terminal without the protocol ignores the query.
  REQUIRE_FALSE(detail::probe_kitty_keyboard(""));
  REQUIRE_FALSE(detail::probe_kitty_keyboard("\033[?62;4;22c"));
  REQUIRE_FALSE(detail::probe_kitty_keyboard("\033[?1"));   // unterminated
  REQUIRE_FALSE(detail::probe_kitty_keyboard("\033[?u"));   // no flag digits
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

TEST_CASE("select_driver_for: a concrete built-in choice overrides precedence",
          "[probe][select]") {
  Capabilities kitty_caps;
  kitty_caps.kitty_graphics = true;
  kitty_caps.truecolor = true;

  CHECK(select_driver_for(kitty_caps, BuiltinDriver::Kitty)->name() ==
        "kitty");
  CHECK(select_driver_for(kitty_caps, BuiltinDriver::AnsiRgb)->name() ==
        "ansi-rgb");
  CHECK(select_driver_for(kitty_caps, BuiltinDriver::Fallback)->name() ==
        "fallback");
  CHECK(select_driver_for(kitty_caps, BuiltinDriver::Automatic)->name() ==
        "kitty");
}

TEST_CASE("Terminal selection carries action-level animation support as state",
          "[probe][select][animation]") {
  Terminal terminal;
  Capabilities caps;
  caps.kitty_graphics = true;
  caps.kitty_animation = true;
  auto supported = terminal.select_driver(caps, BuiltinDriver::Kitty);
  REQUIRE(supported->supports_image_animation());

  auto forced_ansi = terminal.select_driver(caps, BuiltinDriver::AnsiRgb);
  CHECK_FALSE(forced_ansi->supports_image_animation());

  caps.kitty_animation = false;
  auto unsupported = terminal.select_driver(caps, BuiltinDriver::Kitty);
  CHECK_FALSE(unsupported->supports_image_animation());
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

TEST_CASE("Input: the keyboard-flags reply is a report, not a keypress",
          "[probe][input][keyboard]") {
  // CSI ? <flags> u answers our own query. It must stay in the private-marker
  // branch and never reach the new CSI-u key path (#60).
  REQUIRE(count_events("\033[?1u").total == 0);
  REQUIRE(count_events("\033[?27u").total == 0);
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
