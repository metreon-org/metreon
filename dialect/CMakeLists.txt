# Keep TableGen inputs and generated definitions local to the dialect build.
include_directories(${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})

set(LLVM_TARGET_DEFINITIONS MetreonDialect.td)
mlir_tablegen(MetreonDialect.h.inc -gen-dialect-decls -dialect=metreon)
mlir_tablegen(MetreonDialect.cpp.inc -gen-dialect-defs -dialect=metreon)

set(LLVM_TARGET_DEFINITIONS MetreonOps.td)
set(metreon_op_dependencies
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ContextOps.td"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ResourceOps.td"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/StructureOps.td"
)
mlir_tablegen(MetreonOps.h.inc -gen-op-decls DEPENDS ${metreon_op_dependencies})
mlir_tablegen(MetreonOps.cpp.inc -gen-op-defs DEPENDS ${metreon_op_dependencies})

set(LLVM_TARGET_DEFINITIONS MetreonTypes.td)
mlir_tablegen(MetreonTypes.h.inc -gen-typedef-decls -typedefs-dialect=metreon)
mlir_tablegen(MetreonTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=metreon)

add_public_tablegen_target(MetreonIncGen)
add_custom_target(metreon_mlir ALL)
add_dependencies(metreon_mlir MetreonIncGen)

add_library(metreon_dialect ../lib/MLIR/Dialect.cpp)
add_dependencies(metreon_dialect MetreonIncGen)
target_include_directories(metreon_dialect PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(metreon_dialect SYSTEM PUBLIC
    ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(metreon_dialect PUBLIC MLIRIR MLIRSideEffectInterfaces)
add_dependencies(metreon_mlir metreon_dialect)

# Standalone architecture manifests; no CIRCT library or conversion is linked.
find_program(LLVM_TABLEGEN_EXE NAMES llvm-tblgen
    HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
set(LLVM_TARGET_DEFINITIONS MetreonHardwareProfiles.td)
tablegen(LLVM MetreonHardwareProfiles.json -dump-json)
set(LLVM_TARGET_DEFINITIONS Conversion/GraphIRToSeq.td)
tablegen(LLVM GraphIRToSeq.json -dump-json)
add_public_tablegen_target(MetreonHardwareSchemaGen)
add_dependencies(metreon_mlir MetreonHardwareSchemaGen)

if(BUILD_TESTING)
    add_test(NAME metreon.dialect.hardware_schemas
        COMMAND ${CMAKE_COMMAND}
            -DSCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTABLEGEN=${LLVM_TABLEGEN_EXE}
            -DDIALECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DINPUT_DIR=${PROJECT_SOURCE_DIR}/tests/Inputs
            -P ${PROJECT_SOURCE_DIR}/tests/HardwareSchemaTests.cmake)
endif()
