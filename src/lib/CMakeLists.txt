# TermForge: compiled static library.
add_library(${PROJECT_NAME}_lib STATIC
  core/terminal.cpp
  core/screen.cpp
  core/text.cpp
  core/renderer.cpp
  core/input.cpp
  core/app.cpp
  core/image_loader.cpp
  core/image.cpp
  core/image_transport.cpp
  detail/base64.cpp
  detail/payload_hash.cpp
  detail/simd.cpp
  detail/simd_avx2.cpp
  detail/trace.cpp
  widgets/text_box.cpp
  widgets/composer.cpp
  widgets/table_widget.cpp
  widgets/list_widget.cpp
  widgets/waveform_widget.cpp
  widgets/pixel_surface.cpp
  widgets/label.cpp
  widgets/button.cpp
  widgets/progress_bar.cpp
  widgets/text_input.cpp
  widgets/frame.cpp
  widgets/menu_bar.cpp
  widgets/focus_ring.cpp
  widgets/dialog.cpp
  widgets/dialogs.cpp
  widgets/choice_dialog.cpp
  widgets/checkbox.cpp
  widgets/radio_group.cpp
  widgets/select.cpp
  widgets/file_picker_dialog.cpp
  widgets/map_widget.cpp
  widgets/tab_bar.cpp
  drivers/terminal_driver.cpp
  drivers/ansi_rgb_driver.cpp
  drivers/fallback_driver.cpp
  drivers/kitty_driver.cpp
  drivers/select_driver.cpp
)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  PUBLIC
    # PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the *consumer's*
    # root under add_subdirectory, so headers were simply not found there.
    #
    # The generator expressions are mandatory, not cosmetic: install(EXPORT)
    # hard-errors on an exported target whose interface still names a path
    # inside the source tree. Writing only the INSTALL half is the classic
    # mistake -- it drops the build-tree include dir out of the compile
    # database, which is what clang-tidy consumes.
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_BINARY_DIR}/generated/include
)

target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)

# EXPORT_NAME is the line the whole two-path story hangs on: NAMESPACE
# termforge:: applied to target termforge_lib would synthesise
# termforge::termforge_lib, a *different* spelling than the ALIAS above, so
# find_package consumers and add_subdirectory consumers would disagree. With
# EXPORT_NAME lib both paths give termforge::lib.
# OUTPUT_NAME so the installed archive is libtermforge.a, not libtermforge_lib.a.
set_target_properties(${PROJECT_NAME}_lib PROPERTIES
  EXPORT_NAME lib
  OUTPUT_NAME ${PROJECT_NAME}
)
