#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/MLIR/Dialect.h"
#include "metreon/Parser/Parser.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
metreon::graphir::Module graph(const std::string &source) {
  metreon::parser::Parser parser(source);
  return metreon::graphir::lowerModule(parser.parseModule(), "device-test.mtr", true);
}
std::string print(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream output(text);
  module.print(output);
  return text;
}
mlir::OwningOpRef<mlir::ModuleOp> import(const metreon::graphir::Module &source, mlir::MLIRContext &context) {
  const auto before = source.print();
  auto imported = metreon::importDeviceExecution(source, context);
  check(mlir::succeeded(imported), "device import failed");
  check(source.print() == before, "device import mutated GraphIR");
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(print(**imported), &context);
  check(static_cast<bool>(parsed), "live Metreon module failed to round-trip");
  return parsed;
}
template <typename Action> void rejected(mlir::MLIRContext &context, Action action, const std::string &expected) {
  std::string message;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &diagnostic) {
    llvm::raw_string_ostream output(message);
    diagnostic.print(output);
    return mlir::success();
  });
  check(mlir::failed(action()), "invalid execution input was accepted");
  check(message.find(expected) != std::string::npos, "wrong device diagnostic: " + message);
}
void compile(const metreon::graphir::Module &source, mlir::MLIRContext &context) {
  auto live = import(source, context);
  auto manifest = metreon::deviceLaunchManifest(*live);
  check(manifest.getInteger("index_bits") == 64, "wrong index ABI");
  check(mlir::succeeded(metreon::lowerMetreonDeviceToGPU(*live)), "Metreon conversion failed");
  check(mlir::succeeded(mlir::verify(*live)), "invalid primary module");
  live->walk([](mlir::Operation *op) {
    check(op->getName().getDialectNamespace() != "metreon", "unconverted Metreon operation");
  });
  auto primary = mlir::parseSourceString<mlir::ModuleOp>(print(*live), &context);
  check(static_cast<bool>(primary), "primary output failed to round-trip");
  auto nvvm = metreon::lowerDeviceToNVVM(*primary);
  check(mlir::succeeded(nvvm), "NVVM conversion failed");
  (**nvvm).walk([](mlir::Operation *op) {
    const auto name = op->getName().getDialectNamespace();
    check(name == "builtin" || name == "llvm" || name == "nvvm", "unconverted operation in NVVM output");
  });
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(print(**nvvm), &context);
  check(static_cast<bool>(parsed), "NVVM output failed to round-trip");
  auto ptx = metreon::emitDevicePTX(*parsed, "sm_89");
  check(mlir::succeeded(ptx) && ptx->find(".target sm_89") != std::string::npos && ptx->find(".entry") != std::string::npos,
        "missing PTX architecture or kernel entry");
  rejected(context, [&] { return mlir::success(mlir::succeeded(metreon::emitDevicePTX(*parsed, "sm_9999"))); }, "unsupported");
}

const std::string prefix = "context Gpu::Thread grants {gpu_load, gpu_store} device;\n";

void failures(const std::string &source, mlir::MLIRContext &context) {
  auto original = graph(source);
  auto live = import(original, context);
  live->walk([&](metreon::ContextOp declaration) {
    declaration.setGrantsAttr(mlir::ArrayAttr::get(&context, {}));
  });
  rejected(context, [&] { return metreon::lowerMetreonDeviceToGPU(*live); }, "gpu_load");

  live = import(original, context);
  live->walk([&](metreon::ContextOp declaration) {
    mlir::Builder builder(&context);
    declaration.setNominalTypeAttr(builder.getDictionaryAttr({
        builder.getNamedAttr("name", builder.getStringAttr("Host::Main")),
        builder.getNamedAttr("arguments", builder.getArrayAttr({}))}));
  });
  rejected(context, [&] { return metreon::lowerMetreonDeviceToGPU(*live); }, "Gpu execution context");

  auto nominal = graph(prefix + "context Gpu::Thread grants {gpu_load, gpu_store} other; kernel work(@cx: device; a: gpu::span<f32>) { index i = gpu::global_id(@cx); f32 x = gpu::load(@cx; a, i); }");
  live = import(nominal, context);
  live->walk([&](metreon::DeviceIntrinsicOp intrinsic) {
    intrinsic.setContextAttr(mlir::FlatSymbolRefAttr::get(&context, "other"));
  });
  rejected(context, [&] { return metreon::lowerMetreonDeviceToGPU(*live); }, "same nominal execution context");

  auto calls = graph(prefix + "context Gpu::Thread grants {gpu_load, gpu_store} other; procedure helper(@cx: device; a: f32) -> f32 { return a; } kernel work(@cx: device) { f32 x = helper(@cx; 1.0); }");
  live = import(calls, context);
  auto procedure = *live->getOps<metreon::ProcedureOp>().begin();
  procedure.setContextAttr(mlir::SymbolRefAttr::get(&context, "__metreon_bodies",
      {mlir::FlatSymbolRefAttr::get(&context, "other")}));
  rejected(context, [&] { return metreon::lowerMetreonDeviceToGPU(*live); }, "different nominal execution context");

  live = import(original, context);
  auto contract = *live->getOps<metreon::KernelOp>().begin();
  contract.erase();
  rejected(context, [&] { return metreon::lowerMetreonDeviceToGPU(*live); }, "uncontracted function");

  auto foreign = graph(source);
  auto body = original.kernelGraphs()[0].body();
  for (auto &op : body) if (op.context) { op.context = foreign.contexts()[0]; break; }
  original.kernelGraphs()[0].setBody(body);
  rejected(context, [&] { return mlir::success(mlir::succeeded(metreon::importDeviceExecution(original, context))); }, "context");

  for (const auto &invalid : {
      "context Gpu::Thread grants {} device; kernel bad(@cx: device; a: gpu::span<f32>) { f32 x = gpu::load(@cx; a, 0); }",
      "procedure gpu::load() {}",
      "context Host::Main grants {} host; procedure bad(@cx: host) { index i = gpu::global_id(@cx); }"}) {
    bool failed = false;
    try { (void)graph(invalid); } catch (const metreon::DiagnosticError &) { failed = true; }
    check(failed, "source context violation accepted");
  }
  // The execution profile skips effect declarations, but keeps context grants.
  bool fullModeRejected = false;
  try {
    metreon::parser::Parser parser(source);
    (void)metreon::graphir::lowerModule(parser.parseModule(), "full-mode.mtr");
  } catch (const metreon::DiagnosticError &error) {
    fullModeRejected = error.diagnostic().code == "sema.undeclared_effect";
  }
  check(fullModeRejected, "context-only profile accidentally weakened the normal frontend");
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected the vector addition example");
    std::ifstream file(argv[1]);
    check(static_cast<bool>(file), "cannot read example");
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    mlir::MLIRContext context;
    compile(graph(source), context);
    compile(graph(prefix + "kernel __metreon_bodies(@cx: device; n: index) { index i = gpu::global_id(@cx); }"), context);
    failures(source, context);
    compile(graph(prefix + R"(
context Gpu::Loop repeat;
procedure add(@cx: device; a: f32, b: f32) -> f32 { return a + b; }
kernel loops(@cx: device; a: gpu::span<f32>, b: gpu::span<f32>, out: gpu::span<f32>, n: index) {
  index i = gpu::global_id(@cx);
  if (i < n) {
    f32 left = gpu::load(@cx; a, i);
    f32 right = gpu::load(@cx; b, i);
    f32 total = 0.0;
    for<@cx: repeat> (i32 j) in 0..2 { if (j == 0) { continue; } total = add(@cx; left, right); }
    i32 count = 0;
    while<@cx: repeat> (count < 1) { count = count + 1; }
    until<@cx: repeat> (count == 2 is true) { count = count + 1; }
    until<@cx: repeat> (count < 3 is false) { count = count + 1; }
    loop (@cx: repeat) { if (count == 3) { break; } count = count + 1; }
    gpu::store(@cx; out, i, total);
  }
}
)"), context);
    std::cout << "Device execution lowering and context failures passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
