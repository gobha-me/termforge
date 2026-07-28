// Out-of-tree consumer smoke test (issue #27).
//
// Includes public headers by the <termforge/...> spelling the library
// advertises, and calls into two different library TUs (core/screen.cpp and
// widgets/label.cpp) so that a broken include path fails to compile and a
// broken export fails to link. Exits non-zero if the drawn cell is blank,
// which would mean we linked something that does not actually work.

#include <termforge/core/screen.hpp>
#include <termforge/widgets/label.hpp>

auto main() -> int {
  termforge::Screen screen(20, 3);  // core/screen.cpp

  termforge::Label label{"consumed"};
  label.set_geometry(termforge::Rect{0, 0, 20, 1});
  label.draw(screen);  // widgets/label.cpp

  return screen.at(0, 0).blank() ? 1 : 0;
}
