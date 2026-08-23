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
    lib/GraphIR/Graph.cpp
    lib/GraphIR/Lowering.cpp
    lib/Lexer/Lexer.cpp
    lib/Parser/Parser.cpp
    lib/Sema/ContextValidator.cpp
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
    add_executable(metreon_frontend_tests tests/FrontendTests.cpp)
    target_link_libraries(metreon_frontend_tests PRIVATE metreon_frontend)

    metreon_enable_warnings(metreon_frontend_tests)

    add_test(NAME metreon.frontend.unit COMMAND metreon_frontend_tests)
    add_test(
        NAME metreon.frontend.cli
        COMMAND metreonc --emit=graphir ${CMAKE_CURRENT_SOURCE_DIR}/examples/contexts.mtr
    )
    set_tests_properties(
        metreon.frontend.cli
        PROPERTIES PASS_REGULAR_EXPRESSION "graphir.context_var.*I"
    )
endif()
