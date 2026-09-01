message(STATUS "Configuring test setup 78obscuradissolve-test")

include(${PROJECT_SOURCE_DIR}/cmake/embed_obscura_dissolve.cmake)
termforge_embed_obscura_dissolve(
  ${PROJECT_BINARY_DIR}/generated/termforge/examples/obscura_dissolve_assets.hpp
)

add_executable(78obscuradissolve-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test.cpp
)

target_include_directories(78obscuradissolve-test PRIVATE
  ${PROJECT_SOURCE_DIR}/examples
  ${PROJECT_SOURCE_DIR}/test
  ${PROJECT_BINARY_DIR}/generated
)

target_link_libraries(78obscuradissolve-test PRIVATE
  Catch2::Catch2
  ${PROJECT_NAME}::lib
)
