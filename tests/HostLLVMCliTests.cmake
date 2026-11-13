file(MAKE_DIRECTORY "${WORK_DIR}/tmp/llvm")
get_filename_component(stem "${INPUT}" NAME_WLE)
set(artifact "${WORK_DIR}/tmp/llvm/${stem}.mlir")
if(NOT MLIR_ENABLED)
    file(WRITE "${artifact}" "previous artifact\n")
    execute_process(COMMAND "${COMPILER}" --emit=llvm "${INPUT}"
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result ERROR_VARIABLE error)
    file(READ "${artifact}" previous)
    if(result EQUAL 0 OR NOT error MATCHES "METREON_ENABLE_MLIR=ON" OR NOT previous STREQUAL "previous artifact\n")
        message(FATAL_ERROR "disabled LLVM emission did not fail without replacing output: ${error}")
    endif()
    return()
endif()

foreach(mode file stdin no_optimization)
    set(input "${INPUT}")
    set(output_stem "${stem}")
    set(extra)
    if(mode STREQUAL stdin)
        set(input "-")
        set(output_stem stdin)
    elseif(mode STREQUAL no_optimization)
        set(extra --no-optimization)
    endif()
    execute_process(COMMAND "${COMPILER}" --emit llvm ${extra} "${input}"
        INPUT_FILE "${INPUT}" WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0 OR NOT error STREQUAL "" OR
       NOT output STREQUAL "LLVM MLIR written to tmp/llvm/${output_stem}.mlir\n")
        message(FATAL_ERROR "host LLVM emission failed: ${result} ${output} ${error}")
    endif()
    set(current "${WORK_DIR}/tmp/llvm/${output_stem}.mlir")
    file(READ "${current}" text)
    if(NOT text MATCHES "llvm.func @main\\(\\)" OR NOT text MATCHES "llvm.cond_br" OR
       text MATCHES "metreon.procedure|unrealized_conversion_cast|gpu.launch|nvvm.")
        message(FATAL_ERROR "output is not complete host LLVM-dialect MLIR")
    endif()
    execute_process(COMMAND "${INSPECTOR}" "${current}" -o "${WORK_DIR}/roundtrip.mlir"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "LLVM-dialect round-trip failed: ${error}")
    endif()
endforeach()

# Target rejection occurs after valid source/GraphIR import and must not clobber
# a previously generated native artifact with the same input stem.
file(MAKE_DIRECTORY "${WORK_DIR}/unsupported")
set(invalid "${WORK_DIR}/unsupported/${stem}.mtr")
file(WRITE "${invalid}" "context Gpu::Thread grants {} gpu; procedure device(@cx: gpu) -> i32 { return 0; }")
file(READ "${artifact}" before)
execute_process(COMMAND "${COMPILER}" --emit=llvm "${invalid}"
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(READ "${artifact}" after)
if(result EQUAL 0 OR NOT output STREQUAL "" OR NOT before STREQUAL after OR
   NOT error MATCHES "host procedures only")
    message(FATAL_ERROR "host target rejection did not preserve output: ${error}")
endif()
