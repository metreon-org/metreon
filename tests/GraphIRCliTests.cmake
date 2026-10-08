file(MAKE_DIRECTORY "${WORK_DIR}")
if(STDIN_INPUT)
    set(input_argument "-")
    set(output_name "stdin.graphir")
else()
    set(input_argument "${INPUT}")
    get_filename_component(input_name "${INPUT}" NAME_WLE)
    set(output_name "${input_name}.graphir")
endif()

set(emit_argument)
if(NOT DEFAULT_EMIT)
    set(emit_argument --emit=graphir)
endif()
execute_process(
    COMMAND "${COMPILER}" ${emit_argument} "${input_argument}"
    INPUT_FILE "${INPUT}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "compiler failed (${result}): ${stderr}")
endif()
if(NOT stdout STREQUAL "GraphIR written to tmp/graphir/${output_name}\n")
    string(SUBSTRING "${stdout}" 0 200 stdout_preview)
    message(FATAL_ERROR "expected only an output-path notification, got: ${stdout_preview}")
endif()
set(output_path "${WORK_DIR}/tmp/graphir/${output_name}")
if(NOT EXISTS "${output_path}")
    message(FATAL_ERROR "compiler did not create ${output_path}")
endif()
file(READ "${output_path}" graphir)
if(NOT graphir MATCHES "${EXPECTED}")
    message(FATAL_ERROR "generated GraphIR does not match ${EXPECTED}")
endif()
string(FIND "${graphir}" "\n  graphir.cfg {\n" cfg_position)
if(cfg_position EQUAL -1)
    message(FATAL_ERROR "generated file has no CFG section")
endif()
if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "successful compiler printed an error: ${stderr}")
endif()
