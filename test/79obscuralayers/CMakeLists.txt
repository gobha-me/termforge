message(STATUS "Configuring test setup 79obscuralayers-test")

add_executable(79obscuralayers-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test.cpp
)

target_include_directories(79obscuralayers-test PRIVATE
  ${PROJECT_SOURCE_DIR}/examples
  ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(79obscuralayers-test PRIVATE
  Catch2::Catch2
  ${PROJECT_NAME}::lib
)
