# Opt-in clang toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

find_program(CLANG_CXX clang++)

if (CLANG_CXX)
  message(STATUS "clang toolchain: ${CLANG_CXX}")
  # CXX only — project(termforge LANGUAGES CXX) never enables C. Setting
  # CMAKE_C_COMPILER here made Catch2's check_cxx_compiler_flag try_compile
  # generate a scratch project that demanded C and failed a fresh configure
  # (#206). An already-configured tree hid the fault; reuse is not coverage.
  set(CMAKE_CXX_COMPILER ${CLANG_CXX})
else ()
  message(WARNING "clang toolchain requested but clang++ not found; using default compiler")
endif ()
