# AddressSanitizer toolchain. Usage:
#   cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake
#
# Sanitizer *runtime* flags (-fsanitize=address) are all the compiler needs;
# they also imply the linker flags. There is no find_library() step: that
# command sets <VAR>, never <VAR>_FOUND, so gating on ASAN_FOUND silently
# dropped the flags and produced a normal, unsanitized build.
include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

message(STATUS "toolchain: address sanitizer enabled")
add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
add_link_options(-fsanitize=address)
