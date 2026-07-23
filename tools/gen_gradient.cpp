// TermForge tool: generate a sample gradient asset in raw-RGBA format.
//
// Usage: gen_gradient <output.rgba> [width] [height]
// Defaults: 64x64. Produces a horizontal red→green gradient with a vertical
// blue gradient, full alpha. Useful for testing ImageLoader and drivers.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

auto main(int argc, char* argv[]) -> int {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output.rgba> [width] [height]\n", argv[0]);
    return 1;
  }

  const std::string path = argv[1];
  const std::uint32_t w = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 64;
  const std::uint32_t h = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 64;

  if (w == 0 || h == 0 || w > 4096 || h > 4096) {
    std::fprintf(stderr, "error: dimensions must be 1..4096 (got %ux%u)\n", w, h);
    return 1;
  }

  std::ofstream ofs{path, std::ios::binary};
  if (!ofs) {
    std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
    return 1;
  }

  // Header: little-endian width, height.
  ofs.write(reinterpret_cast<const char*>(&w), 4);
  ofs.write(reinterpret_cast<const char*>(&h), 4);

  // Pixels: horizontal red→green, vertical blue ramp, full alpha.
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint8_t px[4] = {
          static_cast<std::uint8_t>(255 - (x * 255 / (w - 1))),  // r: high→low
          static_cast<std::uint8_t>(x * 255 / (w - 1)),          // g: low→high
          static_cast<std::uint8_t>(y * 255 / (h - 1)),          // b: top→bottom
          255,                                                    // a
      };
      ofs.write(reinterpret_cast<const char*>(px), 4);
    }
  }

  if (!ofs) {
    std::fprintf(stderr, "error: write failed\n");
    return 1;
  }

  std::fprintf(stderr, "wrote %s (%ux%u, %zu bytes)\n", path.c_str(), w, h,
               8 + static_cast<std::size_t>(w) * h * 4);
  return 0;
}
