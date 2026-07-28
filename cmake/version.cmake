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
  # `git describe` searches *upward* for a .git, so it will happily answer from
  # a repository that merely contains us. Vendor termforge as a plain directory
  # inside another project (external/termforge/, not a submodule) and describe
  # returns the *consumer's* tag, which project() would then report as
  # termforge's version -- v9.9.9 of some app becomes termforge 9.9.9.
  #
  # Every arrangement whose tags are actually ours puts the repository top
  # level exactly here: a dev clone, a submodule (.git file, still its own
  # repo) and FetchContent's _deps/termforge-src (a real clone). So ask which
  # repo git resolved to and only trust the tag when the answer is us.
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE GIT_ROOT_RESULT
    OUTPUT_VARIABLE GIT_ROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  # REAL_PATH on both sides: describe's answer is already canonical, while ours
  # still contains the /.. -- and either may sit behind a symlink, which would
  # fail a plain string compare between two names for the same directory.
  set(GIT_OWNS_TERMFORGE FALSE)
  if (GIT_ROOT_RESULT EQUAL 0)
    file(REAL_PATH "${GIT_ROOT}" GIT_ROOT)
    file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." TERMFORGE_ROOT)
    if (GIT_ROOT STREQUAL TERMFORGE_ROOT)
      set(GIT_OWNS_TERMFORGE TRUE)
    endif ()
  endif ()

  if (NOT GIT_OWNS_TERMFORGE)
    # Falling through to the default is the whole point: a wrong version is
    # worse than an honest placeholder, and -DTERMFORGE_VERSION is the fix.
    message(STATUS
      "termforge: not a termforge git checkout, ignoring the enclosing repository's tags"
      " -- pass -DTERMFORGE_VERSION=x.y.z to set the version")
  else ()
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
  endif ()
endif ()

if (NOT TERMFORGE_VERSION)
  set(TERMFORGE_VERSION 0.0.0.1)
endif()
