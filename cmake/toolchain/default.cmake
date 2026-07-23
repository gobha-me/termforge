# Default toolchain: respect the environment. The compiler comes from CXX /
# the platform default; we only pin the standard, warnings, and build-type
# defaults. Prefer clang? Use cmake/toolchain/clang.cmake:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake

# CMake evaluates toolchain files more than once per configure; without a
# guard the flag append below would duplicate the warning flags.
include_guard(GLOBAL)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif (NOT CMAKE_BUILD_TYPE)

# Append to (not replace) any user-supplied flags: a plain set() here would
# silently discard -DCMAKE_CXX_FLAGS from the command line (e.g. -fsanitize
# in CI), because toolchain variables shadow the cache.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wnarrowing -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
