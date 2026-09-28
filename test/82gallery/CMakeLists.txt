add_executable(82gallery-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  test.cpp
  ${PROJECT_SOURCE_DIR}/examples/gallery_app.cpp
)
target_include_directories(82gallery-test PRIVATE
  ${PROJECT_SOURCE_DIR}/examples
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/lib
  ${PROJECT_SOURCE_DIR}/test
)
target_link_libraries(82gallery-test PRIVATE Catch2::Catch2 ${PROJECT_NAME}::lib)
target_compile_definitions(82gallery-test PRIVATE
  GALLERY_CAPTURE_DIR="${PROJECT_SOURCE_DIR}/examples/captures/widgets"
)

# Opt-in regeneration tool: never builds for an ordinary consumer or test run.
add_executable(termforge_gallery_capture EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/examples/gallery_capture.cpp
  ${PROJECT_SOURCE_DIR}/examples/gallery_app.cpp
)
target_include_directories(termforge_gallery_capture PRIVATE ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(termforge_gallery_capture PRIVATE ${PROJECT_NAME}::lib)
