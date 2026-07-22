# Default toolchain: respect the environment. The compiler comes from CXX /
# the platform default; we only pin the standard, warnings, and build-type
# defaults. Prefer clang? Use cmake/toolchain/clang.cmake:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif (NOT CMAKE_BUILD_TYPE)

set(CMAKE_CXX_FLAGS "-Wall -Wnarrowing -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
