// Start here to explore shipped widgets. Focused, copyable subsystem examples
// remain separate; see docs/widget-gallery.md and F6 in the running gallery.
#include <exception>
#include <iostream>

#include "gallery_app.hpp"

auto main() -> int {
  try {
    termforge::examples::GalleryApp app;
    return app.run();
  } catch (const std::exception& error) {
    // App has already restored the terminal before an exception reaches here.
    std::cerr << "widget gallery: " << error.what() << '\n';
    return 1;
  }
}
