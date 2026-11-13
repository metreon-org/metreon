file(READ "${SCHEMA_DIR}/MetreonHardwareProfiles.json" profiles)
file(READ "${SCHEMA_DIR}/GraphIRToSeq.json" lowering)

function(expect_field variable record field expected)
    string(JSON actual GET "${${variable}}" "${record}" "${field}")
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${record}.${field}: expected ${expected}, got ${actual}")
    endif()
endfunction()

expect_field(profiles Metreon_FpgaDataflowStage context_name "Fpga::DataflowStage")
expect_field(profiles Metreon_FpgaDataflowStage arity 1)
expect_field(profiles Metreon_FpgaDataflowStage runtime_materializable 0)
expect_field(profiles Metreon_TpuTensorCore context_name "Tpu::TensorCore")
expect_field(profiles Metreon_TpuTensorCore arity 0)
expect_field(profiles Metreon_TpuTensorCore seq_eligible 0)
expect_field(profiles Metreon_DmaCopy arity 2)
expect_field(lowering Metreon_GraphIRToSeqBoundary source_ir
    "post-optimization graphir::Module")
expect_field(lowering Metreon_GraphIRToSeqBoundary parses_mtr_source 0)
expect_field(lowering Metreon_GraphIRToSeqBoundary requires_metreon_mlir_import 0)
expect_field(lowering Metreon_GraphIRToSeqBoundary implemented 0)
expect_field(lowering Metreon_OutputFifoProfile capacity 16)
expect_field(lowering Metreon_OutputFifoProfile read_latency 1)
expect_field(lowering Metreon_OutputFifoProfile clock_domains 1)

function(reject_schema input expected)
    execute_process(
        COMMAND "${TABLEGEN}" -I "${DIALECT_DIR}" -dump-json
            "${INPUT_DIR}/${input}.td" -o "${SCHEMA_DIR}/${input}.json"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(result EQUAL 0 OR NOT stderr MATCHES "${expected}")
        message(FATAL_ERROR "invalid schema ${input}: ${result} ${stdout} ${stderr}")
    endif()
endfunction()

reject_schema(InvalidTpuToSeq "external accelerator context cannot be lowered to seq")
reject_schema(InvalidSeqCapacity "FIFO capacity must be positive")
