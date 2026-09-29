add_executable(83slider-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  test.cpp
)
target_include_directories(83slider-test PRIVATE
  ${PROJECT_SOURCE_DIR}/examples
  ${PROJECT_SOURCE_DIR}/test
)
target_link_libraries(83slider-test PRIVATE Catch2::Catch2 termforge::lib)
