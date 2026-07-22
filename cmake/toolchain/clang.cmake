# Opt-in clang toolchain. Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

find_program(CLANG_C clang)
find_program(CLANG_CXX clang++)

if (CLANG_CXX)
  message(STATUS "clang toolchain: ${CLANG_CXX}")
  set(CMAKE_CXX_COMPILER ${CLANG_CXX})
  set(CMAKE_C_COMPILER ${CLANG_C})
else ()
  message(WARNING "clang toolchain requested but clang++ not found; using default compiler")
endif ()
