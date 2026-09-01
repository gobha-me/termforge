add_test(NAME startup COMMAND ${CMAKE_CURRENT_LIST_DIR}/../cmake/startup.sh)
add_test(NAME shutdown COMMAND ${CMAKE_CURRENT_LIST_DIR}/../cmake/shutdown.sh)
add_test(
  NAME layer-z-single-definition
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_ROOT=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/check_layer_z.cmake
)
add_test(
  NAME catch2-without-git
  COMMAND ${CMAKE_COMMAND}
          -DPROJECT_ROOT=${PROJECT_SOURCE_DIR}
          -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
          -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
          -DCMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}/catch2-without-git-check
          -P ${PROJECT_SOURCE_DIR}/cmake/check_catch2_without_git.cmake
)

# The committed M0 reveal frames are generated art.  Verify decompressed
# palette indices rather than zlib bytes, whose representation may move across
# Python/zlib versions without changing one pixel.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND)
  add_test(
    NAME obscura-dissolve-assets-check
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/examples/obscura_dissolve_bake.py
            --check ${PROJECT_SOURCE_DIR}/examples/assets/obscura-dissolve
  )
else ()
  add_test(
    NAME obscura-dissolve-assets-check
    COMMAND ${CMAKE_COMMAND} -E echo
      "obscura-dissolve-assets-check: python3 is required to verify the committed reveal PNGs"
  )
  set_tests_properties(obscura-dissolve-assets-check PROPERTIES WILL_FAIL TRUE)
endif ()

file(GLOB TEST_DIRS ${CMAKE_CURRENT_LIST_DIR}/*)

foreach(TEST_DIR ${TEST_DIRS})
  cmake_path(GET TEST_DIR FILENAME TEST_NAME_PATH)
  set(TEST_NAME ${TEST_NAME_PATH}-test)

  if(EXISTS ${TEST_DIR}/CMakeLists.txt)
    add_subdirectory(${TEST_DIR})
  elseif (EXISTS ${TEST_DIR}/test.cpp)
    message(STATUS "Configuring test setup ${TEST_NAME}")

    file(GLOB SRCS ${TEST_DIR}/*.cpp)

    add_executable(${TEST_NAME}
      main.cpp
      ${SRCS}
    )

    target_include_directories(${TEST_NAME}
      PUBLIC ${PROJECT_SOURCE_DIR}/include
      PUBLIC ${PROJECT_SOURCE_DIR}/src/lib
      PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include
      PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    )

    target_link_libraries(${TEST_NAME}
      PRIVATE Catch2::Catch2
      PRIVATE ${PROJECT_NAME}::lib
    )
  else()
    continue()
  endif()

  add_test(
    NAME ${TEST_NAME}
    COMMAND ${TEST_NAME} -s
  )

  set_tests_properties(${TEST_NAME} PROPERTIES FIXTURES_REQUIRED runners)
endforeach()

# SET FIXTURE DEPENDENCIES
set_tests_properties(startup PROPERTIES FIXTURES_SETUP runners)
set_tests_properties(shutdown PROPERTIES FIXTURES_CLEANUP runners)
