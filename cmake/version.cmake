find_package (Git)

# There are some potential business rules here
#  Ultimately the idea is to ease release and not need commits just to roll a version
# Also, when there is a version.hpp.in file, this need valid values for compile
#  which has an effect on dev build test cycle for a developer

# TERMFORGE_VERSION, not a bare VERSION: under add_subdirectory this file runs
# in a scope that inherits the consumer's variables, and a parent project's
# stray `VERSION` would silently become termforge's project version (the
# fallback below would never fire). A packager building from a git-less tarball
# can pin it with -DTERMFORGE_VERSION=x.y.z.
if (GIT_FOUND AND NOT TERMFORGE_VERSION)
  # Retrieve nearest tag
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE GIT_RESULT
    OUTPUT_VARIABLE GIT_TAG_STR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if (GIT_RESULT AND NOT GIT_RESULT EQUAL 0)
    message(STATUS "No tags found")
  else()
    string(REGEX MATCH "^[rv]?([0-9]+([^-][0-9]+)+)((-.+)+)?$" CURRENT_VERSION "${GIT_TAG_STR}")

    if (CMAKE_MATCH_1)
      set(TERMFORGE_VERSION ${CMAKE_MATCH_1})
    endif()

    if (CMAKE_MATCH_3) # Use inplace of tweak?
      set(DIRTY_BRANCH ${CMAKE_MATCH_3})
    endif()
  endif()
endif (GIT_FOUND)

if (NOT TERMFORGE_VERSION)
  set(TERMFORGE_VERSION 0.0.0.1)
endif()
