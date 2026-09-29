add_executable(84numeric-test ${PROJECT_SOURCE_DIR}/test/main.cpp test.cpp)
target_include_directories(84numeric-test PRIVATE
  ${PROJECT_SOURCE_DIR}/examples ${PROJECT_SOURCE_DIR}/test)
target_link_libraries(84numeric-test PRIVATE Catch2::Catch2 termforge::lib)
