cmake_minimum_required(VERSION 3.28)

# Everything in here is a developer convenience a consumer must never inherit.
#
# PROJECT_IS_TOP_LEVEL cannot gate it: that variable is set *by* project(), and
# the toolchain file has to be chosen *before* project() (that is when CMake
# probes the compiler). CMAKE_SOURCE_DIR is bound to the top-level source dir
# of the whole build from the first line CMake reads, so comparing it against
# the directory being processed is the pre-project() spelling of
# PROJECT_IS_TOP_LEVEL: equal only when this file is the entry point, never
# under add_subdirectory()/FetchContent.
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  if (NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
  endif ()

  # clang-tidy eats compile_commands.json. Hoisted out of
  # cmake/toolchain/default.cmake so it survives -DCMAKE_TOOLCHAIN_FILE=<any
  # other toolchain>, while never being forced on a consumer's build tree.
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

# Hardcoded, NOT derived from the directory name: FetchContent checks out into
# _deps/termforge-src and GitHub source zips extract to termforge-main, either
# of which would rename every target (termforge-src::lib), every option and the
# installed package -- and break find_package(termforge) outright. PROJECT_NAME
# is load-bearing below for the export set, the termforge:: namespace and the
# termforgeConfig.cmake filename (matched case-sensitively on Linux).
project(termforge
  VERSION ${TERMFORGE_VERSION}
  LANGUAGES CXX
)

# PROJECT_VERSION, not CMAKE_PROJECT_VERSION: the latter is the *top-level*
# project's version, so under add_subdirectory it printed the consumer's.
message(STATUS "${PROJECT_NAME}:${PROJECT_VERSION}")

# Must precede add_subdirectory(src/lib): src/lib's
# $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> expands to an empty install
# interface without it -- silent at build time, "file not found" at the
# consumer.
include(GNUInstallDirs)

# Generated into the BUILD tree. Writing into the source tree breaks read-only
# and shared source dirs (FetchContent's _deps, distro build roots) and is not
# a consumer's business. Deliberately NOT installed: it declares PROGRAM_NAME /
# VERSION_MAJOR ... at global scope under a generic name, which must not land
# in ${prefix}/include.
set(VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")

if (EXISTS ${VERSION_HEADER_IN})
  set(VERSION_HEADER "${PROJECT_BINARY_DIR}/generated/include/version.hpp")
  configure_file(${VERSION_HEADER_IN} ${VERSION_HEADER} @ONLY)
endif ()

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

# Declared BEFORE the add_subdirectory() calls they gate -- they used to sit
# below them, which is why src/bin was built unconditionally. Defaulting to
# PROJECT_IS_TOP_LEVEL is what makes `add_subdirectory(termforge)` with zero
# options build the library and nothing else.
option(${PROJECT_NAME}_TESTS    "Build the termforge test suite"    ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_EXAMPLES "Build the termforge examples"      ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_BIN      "Build the termforge demo binary"   ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_INSTALL  "Generate termforge install rules"  ${PROJECT_IS_TOP_LEVEL})

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)

if (${PROJECT_NAME}_BIN)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)
endif ()

if (${PROJECT_NAME}_TESTS)
  # include(CTest) calls enable_testing() itself and declares BUILD_TESTING;
  # honouring it keeps the ecosystem-wide -DBUILD_TESTING=OFF working alongside
  # the project-specific switch. Both are inside the guard so a consumer never
  # gets CTest's Nightly/Continuous targets or a DartConfiguration.tcl.
  include(CTest)

  if (BUILD_TESTING)
    find_package(Git REQUIRED) # Catch2's FetchContent fallback needs it

    # Catch2 lives here, not at top level: it used to be pulled in
    # unconditionally, so a consumer -- or anyone with -Dtermforge_TESTS=OFF --
    # still paid for a find_package/clone of a test-only dependency.
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
  endif ()
endif ()

if (${PROJECT_NAME}_EXAMPLES)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/examples)
endif ()

# Install / export. Gated so a vendored termforge's install() rules do not
# pollute a consumer's `cmake --install`; a consumer that genuinely wants to
# ship termforge alongside its own export set flips termforge_INSTALL back on.
if (${PROJECT_NAME}_INSTALL)
  include(CMakePackageConfigHelpers)

  set(TERMFORGE_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

  install(TARGETS ${PROJECT_NAME}_lib
    EXPORT   ${PROJECT_NAME}Targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # include/termforge/** only. The generated include/version.hpp stays out:
  # a generic version.hpp in ${prefix}/include is a collision waiting to
  # happen. Globbed at install time, so new headers are picked up for free.
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/${PROJECT_NAME}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
  )

  install(EXPORT ${PROJECT_NAME}Targets
    FILE        ${PROJECT_NAME}Targets.cmake
    NAMESPACE   ${PROJECT_NAME}::
    DESTINATION ${TERMFORGE_CMAKEDIR}
  )

  # Build-tree export: find_package(termforge) resolves against an un-installed
  # build dir via -Dtermforge_DIR=<build>, with the same target names.
  export(EXPORT ${PROJECT_NAME}Targets
    FILE      ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
  )

  # SameMinorVersion while we are 0.x: under SameMajorVersion every 0.y would
  # claim compatibility with every other, which is wrong for a pre-1.0 library
  # that still breaks API between minors. Switch to SameMajorVersion at 1.0.
  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
  )

  configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/cmake/${PROJECT_NAME}Config.cmake.in
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    INSTALL_DESTINATION ${TERMFORGE_CMAKEDIR}
  )

  install(FILES
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    DESTINATION ${TERMFORGE_CMAKEDIR}
  )

  install(FILES ${PROJECT_SOURCE_DIR}/LICENSE.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}
  )
endif ()

# src/bin is deliberately not installed: `termforge` is far too generic a name
# for ${prefix}/bin, and it is a demo, not a tool.
