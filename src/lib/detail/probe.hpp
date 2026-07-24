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
//   kitty supported : "\033_Gi=31;OK\033\\"            + "\033[?62;4;22c"
//   kitty error     : "\033_Gi=31;ENOTSUPPORTED\033\\" + "\033[?62c"
//   no kitty        :                                    "\033[?62;22c"
// A graphics response referencing our probe id (i=31) with an OK status,
// arriving *before* the DA1 primary reply, is the support signal. An error
// status (";E...") must NOT count as support — the terminal answered, and its
// answer was "no". Header-only, stdlib-only, no I/O.

#include <string_view>

namespace termforge::detail {

// A complete DA1 primary device-attributes report — CSI ? ... c ("\033[?"
// then a terminating 'c') — is present in `reply`. This is the probe's
// read terminator: once it arrives, the terminal has answered everything we
// asked (the kitty graphics reply, if any, always precedes DA1), so the
// reader can stop waiting instead of burning the full timeout.
[[nodiscard]] inline auto probe_da1_complete(std::string_view reply) -> bool {
  const auto da1 = reply.find("\033[?");
  if (da1 == std::string_view::npos) return false;
  return reply.find('c', da1) != std::string_view::npos;
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
  // A genuine graphics response precedes the DA1 primary reply.
  const auto da1 = reply.find("\033[?");
  return da1 == std::string_view::npos || g < da1;
}

// Sixel is advertised in the DA1 attribute list (attribute "4").
[[nodiscard]] inline auto probe_sixel(std::string_view reply) -> bool {
  return reply.find(";4;") != std::string_view::npos ||
         reply.find(";4c") != std::string_view::npos ||
         reply.find("[?4;") != std::string_view::npos ||
         reply.find("[?4c") != std::string_view::npos;
}

}  // namespace termforge::detail
