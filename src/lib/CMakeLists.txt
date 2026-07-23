# TermForge: compiled static library.
add_library(${PROJECT_NAME}_lib STATIC
  core/terminal.cpp
  core/screen.cpp
  core/renderer.cpp
  core/input.cpp
  core/app.cpp
  widgets/text_box.cpp
  drivers/ansi_rgb_driver.cpp
  drivers/fallback_driver.cpp
  drivers/select_driver.cpp
)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)
