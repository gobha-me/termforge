# TermForge examples
# Each example is a standalone executable linked against the library.

set(EXAMPLES
  hello
  colors
  image
  input
  chat
  low_level
  dashboard
  widgets
  dialogs
)

foreach(EXAMPLE ${EXAMPLES})
  set(TARGET ${PROJECT_NAME}_example_${EXAMPLE})
  add_executable(${TARGET} ${EXAMPLE}.cpp)
  target_link_libraries(${TARGET} PRIVATE ${PROJECT_NAME}::lib)
endforeach()
