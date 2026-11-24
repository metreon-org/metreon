cmake_minimum_required(VERSION 3.20)

project(MetreonCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(METREON_ENABLE_MLIR "Build the Metreon MLIR dialect and GraphIR lowering" OFF)
option(METREON_TEST_CUDA_EXECUTION "Run integration tests on an actual CUDA GPU" OFF)

if(METREON_TEST_CUDA_EXECUTION AND NOT METREON_ENABLE_MLIR)
    message(FATAL_ERROR "METREON_TEST_CUDA_EXECUTION requires METREON_ENABLE_MLIR=ON")
endif()

if(METREON_ENABLE_MLIR)
    # LLVM's package configuration performs C dependency checks.
    enable_language(C)
    find_package(MLIR REQUIRED CONFIG)

    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}" "${MLIR_CMAKE_DIR}")
    include(TableGen)
    include(AddLLVM)
    include(AddMLIR)

    add_subdirectory(dialect)
endif()

function(metreon_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()

add_library(metreon_frontend
    lib/GraphIR/ControlFlow.cpp
    lib/GraphIR/Graph.cpp
    lib/GraphIR/Composition.cpp
    lib/GraphIR/Lowering.cpp
    lib/GraphIR/Passes.cpp
    lib/GraphIR/Passes/DeadTransitionElimination.cpp
    lib/GraphIR/Passes/StateChainCollapse.cpp
    lib/GraphIR/Passes/ContextHoisting.cpp
    lib/GraphIR/Passes/FusionLegality.cpp
    lib/Lexer/Lexer.cpp
    lib/Parser/Parser.cpp
    lib/Parser/CompositionParser.cpp
    lib/Sema/ContextValidator.cpp
    lib/Sema/CallableValidator.cpp
    lib/Sema/CompositionValidator.cpp
)

target_include_directories(metreon_frontend
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

metreon_enable_warnings(metreon_frontend)

if(METREON_ENABLE_MLIR)
    metreon_enable_warnings(metreon_dialect)
    add_library(metreon_mlir_import lib/MLIR/Import.cpp)
    target_link_libraries(metreon_mlir_import PUBLIC metreon_frontend metreon_dialect)
    metreon_enable_warnings(metreon_mlir_import)
    add_dependencies(metreon_mlir metreon_mlir_import)
    add_library(metreon_scalar_lowering lib/MLIR/ScalarLowering.cpp)
    target_link_libraries(metreon_scalar_lowering PUBLIC metreon_mlir_import
        MLIRFuncDialect MLIRArithDialect MLIRControlFlowDialect MLIRMemRefDialect)
    metreon_enable_warnings(metreon_scalar_lowering)
    add_library(metreon_host_lowering lib/MLIR/HostLowering.cpp)
    target_link_libraries(metreon_host_lowering PUBLIC metreon_scalar_lowering
        MLIRFuncDialect MLIRArithDialect MLIRControlFlowDialect MLIRMemRefDialect
        MLIRFuncToLLVM MLIRArithToLLVM MLIRControlFlowToLLVM MLIRMemRefToLLVM)
    metreon_enable_warnings(metreon_host_lowering)
    add_dependencies(metreon_mlir metreon_host_lowering)
    add_library(metreon_gpu_lowering lib/MLIR/GPULowering.cpp)
    target_link_libraries(metreon_gpu_lowering PUBLIC metreon_scalar_lowering
        MLIRGPUDialect MLIRAffineDialect MLIRSCFDialect MLIRUBDialect
        MLIRControlFlowToSCF MLIRMemRefDialect MLIRTransforms)
    metreon_enable_warnings(metreon_gpu_lowering)
    add_dependencies(metreon_mlir metreon_gpu_lowering)
    add_library(metreon_device_execution lib/MLIR/DeviceExecution.cpp lib/MLIR/NVVMBackend.cpp lib/MLIR/ROCDLBackend.cpp)
    target_include_directories(metreon_device_execution PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
    target_link_libraries(metreon_device_execution PUBLIC metreon_scalar_lowering
        MLIRGPUToNVVMTransforms MLIRFuncToLLVM MLIRArithToLLVM MLIRControlFlowToLLVM MLIRMemRefToLLVM
        MLIRNVVMToLLVMIRTranslation MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation MLIRTargetLLVMIRExport
        MLIRGPUToROCDLTransforms MLIRROCDLToLLVMIRTranslation LLVM)
    metreon_enable_warnings(metreon_device_execution)
    add_dependencies(metreon_mlir metreon_device_execution)
    add_library(metreon_aot lib/MLIR/HostExecution.cpp lib/CodeGen/AOT.cpp)
    target_link_libraries(metreon_aot PUBLIC metreon_device_execution)
    target_include_directories(metreon_aot PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
    metreon_enable_warnings(metreon_aot)
    add_dependencies(metreon_mlir metreon_aot)
    # Tools and import library used by the compiler's link step. metreonc itself
    # has no CUDA/HIP dependency and never initializes a GPU.
    find_package(CUDAToolkit QUIET)
    find_program(METREON_PTXAS NAMES ptxas HINTS "${CUDAToolkit_BIN_DIR}" /usr/local/cuda/bin)
    find_library(METREON_CUDA_DRIVER_LIBRARY NAMES cuda HINTS
        "${CUDAToolkit_LIBRARY_DIR}" "${CUDAToolkit_LIBRARY_DIR}/stubs" /usr/lib/wsl/lib)
    find_program(METREON_AMDGPU_LINKER NAMES ld.lld-${LLVM_VERSION_MAJOR} ld.lld
        HINTS "${LLVM_TOOLS_BINARY_DIR}" /opt/rocm/llvm/bin)
    find_library(METREON_HIP_LIBRARY NAMES amdhip64 HINTS /opt/rocm/lib /opt/rocm/lib64)
    find_file(METREON_ROCM_OCKL NAMES ockl.bc HINTS /opt/rocm/amdgcn/bitcode /opt/rocm/lib/bitcode)
    find_file(METREON_ROCM_ABI500 NAMES oclc_abi_version_500.bc HINTS /opt/rocm/amdgcn/bitcode /opt/rocm/lib/bitcode)
    configure_file(cmake/Toolchain.h.in include/metreon/CodeGen/Toolchain.h @ONLY)
endif()

add_executable(metreonc tools/metreonc.cpp)
target_link_libraries(metreonc PRIVATE metreon_frontend)
if(METREON_ENABLE_MLIR)
    target_link_libraries(metreonc PRIVATE metreon_mlir_import metreon_host_lowering metreon_gpu_lowering metreon_device_execution metreon_aot)
    target_compile_definitions(metreonc PRIVATE METREON_ENABLE_MLIR=1)
    add_executable(metreon-opt tools/metreon-opt.cpp)
    target_link_libraries(metreon-opt PRIVATE metreon_dialect MLIROptLib MLIRLLVMDialect
        MLIRGPUDialect MLIRAffineDialect MLIRSCFDialect MLIRFuncDialect MLIRArithDialect MLIRUBDialect
        MLIRMemRefDialect MLIRControlFlowDialect MLIRNVVMDialect MLIRROCDLDialect)
    metreon_enable_warnings(metreon-opt)
endif()
metreon_enable_warnings(metreonc)

if(BUILD_TESTING)
    add_executable(metreon_structured_cfg_tests tests/StructuredControlFlowTests.cpp)
    target_link_libraries(metreon_structured_cfg_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_structured_cfg_tests)
    add_test(NAME metreon.graphir.structured_cfg COMMAND metreon_structured_cfg_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/loops_and_conditionals.mtr)
    set(metreon_mlir_cli_options -DMLIR_ENABLED=OFF)
    if(METREON_ENABLE_MLIR)
        add_executable(metreon_rocdl_tests tests/ROCDLLoweringTests.cpp)
        target_link_libraries(metreon_rocdl_tests PRIVATE metreon_device_execution MLIRParser)
        metreon_enable_warnings(metreon_rocdl_tests)
        set(metreon_rocdl_test_options)
        if(METREON_ROCM_OCKL AND METREON_ROCM_ABI500)
            set(metreon_rocdl_test_options codegen)
        endif()
        add_test(NAME metreon.amdgpu.rocdl COMMAND metreon_rocdl_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr ${metreon_rocdl_test_options})
        find_program(METREON_LLVM_MC NAMES llvm-mc-${LLVM_VERSION_MAJOR} llvm-mc HINTS "${LLVM_TOOLS_BINARY_DIR}")
        find_program(METREON_LLVM_READOBJ NAMES llvm-readobj-${LLVM_VERSION_MAJOR} llvm-readobj HINTS "${LLVM_TOOLS_BINARY_DIR}")
        find_program(METREON_LLVM_OBJCOPY NAMES llvm-objcopy-${LLVM_VERSION_MAJOR} llvm-objcopy HINTS "${LLVM_TOOLS_BINARY_DIR}")
        if(METREON_ROCM_OCKL AND METREON_ROCM_ABI500 AND METREON_AMDGPU_LINKER AND METREON_HIP_LIBRARY AND
           METREON_LLVM_MC AND METREON_LLVM_READOBJ AND METREON_LLVM_OBJCOPY)
            add_test(NAME metreon.amdgpu.aot.offline COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:metreonc> -DINSPECTOR=$<TARGET_FILE:metreon-opt>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/amd
                -DMC=${METREON_LLVM_MC} -DREADOBJ=${METREON_LLVM_READOBJ} -DOBJCOPY=${METREON_LLVM_OBJCOPY}
                -DAMD_LINKER=${METREON_AMDGPU_LINKER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/AMDGPUCliTests.cmake)
        endif()
        add_executable(metreon_host_execution_tests tests/HostExecutionTests.cpp)
        target_link_libraries(metreon_host_execution_tests PRIVATE metreon_aot MLIRParser)
        metreon_enable_warnings(metreon_host_execution_tests)
        add_test(NAME metreon.aot.host.contexts COMMAND metreon_host_execution_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr)
        add_executable(metreon_device_execution_tests tests/DeviceExecutionTests.cpp)
        target_link_libraries(metreon_device_execution_tests PRIVATE metreon_device_execution MLIRParser)
        metreon_enable_warnings(metreon_device_execution_tests)
        add_test(NAME metreon.gpu.device.lowering COMMAND metreon_device_execution_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr)
        add_test(NAME metreon.gpu.device.cli COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:metreonc> -DINSPECTOR=$<TARGET_FILE:metreon-opt>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/device
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/DeviceExecutionCliTests.cmake)
        if(METREON_TEST_CUDA_EXECUTION)
            add_test(NAME metreon.gpu.device.execution COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:metreonc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/vector_add.mtr
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/cuda
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/AOTGPUExecutionTests.cmake)
            set_tests_properties(metreon.gpu.device.execution PROPERTIES LABELS "gpu;cuda" TIMEOUT 120)
        endif()
        add_executable(metreon_gpu_lowering_tests tests/GPULoweringTests.cpp)
        target_link_libraries(metreon_gpu_lowering_tests PRIVATE metreon_gpu_lowering MLIRParser)
        metreon_enable_warnings(metreon_gpu_lowering_tests)
        add_test(NAME metreon.gpu.control_flow.import COMMAND metreon_gpu_lowering_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/GPUControlFlow.mtr)
        add_executable(metreon_host_lowering_tests tests/HostLoweringTests.cpp)
        target_link_libraries(metreon_host_lowering_tests PRIVATE metreon_host_lowering MLIRParser)
        metreon_enable_warnings(metreon_host_lowering_tests)
        add_test(NAME metreon.host.llvm.import COMMAND metreon_host_lowering_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/HostControlFlow.mtr)

        find_program(METREON_MLIR_TRANSLATE NAMES mlir-translate-${LLVM_VERSION_MAJOR} mlir-translate
            HINTS "${MLIR_INSTALL_PREFIX}/bin" "${LLVM_TOOLS_BINARY_DIR}")
        find_program(METREON_LLI NAMES lli-${LLVM_VERSION_MAJOR} lli HINTS "${LLVM_TOOLS_BINARY_DIR}")
        find_program(METREON_MLIR_OPT NAMES mlir-opt-${LLVM_VERSION_MAJOR} mlir-opt
            HINTS "${MLIR_INSTALL_PREFIX}/bin" "${LLVM_TOOLS_BINARY_DIR}")
        if(METREON_MLIR_OPT AND METREON_MLIR_TRANSLATE AND METREON_LLI)
            add_test(NAME metreon.gpu.control_flow.serial_semantics COMMAND ${CMAKE_COMMAND}
                -DEXPORTER=$<TARGET_FILE:metreon_gpu_lowering_tests>
                -DOPT=${METREON_MLIR_OPT} -DTRANSLATE=${METREON_MLIR_TRANSLATE} -DLLI=${METREON_LLI}
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/GPUControlFlow.mtr
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/gpu serial semantics"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/GPUSerialSemanticsTests.cmake)
        endif()
        if(METREON_MLIR_TRANSLATE AND METREON_LLI)
            add_test(NAME metreon.host.llvm.execution COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:metreonc>
                -DTRANSLATE=${METREON_MLIR_TRANSLATE} -DLLI=${METREON_LLI}
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/HostControlFlow.mtr
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/host execution"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/HostLLVMExecutionTests.cmake)
        endif()
        add_executable(metreon_mlir_import_tests tests/MLIRImportTests.cpp)
        target_link_libraries(metreon_mlir_import_tests PRIVATE metreon_mlir_import MLIRParser)
        metreon_enable_warnings(metreon_mlir_import_tests)
        add_test(NAME metreon.mlir.import COMMAND metreon_mlir_import_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/resources.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/accumulators.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/procedures.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/kernel_variables.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/optimizations.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/loops_and_conditionals.mtr
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/protocols_and_pipelines.mtr)
        set(metreon_mlir_cli_options -DMLIR_ENABLED=ON -DINSPECTOR=$<TARGET_FILE:metreon-opt>)
    endif()
    add_test(NAME metreon.mlir.cli COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:metreonc>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/procedures.mtr
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/mlir output"
        ${metreon_mlir_cli_options}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/MLIRCliTests.cmake)
    add_test(NAME metreon.aot.cli COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:metreonc>
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/aot
        ${metreon_mlir_cli_options}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/AOTCliTests.cmake)
    add_test(NAME metreon.host.llvm.cli COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:metreonc>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/HostControlFlow.mtr
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/host llvm"
        ${metreon_mlir_cli_options}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/HostLLVMCliTests.cmake)
    add_test(NAME metreon.gpu.control_flow.cli COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=$<TARGET_FILE:metreonc>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/Inputs/GPUControlFlow.mtr
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/gpu control flow"
        ${metreon_mlir_cli_options}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/GPUControlFlowCliTests.cmake)

    add_executable(metreon_accelerator_context_tests tests/AcceleratorContextTests.cpp)
    target_link_libraries(metreon_accelerator_context_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_accelerator_context_tests)
    add_test(NAME metreon.frontend.accelerator_contexts
        COMMAND metreon_accelerator_context_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/contexts.mtr)

    add_executable(metreon_optimization_tests tests/OptimizationTests.cpp)
    target_link_libraries(metreon_optimization_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_optimization_tests)
    add_test(NAME metreon.graphir.optimizations COMMAND metreon_optimization_tests)
    add_test(NAME metreon.graphir.optimizations.cli
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:metreonc>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/optimizations.mtr
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/optimizations
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/OptimizationCliTests.cmake)

    add_executable(metreon_composition_tests tests/CompositionTests.cpp)
    target_link_libraries(metreon_composition_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_composition_tests)
    add_test(NAME metreon.frontend.compositions
        COMMAND metreon_composition_tests ${CMAKE_CURRENT_SOURCE_DIR}/examples/protocols_and_pipelines.mtr)
    add_executable(metreon_control_flow_tests tests/ControlFlowTests.cpp)
    target_link_libraries(metreon_control_flow_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_control_flow_tests)
    add_test(NAME metreon.graphir.cfg COMMAND metreon_control_flow_tests)

    function(metreon_add_graphir_cli_test name input expected)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:metreonc>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/${input}.mtr
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/${name}
                "-DEXPECTED=${expected}"
                ${ARGN}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/GraphIRCliTests.cmake)
    endfunction()
    metreon_add_graphir_cli_test(metreon.frontend.pipeline.cli protocols_and_pipelines
        "graphir.pipeline.*TiledGemm")
    metreon_add_graphir_cli_test(metreon.frontend.control_flow.cli loops_and_conditionals
        "graphir.cfg.cond_br.*true.*false")
    metreon_add_graphir_cli_test(metreon.graphir.file.stdin procedures
        "graphir.cfg.procedure" -DSTDIN_INPUT=ON)
    metreon_add_graphir_cli_test(metreon.graphir.file.explicit procedures
        "graphir.cfg.procedure")
    add_test(NAME metreon.graphir.file.failures
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:metreonc>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/failures
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/GraphIRCliFailureTests.cmake)
    add_test(NAME metreon.graphir.file.working_directory
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:metreonc>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/procedures.mtr
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/caller directory"
            -DEXPECTED=graphir.cfg.procedure
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/GraphIRCliTests.cmake)

    add_executable(metreon_callable_tests tests/CallableTests.cpp)
    target_link_libraries(metreon_callable_tests PRIVATE metreon_frontend)
    metreon_enable_warnings(metreon_callable_tests)
    add_test(NAME metreon.frontend.callables
        COMMAND metreon_callable_tests ${CMAKE_CURRENT_SOURCE_DIR}/examples/procedures.mtr)
    metreon_add_graphir_cli_test(metreon.frontend.procedure.cli procedures
        "graphir.call.*CopySlot::complete.*callee_kind.*transition")

    add_executable(metreon_frontend_tests tests/FrontendTests.cpp)
    target_link_libraries(metreon_frontend_tests PRIVATE metreon_frontend)

    metreon_enable_warnings(metreon_frontend_tests)

    add_test(NAME metreon.frontend.unit COMMAND metreon_frontend_tests)
    metreon_add_graphir_cli_test(metreon.frontend.cli contexts
        "graphir.context_metadata.*context_key")
    metreon_add_graphir_cli_test(metreon.frontend.resource.cli resources
        "graphir.type_check.*required.*gpu_await.*result.*valid")
    metreon_add_graphir_cli_test(metreon.frontend.accumulator.cli accumulators
        "graphir.resource_state.*accumulator.*device")
    metreon_add_graphir_cli_test(metreon.frontend.kernel.cli kernel_variables
        "graphir.variable_decl.*initializer.kind.*infinity")
endif()
