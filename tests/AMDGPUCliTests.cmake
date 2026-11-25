# Offline tests only: no generated AMD program is executed.
file(MAKE_DIRECTORY "${WORK_DIR}")
foreach(mode rocdl amdgpu hsaco)
    execute_process(COMMAND "${COMPILER}" --emit=${mode} --arch gfx1030 "${INPUT}"
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0 OR NOT stderr STREQUAL "")
        message(FATAL_ERROR "AMD ${mode} emission failed: ${result} ${stdout} ${stderr}")
    endif()
endforeach()
foreach(stage mlir gpu rocdl)
    execute_process(COMMAND "${INSPECTOR}" "${WORK_DIR}/tmp/${stage}/vector_add.mlir" -o /dev/null
        RESULT_VARIABLE result ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "invalid ${stage} intermediate: ${stderr}")
    endif()
endforeach()
# The emitted assembly must assemble independently, with the matching AMD LLVM
# target, and link without unresolved device dependencies.
execute_process(COMMAND "${MC}" -triple=amdgcn-amd-amdhsa -mcpu=gfx1030 -filetype=obj
    "${WORK_DIR}/tmp/amdgpu/vector_add.s" -o "${WORK_DIR}/assembled.o"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AMDGPU assembly failed to assemble: ${stderr}")
endif()
execute_process(COMMAND "${AMD_LINKER}" -shared --no-undefined "${WORK_DIR}/assembled.o" -o "${WORK_DIR}/assembled.hsaco"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AMD assembly failed to link: ${stderr}")
endif()
function(check_code_object path)
    execute_process(COMMAND "${READOBJ}" --file-headers --notes --dyn-symbols "${path}"
        RESULT_VARIABLE result OUTPUT_VARIABLE text ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0 OR NOT text MATCHES "EM_AMDGPU" OR NOT text MATCHES "SharedObject" OR
        NOT text MATCHES "GFX1030" OR NOT text MATCHES "vector_add.kd" OR NOT text MATCHES "hidden_group_size_x" OR
        NOT text MATCHES "kernarg_segment_size: 384" OR NOT text MATCHES "ABIVersion: 3")
        message(FATAL_ERROR "invalid HSA code object or kernel ABI: ${stderr} ${text}")
    endif()
endfunction()
check_code_object("${WORK_DIR}/assembled.hsaco")
check_code_object("${WORK_DIR}/tmp/hsaco/vector_add.hsaco")
execute_process(COMMAND "${COMPILER}" --target=amdgpu --arch gfx1030 "${INPUT}" -o "${WORK_DIR}/vector_add_amd"
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "AMD native executable compilation failed: ${result} ${stdout} ${stderr}")
endif()
execute_process(COMMAND "${READOBJ}" --file-headers --sections --needed-libs --dyn-symbols "${WORK_DIR}/vector_add_amd"
    RESULT_VARIABLE result OUTPUT_VARIABLE native ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT native MATCHES "EM_X86_64|EM_AARCH64" OR NOT native MATCHES "[.]metreon.amdgpu" OR
    NOT native MATCHES "libamdhip64" OR NOT native MATCHES "hipModuleLaunchKernel" OR NOT native MATCHES "hipMalloc" OR
    NOT native MATCHES "hipMemcpyHtoD" OR native MATCHES "libcuda|cuLaunchKernel|libLLVM|libmetreon")
    message(FATAL_ERROR "wrong native host image or GPU dependencies: ${stderr} ${native}")
endif()
execute_process(COMMAND "${OBJCOPY}" "--dump-section=.metreon.amdgpu=${WORK_DIR}/embedded.hsaco" "${WORK_DIR}/vector_add_amd"
    RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cannot extract embedded device image: ${stderr}")
endif()
check_code_object("${WORK_DIR}/embedded.hsaco")
# Close the same pipeline around a mutable device loop and a context-bearing
# helper, including the address-space-5 slots checked by the lowering unit test.
file(READ "${INPUT}" source)
string(REPLACE "gpu::store(@cx; out, i, left + right);"
    "f32 total = 0.0; for<@cx: repeat> (i32 pass) in 0..2 { if (pass == 0) { continue; } total = sum(@cx; left, right); } gpu::store(@cx; out, i, total);"
    loops "${source}")
file(WRITE "${WORK_DIR}/loops.mtr" "context Gpu::Loop repeat;\nprocedure sum(@cx: gpu_thread; a: f32, b: f32) -> f32 { return a + b; }\n${loops}")
execute_process(COMMAND "${COMPILER}" --target amdgpu --arch gfx1030 loops.mtr -o loops_amd
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT stderr STREQUAL "")
    message(FATAL_ERROR "AMD loop/helper AoT linking failed: ${result} ${stdout} ${stderr}")
endif()
# Failure must preserve the previous executable and must never initialize HIP.
file(SHA256 "${WORK_DIR}/vector_add_amd" before)
execute_process(COMMAND "${COMPILER}" --target amdgpu --arch gfx9999 "${INPUT}" -o "${WORK_DIR}/vector_add_amd"
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result ERROR_VARIABLE stderr)
file(SHA256 "${WORK_DIR}/vector_add_amd" after)
if(result EQUAL 0 OR NOT before STREQUAL after OR NOT stderr MATCHES "unsupported AMDGPU")
    message(FATAL_ERROR "bad AMD architecture was not rejected safely: ${stderr}")
endif()
execute_process(COMMAND "${COMPILER}" --target cuda --emit=rocdl "${INPUT}"
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result ERROR_VARIABLE stderr)
if(result EQUAL 0 OR NOT stderr MATCHES "match the requested output dialect")
    message(FATAL_ERROR "conflicting target and dialect were accepted")
endif()
