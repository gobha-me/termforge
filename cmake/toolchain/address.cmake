include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

find_library(ASAN asan)

if (ASAN_FOUND)
  message(STATUS "Found address sanitizer")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
endif (ASAN_FOUND)
