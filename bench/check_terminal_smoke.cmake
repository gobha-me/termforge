execute_process(
  COMMAND "${BENCHMARK}" --smoke --format json --output "${OUTPUT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if (NOT result EQUAL 0)
  message(FATAL_ERROR
    "terminal benchmark smoke failed (${result}): ${stderr}${stdout}")
endif ()

file(READ "${OUTPUT}" report)
string(JSON schema ERROR_VARIABLE json_error GET "${report}" schema_version)
if (json_error OR NOT schema EQUAL 1)
  message(FATAL_ERROR "invalid terminal benchmark schema: ${json_error}")
endif ()
string(JSON live ERROR_VARIABLE json_error GET "${report}" live)
if (json_error OR live)
  message(FATAL_ERROR "terminal benchmark smoke claimed to be live")
endif ()
string(JSON result_count ERROR_VARIABLE json_error LENGTH "${report}" results)
if (json_error OR NOT result_count EQUAL 2)
  message(FATAL_ERROR "terminal smoke must contain two route sentinels")
endif ()

foreach(index RANGE 0 1)
  string(JSON route GET "${report}" results ${index} route)
  string(JSON cells GET "${report}" results ${index} frame_bytes cells)
  string(JSON transmit GET "${report}" results ${index}
    frame_bytes image_transmit)
  string(JSON edit_bytes GET "${report}" results ${index}
    frame_bytes image_edit)
  string(JSON total GET "${report}" results ${index} frame_bytes total)
  string(JSON checksum GET "${report}" results ${index} checksum)
  math(EXPR bucket_total "${cells} + ${transmit} + ${edit_bytes}")
  if (NOT bucket_total EQUAL total OR checksum MATCHES "^0+$")
    message(FATAL_ERROR "terminal result ${index} lost byte accounting")
  endif ()
  if (index EQUAL 0)
    if (NOT route STREQUAL "kitty-full-transmit" OR NOT cells EQUAL 0 OR
        NOT transmit GREATER 0 OR NOT edit_bytes GREATER 0)
      message(FATAL_ERROR "Kitty smoke sentinel lost its transmit route")
    endif ()
  elseif (NOT route STREQUAL "ansi-half-block" OR NOT cells GREATER 0 OR
          NOT transmit EQUAL 0 OR NOT edit_bytes EQUAL 0)
    message(FATAL_ERROR "ANSI smoke sentinel lost its cell route")
  endif ()
endforeach()

execute_process(
  COMMAND "${BENCHMARK}" --route invalid --smoke
  RESULT_VARIABLE invalid_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if (invalid_result EQUAL 0)
  message(FATAL_ERROR "terminal benchmark accepted an invalid route")
endif ()

execute_process(
  COMMAND "${BENCHMARK}" --samples 0 --smoke
  RESULT_VARIABLE samples_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if (samples_result EQUAL 0)
  message(FATAL_ERROR "terminal benchmark accepted zero samples")
endif ()
