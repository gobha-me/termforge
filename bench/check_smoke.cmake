execute_process(
  COMMAND "${BENCHMARK}" --smoke --format json --output "${OUTPUT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if (NOT result EQUAL 0)
  message(FATAL_ERROR "benchmark smoke failed (${result}): ${stderr}${stdout}")
endif ()

file(READ "${OUTPUT}" report)
string(JSON schema ERROR_VARIABLE json_error GET "${report}" schema_version)
if (json_error OR NOT schema EQUAL 1)
  message(FATAL_ERROR "invalid benchmark schema: ${json_error}")
endif ()
string(JSON result_count ERROR_VARIABLE json_error LENGTH "${report}" results)
if (json_error OR result_count LESS 2)
  message(FATAL_ERROR "benchmark report has no results: ${json_error}")
endif ()
foreach(index RANGE 0 1)
  string(JSON checksum ERROR_VARIABLE json_error
    GET "${report}" results ${index} checksum)
  if (json_error OR checksum MATCHES "^0+$")
    message(FATAL_ERROR "benchmark result ${index} has no checksum: ${json_error}")
  endif ()
endforeach()
string(JSON wall_count ERROR_VARIABLE json_error LENGTH "${report}" walls)
if (json_error OR wall_count LESS 1)
  message(FATAL_ERROR "benchmark report has no W3 walls: ${json_error}")
endif ()

execute_process(
  COMMAND "${BENCHMARK}" --suite invalid
  RESULT_VARIABLE invalid_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if (invalid_result EQUAL 0)
  message(FATAL_ERROR "benchmark accepted an invalid suite")
endif ()
