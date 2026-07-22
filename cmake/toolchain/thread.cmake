include(${CMAKE_CURRENT_LIST_DIR}/default.cmake)

find_library(TSAN tsan)

if (TSAN_FOUND)
  message(STATUS "Found thread sanitizer")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O2 -fsanitize=thread")
endif (TSAN_FOUND)
