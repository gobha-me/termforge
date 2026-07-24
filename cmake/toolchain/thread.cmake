# ThreadSanitizer toolchain. Usage:
#   cmake -B build-tsan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/thread.cmake
#
# Same rationale as address.cmake: pass the sanitizer flag directly instead
# of find_library()+ASAN_FOUND-style gating, which never fired. TSan needs
# optimization (-O1/-O2) to be useful; Debug's -O0 makes it prohibitively
# slow, so bump the Debug optimization level here.
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "toolchain: thread sanitizer enabled")
add_compile_options(-O1 -g -fsanitize=thread)
add_link_options(-fsanitize=thread)
