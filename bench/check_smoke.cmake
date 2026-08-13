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
if (json_error OR NOT schema EQUAL 2)
  message(FATAL_ERROR "invalid benchmark schema: ${json_error}")
endif ()
string(JSON resolved ERROR_VARIABLE json_error GET "${report}" resolved_kernel_tier)
if (json_error OR NOT resolved MATCHES "^(scalar|avx2)$")
  message(FATAL_ERROR "invalid resolved kernel tier: ${json_error}")
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

execute_process(
  COMMAND "${BENCHMARK}" --smoke --kernel-tier scalar --format json
  RESULT_VARIABLE scalar_result
  OUTPUT_VARIABLE scalar_report
  ERROR_VARIABLE scalar_error
)
if (NOT scalar_result EQUAL 0)
  message(FATAL_ERROR "forced scalar benchmark failed: ${scalar_error}")
endif ()
string(JSON forced ERROR_VARIABLE json_error GET "${scalar_report}" resolved_kernel_tier)
if (json_error OR NOT forced STREQUAL "scalar")
  message(FATAL_ERROR "benchmark did not force scalar: ${json_error}")
endif ()
