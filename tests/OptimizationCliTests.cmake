file(MAKE_DIRECTORY "${WORK_DIR}")

function(compile_example output_variable)
    execute_process(
        COMMAND "${COMPILER}" ${ARGN} "${INPUT}"
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0 OR NOT stderr STREQUAL "")
        message(FATAL_ERROR "compiler failed (${result}): ${stderr}")
    endif()
    if(NOT stdout STREQUAL "GraphIR written to tmp/graphir/optimizations.graphir\n")
        message(FATAL_ERROR "unexpected CLI output: ${stdout}")
    endif()
    file(READ "${WORK_DIR}/tmp/graphir/optimizations.graphir" graphir)
    set(${output_variable} "${graphir}" PARENT_SCOPE)
endfunction()

compile_example(raw --emit=graphir --no-optimization)
if(NOT raw MATCHES "Unused" OR raw MATCHES "graphir.state_member|graphir.context_region")
    message(FATAL_ERROR "--no-optimization did not preserve unoptimized GraphIR")
endif()
compile_example(optimized --emit=graphir)
if(optimized MATCHES "Unused")
    message(FATAL_ERROR "default pipeline did not remove unreachable states")
endif()
foreach(expected IN ITEMS "graphir.state_member" "graphir.internal_transition"
                          "graphir.context_region" "graphir.fused_kernel")
    if(NOT optimized MATCHES "${expected}")
        message(FATAL_ERROR "optimized output is missing ${expected}")
    endif()
endforeach()
compile_example(split --emit graphir)
compile_example(default)
if(NOT split STREQUAL optimized OR NOT default STREQUAL optimized)
    message(FATAL_ERROR "GraphIR emit spellings do not use the same default pipeline")
endif()
compile_example(raw_split --no-optimization --emit graphir)
compile_example(raw_default --no-optimization)
if(NOT raw_split STREQUAL raw OR NOT raw_default STREQUAL raw)
    message(FATAL_ERROR "explicit optimization opt-out depends on emit spelling")
endif()
execute_process(
    COMMAND "${COMPILER}" --emit graphir -
    INPUT_FILE "${INPUT}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0 OR NOT stderr STREQUAL "" OR
   NOT stdout STREQUAL "GraphIR written to tmp/graphir/stdin.graphir\n")
    message(FATAL_ERROR "optimized stdin compilation failed: ${result} ${stdout} ${stderr}")
endif()
file(READ "${WORK_DIR}/tmp/graphir/stdin.graphir" stdin_graphir)
if(stdin_graphir MATCHES "Unused" OR NOT stdin_graphir MATCHES "graphir.context_region")
    message(FATAL_ERROR "stdin compilation bypassed the optimization pipeline")
endif()

file(WRITE "${WORK_DIR}/bad.mtr" [=[
pipeline P() {
    stage a(x: i32) -> i32 { return x; }
    stage b(x: i32) -> i32 { return x; }
    topology { a.x => b.x; b.x => a.x; }
    schedule test { kernel worker = fuse(a, b) { blocks 1; } }
}
]=])
file(WRITE "${WORK_DIR}/tmp/graphir/bad.graphir" "previous output\n")
execute_process(
    COMMAND "${COMPILER}" --emit graphir bad.mtr
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(result EQUAL 0 OR NOT stdout STREQUAL "" OR NOT stderr MATCHES "error\\[graphir.cyclic_fusion\\]")
    message(FATAL_ERROR "invalid fusion did not fail cleanly: ${result} ${stdout} ${stderr}")
endif()
file(READ "${WORK_DIR}/tmp/graphir/bad.graphir" previous)
if(NOT previous STREQUAL "previous output\n")
    message(FATAL_ERROR "fusion failure overwrote the previous artifact")
endif()
execute_process(
    COMMAND "${COMPILER}" --emit=graphir --no-optimization bad.mtr
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0 OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "--no-optimization did not skip fusion verification: ${stderr}")
endif()

function(reject_options)
    execute_process(
        COMMAND "${COMPILER}" ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    )
    if(result EQUAL 0 OR NOT stdout STREQUAL "" OR NOT stderr MATCHES "metreonc:")
        message(FATAL_ERROR "malformed CLI options were accepted: ${ARGN}")
    endif()
endfunction()
reject_options(--emit)
reject_options(--emit invalid "${INPUT}")
reject_options(--emit=invalid "${INPUT}")
reject_options(--no-optimization)
