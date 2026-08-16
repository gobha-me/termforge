# Keep Kitty's protocol-defined cell-background separator centralized in the
# public semantic mapping. Driver and App code consume ImageLayer::z_index();
# duplicating the raw integer there would let the definitions drift.
file(GLOB_RECURSE shipped_sources
  LIST_DIRECTORIES false
  "${PROJECT_ROOT}/include/*.hpp"
  "${PROJECT_ROOT}/src/*.cpp"
  "${PROJECT_ROOT}/src/*.hpp")

set(matches 0)
set(match_file "")
foreach(source IN LISTS shipped_sources)
  file(READ "${source}" contents)
  string(REGEX MATCHALL "-1073741825" occurrences "${contents}")
  list(LENGTH occurrences source_matches)
  if(source_matches GREATER 0)
    math(EXPR matches "${matches} + ${source_matches}")
    set(match_file "${source}")
  endif()
endforeach()

set(expected "${PROJECT_ROOT}/include/termforge/core/types.hpp")
if(NOT matches EQUAL 1 OR NOT match_file STREQUAL expected)
  message(FATAL_ERROR
    "expected one -1073741825 definition in ${expected}; "
    "found ${matches}, last in ${match_file}")
endif()
