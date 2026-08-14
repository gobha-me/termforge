# #252: bounded completion coverage for the production game CLI.
#
# Unit coverage already pins App's frame cadence, persistent image lifetime and
# clean-frame behavior. It did not launch the executable whose --benchmark path
# was reported to hang, so every assertion could stay green while the command
# never returned. Keep the timeout here, around the child process itself: the
# outer CTest timeout is a final guard, not the subject of the regression.

if (NOT DEFINED GAME OR NOT EXISTS "${GAME}")
  message(FATAL_ERROR "game benchmark executable is missing: ${GAME}")
endif ()
if (NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "game benchmark output directory was not supplied")
endif ()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(run_game_benchmark frames report_name)
  set(report "${OUTPUT_DIR}/${report_name}")
  file(REMOVE "${report}")
  execute_process(
    COMMAND "${GAME}" --benchmark "${frames}" --report "${report}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 10
  )
  if (NOT result STREQUAL "0")
    message(FATAL_ERROR
      "game --benchmark ${frames} did not complete (${result}): "
      "${stderr}${stdout}")
  endif ()
  if (NOT stdout MATCHES "game workload: frames=${frames} ")
    message(FATAL_ERROR
      "game --benchmark ${frames} omitted its summary: ${stdout}")
  endif ()
  if (NOT EXISTS "${report}")
    message(FATAL_ERROR
      "game --benchmark ${frames} did not write ${report}")
  endif ()

  file(READ "${report}" document)
  string(JSON reported_frames ERROR_VARIABLE json_error
    GET "${document}" frames)
  if (json_error OR NOT reported_frames EQUAL frames)
    message(FATAL_ERROR
      "game report has the wrong frame count: ${json_error}")
  endif ()

  # Return the parsed document to the caller for lifecycle checks specific to
  # the one-frame and clean-frame-spanning runs.
  set(BENCHMARK_REPORT "${document}" PARENT_SCOPE)
endfunction()

function(require_json document key expected)
  string(JSON actual ERROR_VARIABLE json_error GET "${document}" "${key}")
  if (json_error OR NOT actual EQUAL expected)
    message(FATAL_ERROR
      "game report ${key}: expected ${expected}, got ${actual}: ${json_error}")
  endif ()
endfunction()

run_game_benchmark(1 game-benchmark-1.json)
set(one "${BENCHMARK_REPORT}")
require_json("${one}" unique_image_ids 1)
require_json("${one}" peak_live_image_ids 1)
require_json("${one}" data_transmits 1)
require_json("${one}" root_frame_updates 0)
require_json("${one}" placements 1)
require_json("${one}" shutdown_bytes 12)

run_game_benchmark(180 game-benchmark-180.json)
set(full "${BENCHMARK_REPORT}")
require_json("${full}" unique_image_ids 1)
require_json("${full}" peak_live_image_ids 1)
require_json("${full}" data_transmits 1)
require_json("${full}" root_frame_updates 174)
require_json("${full}" placements 1)
require_json("${full}" data_deletes 0)
require_json("${full}" placement_deletes 0)
require_json("${full}" unchanged_frames 5)
require_json("${full}" unchanged_image_bytes 0)
require_json("${full}" shutdown_bytes 12)
