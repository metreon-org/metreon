file(MAKE_DIRECTORY "${WORK_DIR}/invalid/tmp/graphir" "${WORK_DIR}/blocked")
file(WRITE "${WORK_DIR}/invalid/bad.mtr" "procedure f() -> i32 {}")
file(WRITE "${WORK_DIR}/invalid/tmp/graphir/bad.graphir" "previous output\n")
execute_process(
    COMMAND "${COMPILER}" "bad.mtr"
    WORKING_DIRECTORY "${WORK_DIR}/invalid"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(result EQUAL 0 OR NOT stdout STREQUAL "" OR NOT stderr MATCHES "error\\[sema\\.")
    message(FATAL_ERROR "invalid input did not fail cleanly: ${result} ${stdout} ${stderr}")
endif()
file(READ "${WORK_DIR}/invalid/tmp/graphir/bad.graphir" previous)
if(NOT previous STREQUAL "previous output\n")
    message(FATAL_ERROR "invalid compilation overwrote the previous output")
endif()

# A regular file at tmp prevents output-directory creation, even as root.
file(WRITE "${WORK_DIR}/blocked/tmp" "not a directory\n")
file(WRITE "${WORK_DIR}/blocked/valid.mtr" "procedure f() -> void {}")
execute_process(
    COMMAND "${COMPILER}" "valid.mtr"
    WORKING_DIRECTORY "${WORK_DIR}/blocked"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(result EQUAL 0 OR NOT stdout STREQUAL "" OR NOT stderr MATCHES "metreonc:")
    message(FATAL_ERROR "output-directory failure was not reported: ${result} ${stdout} ${stderr}")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}/unwritable/tmp/graphir/valid.graphir")
file(WRITE "${WORK_DIR}/unwritable/valid.mtr" "procedure f() -> void {}")
execute_process(
    COMMAND "${COMPILER}" "valid.mtr"
    WORKING_DIRECTORY "${WORK_DIR}/unwritable"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(result EQUAL 0 OR NOT stdout STREQUAL "" OR NOT stderr MATCHES "cannot open GraphIR output")
    message(FATAL_ERROR "output-file failure was not reported: ${result} ${stdout} ${stderr}")
endif()
