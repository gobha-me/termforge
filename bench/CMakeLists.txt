add_executable(${PROJECT_NAME}_bench
  main.cpp
)

target_include_directories(${PROJECT_NAME}_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/lib
)

target_link_libraries(${PROJECT_NAME}_bench PRIVATE ${PROJECT_NAME}::lib)
target_compile_features(${PROJECT_NAME}_bench PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME}_bench PRIVATE
  TERMFORGE_BENCH_VERSION="${PROJECT_VERSION}"
)

if (${PROJECT_NAME}_TESTS AND BUILD_TESTING)
  add_test(
    NAME benchmark-smoke
    COMMAND ${CMAKE_COMMAND}
      -DBENCHMARK=$<TARGET_FILE:${PROJECT_NAME}_bench>
      -DOUTPUT=${PROJECT_BINARY_DIR}/benchmark-smoke.json
      -P ${CMAKE_CURRENT_LIST_DIR}/check_smoke.cmake
  )
endif ()
