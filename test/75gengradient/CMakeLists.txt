# 75gengradient: exercise tools/gen_gradient.hpp through ImageLoader.

add_executable(75gengradient-test
  ${PROJECT_SOURCE_DIR}/test/main.cpp
  test.cpp
)

target_include_directories(75gengradient-test
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PUBLIC ${PROJECT_SOURCE_DIR}/src/lib
  PUBLIC ${PROJECT_SOURCE_DIR}/test/include
  PUBLIC ${PROJECT_SOURCE_DIR}/test
  PRIVATE ${PROJECT_SOURCE_DIR}/tools
)

target_link_libraries(75gengradient-test
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}::lib
)
