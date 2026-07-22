#include <catch2/catch_session.hpp>

// Pulled from https://github.com/catchorg/Catch2/blob/devel/docs/own-main.md
// This is common to all test/*/test.cpp that does not specify a CMakeLists.txt

auto main(int argc, char **argv) -> int {
  // Setup Code

  int result = Catch::Session().run(argc, argv);

  // Cleanup Code

  return result;
}
