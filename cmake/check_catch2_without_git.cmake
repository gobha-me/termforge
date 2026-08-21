# Configure-only regression for #323.
#
# 1) Static contract: top-level CMakeLists.txt must not require Git; the
#    Catch2 FetchContent fallback in cmake/deps/catch2.cmake must.
# 2) Live configure: with a fake installed Catch2 v3 and
#    CMAKE_DISABLE_FIND_PACKAGE_Git=TRUE, project configure must succeed.

if (NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT is required")
endif()

file(READ "${PROJECT_ROOT}/CMakeLists.txt" root_cmake)
if (root_cmake MATCHES "find_package\\(Git REQUIRED\\)")
  message(FATAL_ERROR
    "CMakeLists.txt must not find_package(Git REQUIRED); "
    "Git belongs only in cmake/deps/catch2.cmake's FetchContent fallback (#323)")
endif()

file(READ "${PROJECT_ROOT}/cmake/deps/catch2.cmake" catch2_cmake)
if (NOT catch2_cmake MATCHES "find_package\\(Git REQUIRED\\)")
  message(FATAL_ERROR
    "cmake/deps/catch2.cmake must find_package(Git REQUIRED) inside the "
    "Catch2-not-found fallback (#323)")
endif()

if (NOT DEFINED CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER STREQUAL "")
  message(STATUS
    "check_catch2_without_git: CMAKE_CXX_COMPILER unset; static contract only")
  return()
endif()

set(fake_dir "${CMAKE_BINARY_DIR}/fake-catch2-323")
file(MAKE_DIRECTORY "${fake_dir}")
file(WRITE "${fake_dir}/Catch2ConfigVersion.cmake"
"set(PACKAGE_VERSION \"3.5.2\")
set(PACKAGE_VERSION_EXACT TRUE)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_UNSUITABLE FALSE)
")
# Minimal imported targets so add_subdirectory(test) can configure.
file(WRITE "${fake_dir}/Catch2Config.cmake"
"if (NOT TARGET Catch2::Catch2)
  add_library(Catch2::Catch2 INTERFACE IMPORTED)
endif()
if (NOT TARGET Catch2::Catch2WithMain)
  add_library(Catch2::Catch2WithMain INTERFACE IMPORTED)
endif()
set(Catch2_FOUND TRUE)
")

set(build_dir "${CMAKE_BINARY_DIR}/configure-no-git-323")
file(REMOVE_RECURSE "${build_dir}")
file(MAKE_DIRECTORY "${build_dir}")

set(cfg_args
  -S ${PROJECT_ROOT}
  -B ${build_dir}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE
  -DCatch2_DIR=${fake_dir}
  -Dtermforge_TESTS=ON
  -Dtermforge_EXAMPLES=OFF
  -Dtermforge_BENCH=OFF
  -Dtermforge_INSTALL=OFF
  -DBUILD_TESTING=ON
)
if (DEFINED CMAKE_C_COMPILER AND NOT CMAKE_C_COMPILER STREQUAL "")
  list(APPEND cfg_args -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} ${cfg_args}
  RESULT_VARIABLE cfg_rc
  OUTPUT_VARIABLE cfg_out
  ERROR_VARIABLE cfg_err
  )

if (NOT cfg_rc EQUAL 0)
  message(FATAL_ERROR
    "preinstalled Catch2 + CMAKE_DISABLE_FIND_PACKAGE_Git=TRUE configure "
    "failed (#323):\n${cfg_out}\n${cfg_err}")
endif()

if (cfg_out MATCHES "find_package for module Git" OR
    cfg_err MATCHES "find_package for module Git")
  message(FATAL_ERROR
    "configure still required Git despite installed Catch2 (#323):\n"
    "${cfg_out}\n${cfg_err}")
endif()
