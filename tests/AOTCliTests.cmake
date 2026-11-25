file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/main.mtr" [=[
context Host::Process grants {} host;
context Host::Loop repeat;
procedure main(@cx: host) -> i32 {
    i32 total = 0;
    for<@cx: repeat> (i32 i) in 0..4 { total = total + i; }
    if (total == 6) { return 23; }
    return 1;
}
]=])
execute_process(COMMAND "${COMPILER}" main.mtr WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT MLIR_ENABLED)
    if(result EQUAL 0 OR NOT stderr MATCHES "METREON_ENABLE_MLIR=ON")
        message(FATAL_ERROR "frontend-only binary emission must be rejected clearly: ${stderr}")
    endif()
    return()
endif()
if(NOT result EQUAL 0 OR NOT stdout STREQUAL "Executable written to main\n" OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "default AoT compilation failed or ran source main: ${result} ${stdout} ${stderr}")
endif()
execute_process(COMMAND ./main WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 23)
    message(FATAL_ERROR "compiled source main returned ${result}, expected 23")
endif()
file(SHA256 "${WORK_DIR}/main" before)
file(WRITE "${WORK_DIR}/bad.mtr" "procedure main() -> i32 { unknown(); return 0; }")
execute_process(COMMAND "${COMPILER}" bad.mtr -o main WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
file(SHA256 "${WORK_DIR}/main" after)
if(result EQUAL 0 OR NOT before STREQUAL after)
    message(FATAL_ERROR "failed compilation replaced the last good executable")
endif()
execute_process(COMMAND "${COMPILER}" main.mtr -o "program with spaces" WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "-o failed: ${stderr}")
endif()
execute_process(COMMAND "${WORK_DIR}/program with spaces" RESULT_VARIABLE result)
if(NOT result EQUAL 23)
    message(FATAL_ERROR "custom output executable failed")
endif()
file(SHA256 "${WORK_DIR}/main.mtr" before)
execute_process(COMMAND "${COMPILER}" main.mtr -o main.mtr WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
file(SHA256 "${WORK_DIR}/main.mtr" after)
if(result EQUAL 0 OR NOT before STREQUAL after)
    message(FATAL_ERROR "compiler overwrote its source input")
endif()
foreach(option --run --launch)
    execute_process(COMMAND "${COMPILER}" "${option}" main.mtr WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result ERROR_VARIABLE stderr)
    if(result EQUAL 0 OR NOT stderr MATCHES "unknown option")
        message(FATAL_ERROR "removed compiler execution option still accepted: ${option}")
    endif()
endforeach()
file(WRITE "${WORK_DIR}/abi_conflict.mtr" [=[
context Host::Process grants {host_io} host;
procedure printf() -> i32 { return 0; }
procedure main(@cx: host) -> i32 { host::print_f32(@cx; 1.0); return 0; }
]=])
file(SHA256 "${WORK_DIR}/main" before)
execute_process(COMMAND "${COMPILER}" abi_conflict.mtr -o main WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
file(SHA256 "${WORK_DIR}/main" after)
if(result EQUAL 0 OR NOT before STREQUAL after OR NOT stderr MATCHES "platform ABI")
    message(FATAL_ERROR "platform symbol conflict was not rejected safely: ${stderr}")
endif()
