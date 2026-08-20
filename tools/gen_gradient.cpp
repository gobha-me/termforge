// TermForge tool: generate a sample gradient asset in raw-RGBA format.
//
// Usage: gen_gradient <output.rgba> [width] [height]
// Defaults: 64x64. Produces a horizontal red→green gradient with a vertical
// blue gradient, full alpha. Useful for testing ImageLoader and drivers.

#include "gen_gradient.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

auto main(int argc, char* argv[]) -> int {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output.rgba> [width] [height]\n", argv[0]);
    return 1;
  }

  const std::string path = argv[1];
  std::uint32_t w = 64;
  std::uint32_t h = 64;

  if (argc > 2 && !gen_gradient::parse_dimension(argv[2], &w)) {
    std::fprintf(stderr, "error: invalid width (need 1..4096 decimal)\n");
    return 1;
  }
  if (argc > 3 && !gen_gradient::parse_dimension(argv[3], &h)) {
    std::fprintf(stderr, "error: invalid height (need 1..4096 decimal)\n");
    return 1;
  }

  if (!gen_gradient::write_file(path, w, h)) {
    std::fprintf(stderr, "error: failed to write %s\n", path.c_str());
    return 1;
  }

  std::fprintf(stderr, "wrote %s (%ux%u, %zu bytes)\n", path.c_str(), w, h,
               8 + static_cast<std::size_t>(w) * h * 4);
  return 0;
}
