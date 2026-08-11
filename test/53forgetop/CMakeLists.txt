add_executable(53forgetop-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  test.cpp
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
