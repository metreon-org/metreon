#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/MLIR/GPULowering.h"
#include "metreon/Parser/Parser.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
metreon::graphir::Module graph(const std::string &text) {
  metreon::parser::Parser parser(text);
  return metreon::graphir::lowerModule(parser.parseModule(), "gpu-test.mtr");
}
std::string print(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  module.print(stream, mlir::OpPrintingFlags().enableDebugInfo());
  return text;
}
mlir::OwningOpRef<mlir::ModuleOp> lower(const metreon::graphir::Module &source, mlir::MLIRContext &context) {
  const auto before = source.print();
  auto output = metreon::lowerGPUControlFlow(source, context);
  check(mlir::succeeded(output), "GPU lowering failed");
  check(mlir::succeeded(mlir::verify(**output)), "GPU output is invalid");
  check(source.print() == before, "GPU lowering mutated GraphIR");
  auto roundtrip = mlir::parseSourceString<mlir::ModuleOp>(print(**output), &context);
  check(static_cast<bool>(roundtrip), "GPU output failed to round-trip");
  (**output).walk([&](mlir::Operation *op) {
    const auto dialect = op->getName().getDialectNamespace();
    check(dialect != "cf" && dialect != "memref" && dialect != "llvm" && dialect != "nvvm",
        "unexpected residual or out-of-scope execution dialect");
  });
  return std::move(*output);
}
void rejects(metreon::graphir::Module &source, mlir::MLIRContext &context, const std::string &expected) {
  std::string message;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &diagnostic) {
    llvm::raw_string_ostream out(message);
    diagnostic.print(out);
    return mlir::success();
  });
  check(mlir::failed(metreon::lowerGPUControlFlow(source, context)), "unsafe/unsupported graph accepted");
  check(message.find(expected) != std::string::npos, "wrong GPU rejection: " + message);
}
const std::string prefix = "context Gpu::Thread grants {} device; context Gpu::Loop repeat;\n";

void failures(mlir::MLIRContext &context) {
  auto uninitialized = graph(prefix + "procedure main(@cx: device) -> i32 { i32 x = 1; return x; }");
  auto body = uninitialized.procedureGraphs()[0].body();
  body[0].attributes["initialized"] = "false";
  body[0].attributes.erase("initializer.kind"); body[0].attributes.erase("initializer.value");
  uninitialized.procedureGraphs()[0].setBody(body);
  rejects(uninitialized, context, "uninitialized");

  const auto calls = prefix + R"(
context Gpu::Thread grants {} other;
procedure target(@cx: device) -> void { return; }
procedure main(@cx: device) -> void { target(@cx); return; }
)";
  auto nominal = graph(calls);
  body = nominal.procedureGraphs()[1].body();
  body[0].context = nominal.contexts()[2];
  nominal.procedureGraphs()[1].setBody(body);
  rejects(nominal, context, "different nominal execution context");
  auto foreign = graph(calls);
  body = foreign.procedureGraphs()[1].body(); body[0].context = nominal.contexts()[0];
  foreign.procedureGraphs()[1].setBody(body);
  rejects(foreign, context, "context");

  auto wrongLoop = graph(prefix + "context Host::Loop host_loop; kernel work(@cx: device) { for<@cx: repeat> (i32 i) in 0..4 {} }");
  body = wrongLoop.kernelGraphs()[0].body(); body[0].loopContext = wrongLoop.contexts()[2];
  wrongLoop.kernelGraphs()[0].setBody(body);
  rejects(wrongLoop, context, "Gpu::Loop");

  auto view = graph(prefix + "kernel work(@cx: device; values: view<i32, gpu::global>) { return; }");
  rejects(view, context, "extent, layout and access contract");
  auto owned = graph(prefix + "procedure work(@cx: device; value: own buffer<gpu::global>) -> own buffer<gpu::global> { return value; }");
  rejects(owned, context, "ownership/resource obligations");
  auto overflow = graph(prefix + "procedure work(@cx: device) -> i8 { i8 x = 128; return x; }");
  rejects(overflow, context, "outside its source type range");
  auto wrongType = graph(prefix + "procedure work(@cx: device) -> u32 { u32 a = 1; u32 b = 2; return a + b; }");
  body = wrongType.procedureGraphs()[0].body(); body[1].attributes["type"] = "i32";
  wrongType.procedureGraphs()[0].setBody(body);
  rejects(wrongType, context, "binary source types disagree");
  auto wrongRead = graph(prefix + "procedure work(@cx: device) -> u32 { u32 a = 1; a = a + 1; return a; }");
  body = wrongRead.procedureGraphs()[0].body();
  for (auto &op : body) if (op.kind == metreon::graphir::CallableOperationKind::Load) {
    op.attributes["type"] = "i32"; break;
  }
  wrongRead.procedureGraphs()[0].setBody(body);
  rejects(wrongRead, context, "local read source types disagree");
  auto wrongResult = graph(prefix + "procedure work(@cx: device) -> u32 { u32 a = 1; return a + a; }");
  body = wrongResult.procedureGraphs()[0].body();
  for (auto &op : body) if (op.kind == metreon::graphir::CallableOperationKind::Binary) op.attributes["type"] = "i32";
  wrongResult.procedureGraphs()[0].setBody(body);
  rejects(wrongResult, context, "binary result source type disagrees");

  auto helper = graph(prefix + "context Host::Loop host_loop; procedure helper() -> void { for<@cx: host_loop> (i32 i) in 0..2 {} } kernel work(@cx: device) { helper(); }");
  rejects(helper, context, "Gpu::Loop");
  auto host = graph("procedure main() -> i32 { return 0; }");
  rejects(host, context, "requires a GPU kernel");
  for (const auto &text : {
      "kernel forever(@cx: device) { loop (@cx: repeat) {} }",
      "kernel forever(@cx: device; again: bool) { while<@cx: repeat> (again) {} }",
      "kernel forever(@cx: device; again: bool) { if (again) { loop (@cx: repeat) {} } return; }"}) {
    for (bool optimize : {false, true}) {
      auto infinite = graph(prefix + text);
      if (optimize) metreon::graphir::passes::runDefaultPipeline(infinite);
      check(print(*lower(infinite, context)).find("scf.while") != std::string::npos,
          "potentially nonterminating loop lost its structured backedge");
    }
  }
}

// Export only scalar func.func bodies for CPU semantic regression checks. This
// test adapter does not implement a compiler output mode or GPU execution.
void exportSerial(mlir::ModuleOp source, const std::filesystem::path &path) {
  auto output = mlir::OwningOpRef<mlir::ModuleOp>(mlir::ModuleOp::create(source.getLoc()));
  for (auto module : source.getOps<mlir::gpu::GPUModuleOp>())
    for (auto function : module.getOps<mlir::func::FuncOp>()) output->getBody()->push_back(function->clone());
  check(mlir::succeeded(mlir::verify(*output)), "invalid serial GPU control-flow test adapter");
  std::ofstream file(path);
  file << print(*output);
  check(static_cast<bool>(file), "cannot write serial GPU test module");
}
void semantics(mlir::MLIRContext &context, const std::string &fixture, const std::filesystem::path &directory) {
  struct Case { const char *name; std::string source; int expected; };
  const Case cases[] = {
    {"all_loops", fixture + "procedure main(@cx: gpu_thread) -> i32 { return calculate(@cx; 5); }", 42},
    {"ranges", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 n = 0; i32 bound = 4;
  for<@cx: repeat> (i32 i) in -3..bound { n = n + 1; bound = 0; }
  for<@cx: repeat> (i32 i) in 4..4 { n = 99; }
  for<@cx: repeat> (i32 i) in 9..2 { n = 99; }
  if (n == 7) { return 42; } else { return 1; }
})", 42},
    {"nested_exits", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 n = 0;
  for<@cx: repeat> (i32 i) in 0..4 {
    if (i == 1) { continue; }
    for<@cx: repeat> (i32 j) in 0..5 {
      if (j == 3) { break; }
      n = n + 1;
    }
  }
  if (n == 9) { return 42; } else { return 1; }
})", 42},
    {"early_return", prefix + R"(
procedure find(@cx: device; limit: i32) -> i32 {
  for<@cx: repeat> (i32 i) in 0..limit { if (i == 3) { return 42; } }
  return 1;
}
procedure main(@cx: device) -> i32 { return find(@cx; 9); }
)", 42},
    {"short_circuit", prefix + R"(
procedure test(@cx: device; n: i32) -> i32 {
  if (n != 0 && 10 / n > 1) { return 1; }
  if (n == 0 || 10 / n > 1) { return 42; }
  return 2;
}
procedure main(@cx: device) -> i32 { return test(@cx; 0); }
)", 42},
    {"integer_edges", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 n = 0;
  for<@cx: repeat> (i8 i) in 125..127 { n = n + 1; }
  for<@cx: repeat> (i32 i) in 2147483645..2147483647 { n = n + 1; }
  for<@cx: repeat> (u8 i) in 253..255 { n = n + 1; }
  for<@cx: repeat> (i64 i) in 9223372036854775805..9223372036854775807 { n = n + 1; }
  for<@cx: repeat> (u64 i) in 18446744073709551613..18446744073709551615 { n = n + 1; }
  if (n == 10) { return 42; } else { return 1; }
})", 42},
    {"affine_nested", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 n = 0;
  for<@cx: repeat> (index i) in 0..5 {
    for<@cx: repeat> (index j) in 0..i {
      if (j < 2) { n = n + 1; }
    }
  }
  if (n == 7) { return 42; } else { return 1; }
})", 42},
    {"signed_index_compare", prefix + R"(
procedure observe(n: i32) -> i32 { return n; }
procedure compare(@cx: device; a: index, b: index) -> i32 {
  if (a < b) { return observe(42); } else { observe(9); return 1; }
}
procedure main(@cx: device) -> i32 {
  index a = -9223372036854775808; index b = 9223372036854775807;
  return compare(@cx; a, b);
})", 42},
    {"pretest_until", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 n = 0;
  until<@cx: repeat> (n == 0 is true) { n = 99; }
  until<@cx: repeat> (n != 0 is false) { n = 99; }
  if (n == 0) { return 42; } else { return 1; }
})", 42},
    {"live_bound", prefix + R"(
procedure main(@cx: device) -> i32 {
  i32 bound = 5; i32 i = 0;
  while<@cx: repeat> (i < bound) { i = i + 1; bound = bound - 1; }
  if (i == 3 && bound == 2) { return 42; } else { return 1; }
})", 42},
    {"recursion", prefix + R"(
procedure factorial(@cx: device; n: i32) -> i32 {
  if (n < 2) { return 1; } else { return n * factorial(@cx; n - 1); }
}
procedure main(@cx: device) -> i32 {
  if (factorial(@cx; 5) == 120) { return 42; } else { return 1; }
})", 42}
  };
  if (!directory.empty()) std::filesystem::create_directories(directory);
  for (const auto &test : cases) for (bool optimize : {false, true}) {
    auto source = graph(test.source);
    if (optimize) metreon::graphir::passes::runDefaultPipeline(source);
    auto output = lower(source, context);
    if (!directory.empty()) {
      const auto stem = std::string(test.name) + (optimize ? "_optimized" : "_unoptimized");
      exportSerial(*output, directory / (stem + ".mlir"));
      std::ofstream(directory / (stem + ".expected")) << test.expected;
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2 || argc == 3, "expected fixture and optional serial output directory");
    std::ifstream file(argv[1]);
    check(static_cast<bool>(file), "cannot open GPU fixture");
    std::string fixture(std::istreambuf_iterator<char>(file), {});
    mlir::MLIRContext context;
    for (bool optimize : {false, true}) {
      auto source = graph(fixture);
      if (optimize) metreon::graphir::passes::runDefaultPipeline(source);
      auto output = lower(source, context);
      auto text = print(*output);
      check(text.find("affine.for") != std::string::npos && text.find("affine.if") != std::string::npos &&
            text.find("scf.for") != std::string::npos && text.find("scf.while") != std::string::npos &&
            text.find("scf.if") != std::string::npos, "GPU dialect selection is incomplete");
      auto modules = output->getOps<mlir::gpu::GPUModuleOp>();
      auto module = *modules.begin();
      auto kernel = mlir::SymbolTable::lookupSymbolIn(module, "work");
      check(kernel && mlir::cast<mlir::gpu::GPUFuncOp>(kernel).getNumArguments() == 1,
          "GPU runtime signature contains context evidence");
      check(!mlir::SymbolTable::lookupSymbolIn(module, "host_only"), "host body moved to GPU");
      auto *host = mlir::SymbolTable::lookupSymbolIn(*output, "host_only");
      check(host && host->hasAttr("deferred_body") && !host->hasAttr("metreon.device_body"),
          "host semantic contract was lost");
    }
    failures(context);
    semantics(context, fixture, argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    std::cout << "GPU GraphIR lowering checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
