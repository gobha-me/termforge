# Embed the committed OBSCURA M0 reveal frames into one build-tree header.
#
# This is example/test plumbing only.  Nothing is installed, and the generated
# include directory is always PRIVATE to its consumers.

function(termforge_embed_obscura_dissolve OUT_HEADER)
  set(_asset_dir "${PROJECT_SOURCE_DIR}/examples/assets/obscura-dissolve")
  set(_declarations "")

  foreach(_frame RANGE 1 8)
    set(_suffix "0${_frame}")
    set(_asset "${_asset_dir}/reveal-${_suffix}.png")
    if (NOT EXISTS "${_asset}")
      message(FATAL_ERROR
        "obscura dissolve asset is missing: ${_asset}\n"
        "  Restore the committed PNGs or regenerate them with\n"
        "  examples/obscura_dissolve_bake.py.")
    endif ()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_asset}")
    file(SIZE "${_asset}" _size)
    if (_size EQUAL 0 OR _size GREATER 8192)
      message(FATAL_ERROR
        "obscura dissolve asset ${_asset} has invalid size ${_size}; expected 1..8192 bytes")
    endif ()
    file(READ "${_asset}" _hex HEX)
    string(REGEX REPLACE "(................................)" "\\1\n" _hex "${_hex}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _bytes "${_hex}")
    string(APPEND _declarations
      "inline constexpr std::array<unsigned char, ${_size}> kReveal${_suffix}{\n"
      "${_bytes}\n"
      "};\n\n")
  endforeach ()

  set(TERMFORGE_OBSCURA_DISSOLVE_DECLARATIONS "${_declarations}")
  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/obscura_dissolve_assets.hpp.in"
    "${OUT_HEADER}"
    @ONLY
  )
endfunction()
