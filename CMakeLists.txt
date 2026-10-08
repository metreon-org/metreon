cmake_minimum_required(VERSION 3.20)

project(MetreonCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    lib/GraphIR/Lowering.cpp
    lib/Lexer/Lexer.cpp
    lib/Parser/Parser.cpp
    lib/Sema/ContextValidator.cpp
    lib/Sema/CallableValidator.cpp
)

target_include_directories(metreon_frontend
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

metreon_enable_warnings(metreon_frontend)

add_executable(metreonc tools/metreonc.cpp)
target_link_libraries(metreonc PRIVATE metreon_frontend)
metreon_enable_warnings(metreonc)

include(CTest)

if(BUILD_TESTING)
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
    metreon_add_graphir_cli_test(metreon.graphir.file.stdin procedures
        "graphir.cfg.procedure" -DSTDIN_INPUT=ON)
    metreon_add_graphir_cli_test(metreon.graphir.file.default procedures
        "graphir.cfg.procedure" -DDEFAULT_EMIT=ON)
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
