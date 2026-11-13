file(MAKE_DIRECTORY "${WORK_DIR}")
function(check_native name source expected)
    file(WRITE "${WORK_DIR}/${name}.mtr" "${source}")
    foreach(mode optimized unoptimized)
        set(extra)
        if(mode STREQUAL unoptimized)
            set(extra --no-optimization)
        endif()
        execute_process(COMMAND "${COMPILER}" --emit=llvm ${extra} "${WORK_DIR}/${name}.mtr"
            WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_QUIET)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name}/${mode} compilation failed: ${error}")
        endif()
        set(ir "${WORK_DIR}/${name}.ll")
        execute_process(COMMAND "${TRANSLATE}" --mlir-to-llvmir "${WORK_DIR}/tmp/llvm/${name}.mlir" -o "${ir}"
            RESULT_VARIABLE result ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name}/${mode} LLVM IR translation failed: ${error}")
        endif()
        execute_process(COMMAND "${LLI}" "${ir}" RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 15)
        if(NOT result EQUAL expected)
            message(FATAL_ERROR "${name}/${mode} returned ${result}, expected ${expected}: ${error}")
        endif()
    endforeach()
endfunction()

file(READ "${INPUT}" example)
check_native(all_loops "${example}" 42)
check_native(zero_iterations [=[
context Host::Loop repeat_ctx;
procedure main() -> i32 {
  i32 x = 0;
  while<@cx: repeat_ctx> (false) { x = 1; }
  until<@cx: repeat_ctx> (true is true) { x = 2; }
  until<@cx: repeat_ctx> (false is false) { x = 3; }
  for<@cx: repeat_ctx> (i32 i) in 4..4 { x = 4; }
  for<@cx: repeat_ctx> (i32 i) in 5..2 { x = 5; }
  if (x == 0) { return 42; } else { return 1; }
}
]=] 42)
check_native(nested_loops [=[
context Host::Loop repeat_ctx;
procedure main() -> i32 {
  i32 sum;
  if (true) { sum = 0; } else { sum = 1; }
  for<@cx: repeat_ctx> (i32 i) in 0..3 {
    for<@cx: repeat_ctx> (i32 j) in 0..5 {
      if (j == 1) { continue; }
      if (j == 3) { break; }
      sum = sum + 1;
    }
  }
  if (sum == 6) { return 42; } else { return 0; }
}
]=] 42)
check_native(short_circuit [=[
procedure main() -> i32 {
  if (false && (1 / 0 == 0)) { return 1; }
  if (true || (1 / 0 == 0)) { return 42; }
  return 0;
}
]=] 42)
check_native(unsigned [=[
procedure main() -> i32 {
  u32 a = 4294967295;
  u32 b = 1;
  u32 wrapped = a + b;
  if (a > b && a / b > b && wrapped == 0) { return 42; }
  return 0;
}
]=] 42)
check_native(signed [=[
procedure main() -> i32 {
  i32 a = -7;
  i32 b = 2;
  if (a / b == -3 && a % b == -1 && a < b) { return 42; }
  return 0;
}
]=] 42)
check_native(floating [=[
procedure main() -> i32 {
  f64 a = 3.5;
  f64 b = 2.0;
  f64 zero = 0.0;
  f64 nan = zero / zero;
  if (a * b == 7.0 && nan != nan && !(nan == nan)) { return 42; }
  return 0;
}
]=] 42)
check_native(recursive_calls [=[
procedure factorial(n: i32) -> i32 {
  if (n <= 1) { return 1; }
  return n * factorial(n - 1);
}
procedure main() -> i32 {
  if (factorial(5) == 120) { return 42; }
  return 0;
}
]=] 42)
check_native(index_loop [=[
context Host::Loop repeat_ctx;
procedure advance(n: index) -> index {
  while<@cx: repeat_ctx> (n < 4) { n = n + 1; }
  return n;
}
procedure main() -> i32 {
  index n = 0;
  if (advance(n) == 4) { return 42; }
  return 0;
}
]=] 42)
check_native(bounds_once [=[
context Host::Loop repeat_ctx;
procedure main() -> i32 {
  i32 end = 3;
  i32 sum = 0;
  for<@cx: repeat_ctx> (i32 i) in 0..end { end = 0; sum = sum + 1; }
  if (sum == 3) { return 42; }
  return 0;
}
]=] 42)
