// Out-of-tree consumer smoke test (issue #27).
//
// Includes public headers by the <termforge/...> spelling the library
// advertises, and calls into two different library TUs (core/screen.cpp and
// widgets/label.cpp) so that a broken include path fails to compile and a
// broken export fails to link. It also compiles a consumer-owned App subclass
// against the three protected loop-source seams (#118). Exits non-zero if the
// drawn cell is blank, which would mean we linked something that does not
// actually work.

#include <chrono>

#include <termforge/core/app.hpp>
#include <termforge/core/screen.hpp>
#include <termforge/widgets/label.hpp>

namespace {

class ScriptedApp final : public termforge::App {
 public:
  // Compile-only access proof. Merely overriding a private virtual is legal
  // C++, so the overrides below cannot distinguish private from protected.
  // These qualified base calls can: moving the seams back to private makes
  // both add_subdirectory and installed-package consumer builds fail.
  auto compile_base_defaults(char* out) -> void {
    (void)termforge::App::now_steady();
    (void)termforge::App::wait_readable(0);
    (void)termforge::App::read_available(out, 1);
  }

 protected:
  auto on_render(termforge::Screen&) -> void override {}

  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }

  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds{timeout_ms};
    return false;
  }

  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
};

}  // namespace

auto main() -> int {
  termforge::Screen screen(20, 3);  // core/screen.cpp

  termforge::Label label{"consumed"};
  label.set_geometry(termforge::Rect{0, 0, 20, 1});
  label.draw(screen);  // widgets/label.cpp

  return screen.at(0, 0).blank() ? 1 : 0;
}
