add_executable(53forgetop-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  test.cpp
  ux.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/cli.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/fake_reader.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/forge_top.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/panels.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/proc_reader.cpp
)

target_include_directories(53forgetop-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/lib
  PUBLIC ${PROJECT_SOURCE_DIR}/src/bin
  PUBLIC ${PROJECT_SOURCE_DIR}/test/include
  PUBLIC ${PROJECT_SOURCE_DIR}/test
)

target_link_libraries(53forgetop-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::lib
)
target_compile_definitions(53forgetop-test PRIVATE
  FORGE_TOP_CAPTURES="${PROJECT_SOURCE_DIR}/src/bin/captures/forge-top"
)

# Optional capture utility for the private flagship app, never a consumer API.
add_executable(termforge_forge_top_capture EXCLUDE_FROM_ALL
  capture.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/fake_reader.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/forge_top.cpp
  ${PROJECT_SOURCE_DIR}/src/bin/panels.cpp
)
target_include_directories(termforge_forge_top_capture PRIVATE
  ${PROJECT_SOURCE_DIR}/src/bin
  ${PROJECT_SOURCE_DIR}/test
)
target_link_libraries(termforge_forge_top_capture PRIVATE ${PROJECT_NAME}::lib)

# Keep the built program's identity observable at the executable boundary.
# This target is optional when a consumer enables tests but disables the bin.
if (TARGET forge-top)
  add_test(NAME forge-top-help
    COMMAND $<TARGET_FILE:forge-top> --help
  )
  set_tests_properties(forge-top-help PROPERTIES
    PASS_REGULAR_EXPRESSION "^Usage: forge-top"
  )

  add_test(NAME forge-top-invalid-option
    COMMAND ${CMAKE_COMMAND}
      -DPROGRAM=$<TARGET_FILE:forge-top>
      -P ${CMAKE_CURRENT_LIST_DIR}/assert_cli.cmake
  )
endif ()
