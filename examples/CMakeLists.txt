# TermForge examples
# Each example is a standalone executable linked against the library.

set(EXAMPLES
  hello
  colors
  image
  sprites
  pinned
  animation_control
  obscura_dissolve
  input
  chat
  low_level
  dashboard
  game
  motion
  pixel_surface
  widgets
  dialogs
  forms
)

foreach(EXAMPLE ${EXAMPLES})
  set(TARGET ${PROJECT_NAME}_example_${EXAMPLE})
  add_executable(${TARGET} ${EXAMPLE}.cpp)
  target_link_libraries(${TARGET} PRIVATE ${PROJECT_NAME}::lib)
endforeach()

# OBSCURA's M0 spike carries committed PNG frames rather than runtime file I/O.
# Generate the private byte arrays at configure time for the example; its
# focused test calls the same helper so tests remain buildable when examples
# are disabled.
include(${PROJECT_SOURCE_DIR}/cmake/embed_obscura_dissolve.cmake)
termforge_embed_obscura_dissolve(
  ${PROJECT_BINARY_DIR}/generated/termforge/examples/obscura_dissolve_assets.hpp
)
target_include_directories(${PROJECT_NAME}_example_obscura_dissolve PRIVATE
  ${PROJECT_BINARY_DIR}/generated
)

# #252: exercise the shipped CLI, not a replay of GameWorkload's internals.
# This lives after the example targets (rather than in test/) because test/ is
# configured before examples/ and therefore cannot name the executable yet.
if (${PROJECT_NAME}_TESTS AND BUILD_TESTING)
  add_test(
    NAME game-benchmark-cli
    COMMAND ${CMAKE_COMMAND}
      -DGAME=$<TARGET_FILE:${PROJECT_NAME}_example_game>
      -DOUTPUT_DIR=${PROJECT_BINARY_DIR}/Testing/Temporary
      -P ${CMAKE_CURRENT_LIST_DIR}/check_game_benchmark.cmake
  )
  set_tests_properties(game-benchmark-cli PROPERTIES
    FIXTURES_REQUIRED runners
    TIMEOUT 30
  )
endif ()
