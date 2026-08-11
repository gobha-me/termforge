if (NOT DEFINED PROGRAM)
  message(FATAL_ERROR "PROGRAM is required")
endif ()

execute_process(
  COMMAND "${PROGRAM}" --not-an-option
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if (NOT result EQUAL 2)
  message(FATAL_ERROR "forge-top returned ${result}, expected 2")
endif ()

if (NOT stderr MATCHES "^forge-top: unknown option '--not-an-option'\nUsage: forge-top")
  message(FATAL_ERROR "unexpected forge-top diagnostic:\n${stderr}")
endif ()

get_filename_component(program_dir "${PROGRAM}" DIRECTORY)
if (EXISTS "${program_dir}/termforge")
  message(FATAL_ERROR "old termforge monitor still exists beside forge-top")
endif ()
