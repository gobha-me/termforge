add_executable(forge-top
  cli.cpp
  fake_reader.cpp
  forge_top.cpp
  main.cpp
  panels.cpp
  proc_reader.cpp
)
target_link_libraries(forge-top
  PRIVATE ${PROJECT_NAME}::lib
)
