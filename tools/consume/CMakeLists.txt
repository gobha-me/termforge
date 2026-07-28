# Minimal out-of-tree consumer -- the acceptance fixture for issue #27.
#
# Deliberately lives under tools/, NOT test/: test/CMakeLists.txt globs test/*
# and add_subdirectory()s anything with a CMakeLists.txt, then add_test()s a
# matching <dir>-test binary. A standalone project with its own project() and
# find_package() would be swallowed into the main build and fail to configure.
#
# Driven by tools/consume/run.sh; not part of the termforge build.
cmake_minimum_required(VERSION 3.28)

project(termforge_consumer LANGUAGES CXX)

if (TERMFORGE_SOURCE_DIR)
  # Path 1: add_subdirectory with ZERO extra options. If termforge's gating is
  # wrong this build also grows a demo binary, ten examples and a Catch2 clone
  # -- which is exactly what run.sh asserts against.
  add_subdirectory(${TERMFORGE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/termforge)
else ()
  # Path 2: the installed package.
  find_package(termforge CONFIG REQUIRED)
  message(STATUS "consumer: found termforge ${termforge_VERSION}")
endif ()

# The same target spelling in both modes. That is the point of the exercise.
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE termforge::lib)
