// TermForge example: image
//
// Demonstrates the full image pipeline: ImageLoader loads a raw-RGBA asset
// from disk, then the best available driver renders it. Shows which driver
// tier was selected so you can verify kitty vs. fallback behavior.
//
// Shows:
//   - ImageLoader::load() with error handling
//   - Terminal::query_capabilities() and select_driver(caps)
//   - TerminalDriver::draw_image() (degrades gracefully)
//   - TerminalDriver::draw_text() with fg/bg colors
//   - Which driver tier is active (kitty / ansi_rgb / fallback)
//   - Lifecycle order (#320): every fallible step -- probe, driver init,
//     asset load -- runs BEFORE enter_screen(), so no failure path strands
//     the user's terminal on the alt-screen
//   - RAII teardown: driver->shutdown() then leave_screen() on every exit
//     path from the alt-screen on, exceptions included
//   - An Input-decoder exit wait: early-typed keys are preserved and
//     honored, and terminal control-plane replies are routed to the driver
//     instead of being discarded raw

#include <cstdio>
#include <format>

#include "termforge/core/image_loader.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/widgets/theme.hpp"

using namespace termforge;

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

auto main() -> int {
  Terminal term;

  if (auto res = term.enter_raw(); !res) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Failed to enter raw mode: {}", res.error().message)
            .c_str());
    return 1;
  }

  // Everything fallible happens before enter_screen() (#320): a failure on
  // any of these paths returns with the terminal still on its normal screen.
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

  // Load the sample gradient asset.
  auto img_result = ImageLoader::load("assets/gradient.rgba");
  if (!img_result) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Asset load failed: {}", img_result.error().message)
            .c_str());
    return 1;
  }
  auto& img = *img_result;

  // Enter alt-screen, then arm the guard that undoes it (and ends the
  // driver's session) on every exit path from here on.
  term.enter_screen();
  const ScreenGuard cleanup{*driver, term};

  // Describe the active driver tier.
  const auto dcaps = driver->capabilities();
  const char* tier = dcaps.kitty_graphics ? "Kitty graphics"
                     : dcaps.truecolor    ? "ANSI truecolor half-blocks"
                                          : "ASCII fallback";

  const Rgb white{0xFF, 0xFF, 0xFF}, dark = theme::kBg;
  const Rgb cyan{0x00, 0xFF, 0xFF}, green{0x00, 0xFF, 0x80};

  driver->draw_text(0, 0, "TermForge Image Demo", cyan, dark, Attr::Bold);
  driver->draw_text(0, 1, std::format("Driver tier: {}", tier), green, dark,
                    Attr::None);
  driver->draw_text(0, 2,
                    std::format("Asset: assets/gradient.rgba ({}x{})",
                                img.width(), img.height()),
                    white, dark, Attr::None);

  // Ask the driver how many cells this image wants at its native resolution,
  // then name that rect as the destination. The answer and the placement are
  // now the same number by construction (#100): this example used to derive
  // rows_used from capability flags, which is not what determines the packing
  // -- the flags describe colour -- and the first version of that expression
  // put the prompt on top of the image on the fallback tier.
  const Extent extent = driver->image_cell_extent(img);
  const Rect dest{0, 4, extent.w, extent.h};
  if (auto res = driver->draw_image(dest, img); !res) {
    driver->draw_text(0, 4, "Image render failed: " + res.error().message,
                      Rgb{0xFF, 0x40, 0x40}, dark, Attr::None);
  }

  const int prompt_row = dest.y + dest.h + 1;
  driver->draw_text(0, prompt_row, "Press any key to exit...", white, dark,
                    Attr::Dim);
  driver->flush();

  // Wait for a keypress through the Input decoder (#320). This demo uploads
  // raw RGBA with no ack requested, so the raw drain loop this replaces was
  // not discarding "kitty ack responses" -- it was discarding keys the user
  // typed early. Feed every chunk until a read comes back empty, flush()
  // only at that drained boundary (a held lone ESC resolves to Escape there,
  // while split sequences stay held), route control-plane replies to the
  // driver, and exit on the first decoded key press.
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
