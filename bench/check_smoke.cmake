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
if (json_error OR NOT schema EQUAL 4)
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

string(JSON w2_count ERROR_VARIABLE json_error LENGTH "${report}" w2_results)
if (json_error OR NOT w2_count EQUAL 8)
  message(FATAL_ERROR "benchmark smoke must contain eight W2 sentinels: ${json_error}")
endif ()

foreach(index RANGE 0 7)
  string(JSON checksum ERROR_VARIABLE json_error
    GET "${report}" w2_results ${index} checksum)
  if (json_error OR checksum MATCHES "^0+$")
    message(FATAL_ERROR "W2 result ${index} has no checksum: ${json_error}")
  endif ()
  string(JSON cells GET "${report}" w2_results ${index} frame_bytes cells)
  string(JSON transmit GET "${report}" w2_results ${index} frame_bytes image_transmit)
  string(JSON edit_bytes GET "${report}" w2_results ${index} frame_bytes image_edit)
  string(JSON total GET "${report}" w2_results ${index} frame_bytes total)
  math(EXPR bucket_total "${cells} + ${transmit} + ${edit_bytes}")
  if (NOT bucket_total EQUAL total)
    message(FATAL_ERROR "W2 result ${index} byte buckets do not sum")
  endif ()
  string(JSON path GET "${report}" w2_results ${index} path)
  string(JSON motions GET "${report}" w2_results ${index} motion_events)
  string(JSON pins_before GET "${report}"
    w2_results ${index} residency_before pinned_images)
  string(JSON pins_after GET "${report}"
    w2_results ${index} residency_after pinned_images)
  string(JSON payload_before GET "${report}"
    w2_results ${index} residency_before source_payload_bytes)
  string(JSON payload_after GET "${report}"
    w2_results ${index} residency_after source_payload_bytes)
  string(JSON dirty_w GET "${report}" w2_results ${index} dirty_pixels w)
  string(JSON dirty_h GET "${report}" w2_results ${index} dirty_pixels h)
  string(JSON samples GET "${report}" w2_results ${index} samples)
  string(JSON latency ERROR_VARIABLE json_error
    GET "${report}" w2_results ${index} timing_ms input_to_write median)
  if (json_error OR latency LESS 0 OR NOT motions EQUAL samples OR
      NOT pins_before EQUAL 1 OR NOT pins_after EQUAL 1)
    message(FATAL_ERROR "W2 result ${index} lost its input/residency shape")
  endif ()
  math(EXPR payload_delta "${payload_after} - ${payload_before}")
  if (path STREQUAL "replace")
    if (transmit LESS 1 OR NOT edit_bytes EQUAL 0 OR NOT payload_delta EQUAL 0)
      message(FATAL_ERROR "W2 replace result ${index} lost its full-root contract")
    endif ()
  elseif (path STREQUAL "edit")
    math(EXPR expected_delta "${dirty_w} * ${dirty_h} * 4 * ${samples}")
    if (NOT transmit EQUAL 0 OR edit_bytes LESS 1 OR
        NOT payload_delta EQUAL expected_delta)
      message(FATAL_ERROR "W2 edit result ${index} lost its block-edit contract")
    endif ()
  else ()
    message(FATAL_ERROR "W2 result ${index} has invalid path ${path}")
  endif ()
endforeach()

string(JSON w2_wall_count ERROR_VARIABLE json_error LENGTH "${report}" w2_walls)
if (json_error OR NOT w2_wall_count EQUAL 8)
  message(FATAL_ERROR "benchmark smoke must derive eight W2 budget walls: ${json_error}")
endif ()
foreach(index RANGE 0 7)
  string(JSON passing_type TYPE "${report}" w2_walls ${index} largest_passing)
  if (NOT passing_type STREQUAL "OBJECT")
    message(FATAL_ERROR "W2 wall ${index} has no passing canvas")
  endif ()
endforeach()

string(JSON w4_count ERROR_VARIABLE json_error LENGTH "${report}" w4_results)
if (json_error OR NOT w4_count EQUAL 4)
  message(FATAL_ERROR "benchmark smoke must contain four W4 sentinels: ${json_error}")
endif ()

foreach(index RANGE 0 3)
  string(JSON checksum ERROR_VARIABLE json_error
    GET "${report}" w4_results ${index} checksum)
  if (json_error OR checksum MATCHES "^0+$")
    message(FATAL_ERROR "W4 result ${index} has no checksum: ${json_error}")
  endif ()
  string(JSON cells GET "${report}" w4_results ${index} frame_bytes cells)
  string(JSON transmit GET "${report}" w4_results ${index} frame_bytes image_transmit)
  string(JSON edit_bytes GET "${report}" w4_results ${index} frame_bytes image_edit)
  string(JSON total GET "${report}" w4_results ${index} frame_bytes total)
  math(EXPR bucket_total "${cells} + ${transmit} + ${edit_bytes}")
  if (NOT bucket_total EQUAL total)
    message(FATAL_ERROR "W4 result ${index} byte buckets do not sum")
  endif ()
  string(JSON median ERROR_VARIABLE json_error
    GET "${report}" w4_results ${index} timing_ms frame_work median)
  if (json_error OR median LESS 0)
    message(FATAL_ERROR "W4 result ${index} has invalid frame timing: ${json_error}")
  endif ()
  string(JSON payload GET "${report}"
    w4_results ${index} residency source_payload_bytes)
  if (payload LESS 1)
    message(FATAL_ERROR "W4 result ${index} has no accounted payload")
  endif ()
endforeach()

foreach(index RANGE 0 3)
  string(JSON mode GET "${report}" w4_results ${index} mode)
  string(JSON count GET "${report}" w4_results ${index} region_count)
  string(JSON transmit GET "${report}" w4_results ${index} frame_bytes image_transmit)
  string(JSON regions GET "${report}" w4_results ${index} residency region_images)
  string(JSON pins GET "${report}" w4_results ${index} residency pinned_images)
  if (index EQUAL 0)
    if (NOT mode STREQUAL "immediate" OR NOT count EQUAL 16 OR
        NOT transmit EQUAL 0 OR NOT regions EQUAL 16 OR NOT pins EQUAL 0)
      message(FATAL_ERROR "W4 immediate/16 sentinel lost its cache contract")
    endif ()
  elseif (index EQUAL 1)
    if (NOT mode STREQUAL "immediate" OR NOT count EQUAL 17 OR
        transmit LESS 1 OR NOT regions EQUAL 16 OR NOT pins EQUAL 0)
      message(FATAL_ERROR "W4 immediate/17 sentinel did not expose retransmit eviction")
    endif ()
  elseif (index EQUAL 2)
    if (NOT mode STREQUAL "persistent" OR NOT count EQUAL 16 OR
        NOT transmit EQUAL 0 OR NOT regions EQUAL 0 OR NOT pins EQUAL 16)
      message(FATAL_ERROR "W4 persistent/16 sentinel lost pinned residency")
    endif ()
  elseif (NOT mode STREQUAL "persistent" OR NOT count EQUAL 17 OR
          NOT transmit EQUAL 0 OR NOT regions EQUAL 0 OR NOT pins EQUAL 17)
    message(FATAL_ERROR "W4 persistent/17 sentinel crossed the immediate cache wall")
  endif ()
endforeach()

string(JSON w4_wall_count ERROR_VARIABLE json_error LENGTH "${report}" w4_walls)
if (json_error OR NOT w4_wall_count EQUAL 2)
  message(FATAL_ERROR "benchmark smoke must derive both W4 mode walls: ${json_error}")
endif ()
string(JSON immediate_wall GET "${report}" w4_walls 0 first_retransmit_count)
string(JSON persistent_wall_type TYPE "${report}" w4_walls 1 first_retransmit_count)
if (NOT immediate_wall EQUAL 17 OR NOT persistent_wall_type STREQUAL "NULL")
  message(FATAL_ERROR "W4 retransmit walls do not distinguish immediate and persistent")
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
