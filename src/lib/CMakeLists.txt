# TermForge: compiled static library.
add_library(${PROJECT_NAME}_lib STATIC
  core/terminal.cpp
  core/screen.cpp
  core/renderer.cpp
  core/input.cpp
  core/app.cpp
  core/image_loader.cpp
  detail/base64.cpp
  widgets/text_box.cpp
  widgets/table_widget.cpp
  widgets/list_widget.cpp
  widgets/waveform_widget.cpp
  drivers/ansi_rgb_driver.cpp
  drivers/fallback_driver.cpp
  drivers/kitty_driver.cpp
  drivers/select_driver.cpp
)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)
