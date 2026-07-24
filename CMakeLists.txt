cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

if (NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
endif()

# https://stackoverflow.com/questions/12344368/automatically-use-the-directory-as-the-project-name-in-cmake
get_filename_component(ProjectId ${CMAKE_CURRENT_LIST_DIR} NAME)
string(REPLACE " " "_" ProjectId ${ProjectId})

project(${ProjectId}
  VERSION ${VERSION}
  LANGUAGES CXX
)

message(STATUS "${PROJECT_NAME}:${CMAKE_PROJECT_VERSION}")

set (VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")

if (EXISTS ${VERSION_HEADER_IN})
  string (REPLACE ".hpp.in.cmake" ".hpp" VERSION_HEADER ${VERSION_HEADER_IN})
  message(STATUS "${VERSION_HEADER_IN} ${VERSION_HEADER}")
  configure_file (${VERSION_HEADER_IN} ${VERSION_HEADER})
endif ()

find_package(Git REQUIRED) # some deps may require this, depends .. :D
#message(STATUS "git found: ${GIT_EXECUTABLE} in version ${GIT_VERSION_STRING}")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

# GCC 13/14 -Wmaybe-uninitialized false positive: moving a std::variant whose
# alternatives hold an SSO std::string through std::deque::push_back trips a
# spurious "may be used uninitialized" deep inside libstdc++'s variant move
# ctor at -O2/-O3 (the analyzer loses the active member across the inlining).
# ASan/UBSan and Clang are clean on the same code; it is a known GCC bug.
# add_compile_options (not CMAKE_CXX_FLAGS) so it applies even when CI passes
# -DCMAKE_CXX_FLAGS=-Werror, which would otherwise shadow toolchain appends.
# Scoped to GCC only so Clang keeps the genuine diagnostic.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wno-maybe-uninitialized)
endif ()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)

option(${PROJECT_NAME}_EXAMPLES "ENABLE EXAMPLES" ON)
option(${PROJECT_NAME}_TESTS "ENABLE TESTS" ON)

if (${PROJECT_NAME}_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif(${PROJECT_NAME}_TESTS)

if (${PROJECT_NAME}_EXAMPLES)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/examples)
endif(${PROJECT_NAME}_EXAMPLES)

# TODO Install Template - even if it is commented out as examples
