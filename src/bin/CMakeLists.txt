add_executable(${PROJECT_NAME}
  cli.cpp
  fake_reader.cpp
  forge_top.cpp
  main.cpp
  panels.cpp
  proc_reader.cpp
)
target_link_libraries(${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}::lib
)
