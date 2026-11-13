# These cases exercise serial per-work-item scalar semantics on the CPU. They
# make no claim about GPU execution, synchronization, memory or launch mapping.
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${EXPORTER}" "${INPUT}" "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "GPU scalar test export failed: ${output} ${error}")
endif()
file(GLOB cases "${WORK_DIR}/*.expected")
list(LENGTH cases count)
if(NOT count EQUAL 22)
    message(FATAL_ERROR "expected 22 scalar semantic cases; found ${count}")
endif()
foreach(case IN LISTS cases)
    get_filename_component(stem "${case}" NAME_WLE)
    file(READ "${case}" expected)
    execute_process(COMMAND "${OPT}" "${WORK_DIR}/${stem}.mlir"
        --lower-affine --convert-scf-to-cf --convert-ub-to-llvm --convert-arith-to-llvm
        --convert-func-to-llvm --convert-cf-to-llvm --reconcile-unrealized-casts
        -o "${WORK_DIR}/${stem}.llvm.mlir" RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${stem}: primary dialect conversion failed: ${error}")
    endif()
    execute_process(COMMAND "${TRANSLATE}" --mlir-to-llvmir "${WORK_DIR}/${stem}.llvm.mlir"
        -o "${WORK_DIR}/${stem}.ll" RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${stem}: LLVM translation failed: ${error}")
    endif()
    execute_process(COMMAND "${LLI}" "${WORK_DIR}/${stem}.ll"
        RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 15)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "${stem}: serial result ${result}, expected ${expected}: ${error}")
    endif()
endforeach()
