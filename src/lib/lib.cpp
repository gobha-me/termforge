// Placeholder translation unit for the default compiled-library target.
//
// A STATIC library with no sources is ill-formed on some generators, and an
// empty archive teaches nothing. Replace this file (and the add_library()
// source list in src/lib/CMakeLists.txt) with the project's real sources.
//
// The matching header lives in include/ so the include-dir wiring is
// exercised by the default build + tests.

#include <version.hpp>

namespace template_lib {

// Trivial symbol so the archive is non-empty and linkable. Demonstrates that
// the generated version header is visible to the library target.
auto version_string() -> const char* {
  return PROGRAM_NAME.data();
}

}  // namespace template_lib
