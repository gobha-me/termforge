// Public-header self-sufficiency suite (issue #54).
//
// detail/dropdown.hpp shipped including "detail/width.hpp", which lives under
// src/lib on the library's PRIVATE include path — so an out-of-tree consumer
// with only the PUBLIC include dir couldn't compile the advertised building
// block. In-tree builds never noticed (library TUs compile with the private
// dir). This suite pins the contract that killed it: EVERY public header must
// compile standalone with ${PROJECT_SOURCE_DIR}/include as the ONLY include
// path.
//
// Mechanism: CMake globs include/termforge/**/*.hpp at configure time and
// generates one .cpp per header containing exactly `#include <that header>`
// plus a main(); the suite fails to BUILD — which is the point — the moment a
// public header reaches for a private one. New public headers are picked up
// on the next configure with no test edits.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("public headers: every header compiled standalone (see 22headers build)",
          "[headers]") {
  // The check is at BUILD time; this case exists so the suite registers in
  // ctest and the directory reads like the others. If you are looking at a
  // failure here, scroll up: the real error was a compile error in one of the
  // generated header_*.cpp files.
  SUCCEED("all public headers compiled standalone at build time");
}
