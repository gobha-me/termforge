#pragma once

// TermForge — capability-probe reply classification (pure, offline-testable).
//
// The startup probe (Terminal::query_capabilities) writes a kitty graphics
// query followed by a DA1 request, then reads whatever the terminal sends
// back. Classifying that raw reply is pure string work with no I/O, so it
// lives here where it can be unit-tested without a real terminal (the probe
// itself needs a tty; these predicates do not). See core/terminal.cpp.
//
// The reply is a concatenation of terminal responses, e.g.
//   kitty supported : "\033_Gi=31;OK\033\\"            + "\033[?1u" + "\033[?62;4;22c"
//   kitty error     : "\033_Gi=31;ENOTSUPPORTED\033\\" +             "\033[?62c"
//   no kitty        :                                                "\033[?62;22c"
// A graphics response referencing our probe id (i=31) with an OK status,
// arriving *before* the DA1 primary reply, is the support signal. An error
// status (";E...") must NOT count as support — the terminal answered, and its
// answer was "no". Header-only, stdlib-only, no I/O.
//
// Two replies now share the "\033[?" prefix: DA1 (CSI ? ... c) and the kitty
// keyboard-flags report (CSI ? ... u, #60). So "find \033[?" is no longer a
// DA1 locator — find_da1() below checks the *final byte* too, and everything
// that needs DA1's position goes through it.

#include <cstddef>
#include <string_view>

namespace termforge::detail {

// Offset of a complete DA1 primary device-attributes report — CSI ? then only
// parameter bytes [0-9;] then a final 'c' — or npos. Skips CSI ? reports with
// any other final byte (notably the keyboard-flags report, CSI ? <flags> u),
// so adding a query cannot make an unrelated reply look like DA1.
[[nodiscard]] inline auto find_da1(std::string_view reply) -> std::size_t {
  for (auto at = reply.find("\033[?"); at != std::string_view::npos;
       at = reply.find("\033[?", at + 3)) {
    for (std::size_t i = at + 3; i < reply.size(); ++i) {
      const char c = reply[i];
      if (c == 'c') return at;
      if (!((c >= '0' && c <= '9') || c == ';')) break;  // some other report
    }
  }
  return std::string_view::npos;
}

// A complete DA1 primary device-attributes report is present in `reply`.
// DA1 remains the read terminator after adding optional queries: terminals
// process the query stream in order, and one that does not implement a query
// ignores it. Waiting for an answer to an ignored DECRQM would add the full
// probe timeout to every unsupported terminal. As with the keyboard query, a
// reply arriving after DA1 is conservatively missed instead of taxing all
// other sessions.
[[nodiscard]] inline auto probe_da1_complete(std::string_view reply) -> bool {
  return find_da1(reply) != std::string_view::npos;
}

// The terminal answers synchronized-output as supported: DECRQM
// ESC [ ? 2026 $ p earns a DECRPM of the form ESC [ ? 2026 ; 1 $ y (set) or
// ESC [ ? 2026 ; 2 $ y (reset) -- 0 means "not supported", and 3/4 are the
// permanently-set/reset states. Either 1 or 2 means the wire
// carries the mode and a frame can be wrapped in it. The query is
// conservative in a way kitty_keyboard's is not: a terminal that ignores
// the whole thing says nothing, and the reply is marked "no".
[[nodiscard]] inline auto probe_sync_updates(std::string_view reply) -> bool {
  constexpr std::string_view kPrefix = "\033[?2026;";
  const auto rp = reply.find(kPrefix);
  if (rp == std::string_view::npos) return false;
  const auto value = rp + kPrefix.size();
  if (value + 2 >= reply.size()) return false;
  return (reply[value] == '1' || reply[value] == '2') &&
         reply[value + 1] == '$' && reply[value + 2] == 'y';
}

// The kitty keyboard protocol is supported: the terminal answered our flags
// query (CSI ? u) with a flags report, CSI ? <digits> u. *Any* flag value
// counts, including 0 — the signal is that the terminal answered at all. A
// terminal without the protocol ignores the query and says nothing.
[[nodiscard]] inline auto probe_kitty_keyboard(std::string_view reply) -> bool {
  for (auto at = reply.find("\033[?"); at != std::string_view::npos;
       at = reply.find("\033[?", at + 3)) {
    std::size_t i = at + 3;
    for (; i < reply.size() && reply[i] >= '0' && reply[i] <= '9'; ++i) {}
    if (i > at + 3 && i < reply.size() && reply[i] == 'u') return true;
  }
  return false;
}

// Kitty graphics is supported: an APC graphics response ("\033_G" ... "\033\\")
// that echoes our probe id (i=31) and carries an OK status, arriving before
// the DA1 reply. A ";E..." error status or a missing/late response is a "no".
[[nodiscard]] inline auto probe_kitty_ok(std::string_view reply) -> bool {
  const auto g = reply.find("\033_G");
  if (g == std::string_view::npos) return false;
  const auto st = reply.find("\033\\", g);  // APC String Terminator
  if (st == std::string_view::npos) return false;  // response not terminated
  const auto apc = reply.substr(g, st - g);
  if (apc.find("i=31") == std::string_view::npos) return false;
  if (apc.find(";OK") == std::string_view::npos) return false;  // reject ";E..."
  // A genuine graphics response precedes the DA1 primary reply. Located with
  // find_da1, not a bare "\033[?" search: the keyboard-flags report shares that
  // prefix, and mistaking it for DA1 would report "graphics arrived late" —
  // a silent driver downgrade caused by an unrelated query.
  const auto da1 = find_da1(reply);
  return da1 == std::string_view::npos || g < da1;
}

// Kitty image-animation actions are supported: the terminal accepted the
// dedicated a=f probe under an id outside every TermForge driver pool. This is
// deliberately separate from probe_kitty_ok -- a terminal can implement the
// basic transmit/place subset and still ignore animation-frame commands.
//
// Scan every APC rather than only the first because the ordinary i=31 graphics
// query intentionally precedes this one in the same startup response stream.
[[nodiscard]] inline auto probe_kitty_animation(
    std::string_view reply) -> bool {
  constexpr std::string_view kProbeId{"i=4294967295"};
  const auto da1 = find_da1(reply);
  std::size_t search_from = 0;
  while (true) {
    const auto g = reply.find("\033_G", search_from);
    if (g == std::string_view::npos) break;
    const auto st = reply.find("\033\\", g);
    if (st == std::string_view::npos) return false;
    const auto apc = reply.substr(g, st - g);
    if (apc.find(kProbeId) != std::string_view::npos &&
        apc.find(";OK") != std::string_view::npos) {
      return da1 == std::string_view::npos || g < da1;
    }
    search_from = st + 2;
  }
  return false;
}

// Sixel is advertised in the DA1 attribute list (attribute "4").
[[nodiscard]] inline auto probe_sixel(std::string_view reply) -> bool {
  return reply.find(";4;") != std::string_view::npos ||
         reply.find(";4c") != std::string_view::npos ||
         reply.find("[?4;") != std::string_view::npos ||
         reply.find("[?4c") != std::string_view::npos;
}

}  // namespace termforge::detail
