#include "metreon/MLIR/HostLowering.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
metreon::graphir::Module graph(const std::string &text) {
  return metreon::graphir::lowerModule(metreon::parser::Parser(text).parseModule(), "host.mtr");
}
std::string inspect(metreon::graphir::Module &source, mlir::MLIRContext &context) {
  const auto before = source.print();
  auto result = metreon::lowerHostToLLVM(source, context);
  check(mlir::succeeded(result), "host LLVM conversion failed");
  check(mlir::succeeded(mlir::verify(**result)), "invalid LLVM module");
  (**result).walk([&](mlir::Operation *op) {
    check(mlir::isa<mlir::ModuleOp>(op) || op->getName().getDialectNamespace() == "llvm",
        "host output contains a non-LLVM operation");
  });
  std::string text;
  llvm::raw_string_ostream output(text);
  (**result).print(output, mlir::OpPrintingFlags().enableDebugInfo());
  auto roundtrip = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  check(static_cast<bool>(roundtrip), "LLVM MLIR failed to round-trip");
  check(source.print() == before, "host lowering mutated GraphIR");
  return text;
}
void rejects(metreon::graphir::Module &source, mlir::MLIRContext &context, const std::string &expected) {
  std::string message;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &diagnostic) {
    llvm::raw_string_ostream out(message);
    diagnostic.print(out);
    return mlir::success();
  });
  check(mlir::failed(metreon::lowerHostToLLVM(source, context)), "unsafe/unsupported GraphIR was accepted");
  check(message.find(expected) != std::string::npos, "wrong failure: " + message);
}

void checkFailures(mlir::MLIRContext &context) {
  auto uninitialized = graph("procedure main() -> i32 { i32 x = 1; return x; }");
  auto body = uninitialized.procedureGraphs()[0].body();
  body[0].attributes["initialized"] = "false";
  body[0].attributes.erase("initializer.kind");
  body[0].attributes.erase("initializer.value");
  uninitialized.procedureGraphs()[0].setBody(body);
  rejects(uninitialized, context, "uninitialized");

  const std::string contexts = R"(
context Host::Process grants {} first;
context Host::Process grants {} second;
procedure callee(@cx: first) -> void { return; }
procedure caller(@cx: first) -> void { callee(@cx); return; }
)";
  auto invalid = graph(contexts);
  body = invalid.procedureGraphs()[1].body();
  body[0].context = invalid.contexts()[1];
  invalid.procedureGraphs()[1].setBody(body);
  rejects(invalid, context, "different nominal execution context");

  auto foreign = graph(contexts);
  body = foreign.procedureGraphs()[1].body();
  body[0].context = invalid.contexts()[0];
  foreign.procedureGraphs()[1].setBody(body);
  rejects(foreign, context, "context");

  auto device = graph("context Gpu::Thread grants {} gpu; procedure device(@cx: gpu) -> i32 { return 1; }");
  rejects(device, context, "host procedures only");
  auto kernel = graph("kernel device() { return; }");
  rejects(kernel, context, "does not lower kernels");
  auto view = graph("procedure main(values: view<i32, host::memory>) -> void { return; }");
  rejects(view, context, "extent, layout and access contract");
  auto owned = graph("procedure main(value: own buffer<host::memory>) -> own buffer<host::memory> { return value; }");
  rejects(owned, context, "ownership/resource obligations");

  auto overflow = graph("procedure main() -> i8 { i8 value = 128; return value; }");
  rejects(overflow, context, "outside its source type range");
  auto underflow = graph("procedure main() -> i8 { i8 value = -129; return value; }");
  rejects(underflow, context, "outside its source type range");
  auto unsignedOverflow = graph("procedure main() -> u8 { u8 value = 256; return value; }");
  rejects(unsignedOverflow, context, "outside its source type range");

  auto binaryMismatch = graph("procedure main() -> u32 { u32 a = 1; u32 b = 2; return a + b; }");
  body = binaryMismatch.procedureGraphs()[0].body();
  body[1].attributes["type"] = "i32";
  binaryMismatch.procedureGraphs()[0].setBody(body);
  rejects(binaryMismatch, context, "binary source types disagree");

  auto returnMismatch = graph("procedure main() -> i32 { i32 a = 1; return a; }");
  body = returnMismatch.procedureGraphs()[0].body();
  body[0].attributes["type"] = "u32";
  returnMismatch.procedureGraphs()[0].setBody(body);
  rejects(returnMismatch, context, "return source types disagree");
}

void checkArithmetic(mlir::MLIRContext &context) {
  auto signedOps = graph("procedure calc(a: i32, b: i32) -> bool { i32 v = a / b; return v < a; }");
  auto signedText = inspect(signedOps, context);
  check(signedText.find("llvm.sdiv") != std::string::npos && signedText.find("slt") != std::string::npos,
      "signed source semantics were lost");
  auto unsignedOps = graph("procedure calc(a: u32, b: u32) -> bool { u32 v = a / b; return v < a; }");
  auto unsignedText = inspect(unsignedOps, context);
  check(unsignedText.find("llvm.udiv") != std::string::npos && unsignedText.find("ult") != std::string::npos,
      "unsigned source semantics were lost");
  auto floatOps = graph("procedure calc(a: f64, b: f64) -> bool { f64 v = -a + b; return v != a; }");
  auto floatText = inspect(floatOps, context);
  check(floatText.find("llvm.fneg") != std::string::npos && floatText.find("llvm.fadd") != std::string::npos &&
        floatText.find("une") != std::string::npos, "floating-point source semantics were lost");
  auto limits = graph("procedure low() -> i64 { i64 v = -9223372036854775808; return v; } procedure high() -> u64 { u64 v = 18446744073709551615; return v; }");
  inspect(limits, context);
  auto zero = graph("procedure zero() -> f64 { f64 v = -0.0; return v; }");
  check(inspect(zero, context).find("-0.000000e+00") != std::string::npos, "negative floating zero was lost");
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected tracked host fixture");
    std::ifstream input(argv[1]);
    check(static_cast<bool>(input), "cannot read host fixture");
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    mlir::MLIRContext context;
    for (bool optimize : {false, true}) {
      auto module = graph(source);
      if (optimize) metreon::graphir::passes::runDefaultPipeline(module);
      const auto text = inspect(module, context);
      check(text.find("llvm.cond_br") != std::string::npos && text.find("llvm.call") != std::string::npos,
          "host CFG or calls were not lowered");
      check(text.find("llvm.func @main()") != std::string::npos, "context evidence leaked into the runtime ABI");
    }
    checkFailures(context);
    checkArithmetic(context);
    std::cout << "GraphIR host LLVM lowering checks passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
