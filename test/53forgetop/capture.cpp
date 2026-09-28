#include "forge_top.hpp"
#include "support/screen.hpp"

#include <cstdint>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace termforge;
using namespace termforge::forge_top;

auto main(int argc, char** argv) -> int {
  try {
    if (argc < 4 || argc > 5)
      throw std::runtime_error("usage: capture COLS ROWS TIER=0..2 [--wire]");
    const int cols = std::stoi(argv[1]), rows = std::stoi(argv[2]);
    const int tier = std::stoi(argv[3]);
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000 || tier < 0 ||
        tier > 2)
      throw std::runtime_error("dimensions/tier out of range");
    if (argc == 5 && std::string_view{argv[4]} != "--wire")
      throw std::runtime_error("unknown capture option");
    const auto choice = tier == 0   ? DriverChoice::Fallback
                        : tier == 1 ? DriverChoice::AnsiRgb
                                    : DriverChoice::Kitty;
    SyntheticClock clock;
    ForgeTopApp app{make_fake_reader(), true};
    app.set_clock(&clock);
    std::string cells, wire;
    app.set_frame_observer([&](const FrameObservation&) {
      const auto& screen = app.screen_for_test();
      for (int y = 0; y < screen.rows(); ++y)
        cells += tfsupport::row_text(screen, y) + '\n';
    });
    app.run_headless(1, cols, rows, &wire, choice);
    if (argc == 4) {
      std::cout << cells;
    } else {
      // An emission fingerprint, not a security hash or performance result.
      std::uint64_t fingerprint = 14695981039346656037ULL;
      for (const unsigned char byte : wire) {
        fingerprint ^= byte;
        fingerprint *= 1099511628211ULL;
      }
      std::cout << std::format(
          "{{\"cols\":{},\"rows\":{},\"tier\":{},\"cores\":20,"
          "\"frames\":1,\"includes_shutdown\":true,\"bytes\":{},"
          "\"fnv1a64\":\"{:016x}\"}}\n",
          cols, rows, tier, wire.size(), fingerprint);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
