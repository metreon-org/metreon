#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/MLIR/Dialect.h"
#include "metreon/MLIR/HostExecution.h"
#include "metreon/Parser/Parser.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Diagnostics.h"
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
  return metreon::graphir::lowerModule(parser.parseModule(), "host-execution.mtr", true);
}
mlir::OwningOpRef<mlir::ModuleOp> import(const metreon::graphir::Module &source, mlir::MLIRContext &context) {
  const auto before = source.print();
  auto result = metreon::importHostExecution(source, context);
  check(mlir::succeeded(result), "host import failed");
  check(source.print() == before, "host import mutated GraphIR");
  std::string text;
  llvm::raw_string_ostream output(text);
  (**result).print(output);
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  check(static_cast<bool>(parsed), "live host module did not round-trip");
  return parsed;
}
template <typename Action> void rejects(mlir::MLIRContext &context, Action action, const std::string &expected) {
  std::string message;
  mlir::ScopedDiagnosticHandler diagnostics(&context, [&](mlir::Diagnostic &diagnostic) {
    llvm::raw_string_ostream output(message);
    diagnostic.print(output);
    return mlir::success();
  });
  check(mlir::failed(action()), "invalid context input was accepted");
  check(message.find(expected) != std::string::npos, "unexpected diagnostic: " + message);
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected vector-add source");
    std::ifstream file(argv[1]);
    check(static_cast<bool>(file), "cannot read source");
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto original = graph(source);
    mlir::MLIRContext context;
    auto live = import(original, context);
    auto lowered = metreon::lowerHostExecution(*live);
    check(mlir::succeeded(lowered) && !lowered->empty(), "host ABI lowering failed");
    live->walk([&](mlir::Operation *op) {
      check(mlir::isa<mlir::ModuleOp>(op) || op->getName().getDialectNamespace() == "llvm", "non-LLVM host operation remains");
    });
    live = import(original, context);
    live->walk([&](metreon::ContextOp declaration) {
      if (declaration.getSymName() == "host") declaration.setGrantsAttr(mlir::ArrayAttr::get(&context, {}));
    });
    rejects(context, [&] { return metreon::verifyHostContexts(*live); }, "grant");

    const auto extra = source + "\ncontext Host::Process grants {} other_host; context Gpu::Thread grants {gpu_load, gpu_store} other_gpu;";
    live = import(graph(extra), context);
    live->walk([&](metreon::HostIntrinsicOp intrinsic) {
      intrinsic.setContextAttr(mlir::FlatSymbolRefAttr::get(&context, "other_host"));
    });
    rejects(context, [&] { return metreon::verifyHostContexts(*live); }, "same nominal execution context");

    live = import(graph(extra), context);
    auto kernel = *live->getOps<metreon::KernelOp>().begin();
    kernel.setContextAttr(mlir::FlatSymbolRefAttr::get(&context, "other_gpu"));
    rejects(context, [&] { return metreon::verifyHostContexts(*live); }, "grant");

    auto foreign = graph(source);
    auto body = original.procedureGraphs()[0].body();
    for (auto &op : body) if (op.context) { op.context = foreign.procedureGraphs()[0].context(); break; }
    original.procedureGraphs()[0].setBody(body);
    rejects(context, [&] { return mlir::success(mlir::succeeded(metreon::importHostExecution(original, context))); }, "context");

    auto invalid = source;
    const auto at = invalid.find("gpu_launch<gpu_thread>");
    check(at != std::string::npos, "missing launch grant in fixture");
    invalid.replace(at, std::string("gpu_launch<gpu_thread>").size(), "gpu_launch<other_gpu>");
    invalid += "\ncontext Gpu::Thread grants {gpu_load, gpu_store} other_gpu;";
    bool failed = false;
    try { (void)graph(invalid); }
    catch (const metreon::DiagnosticError &error) { failed = error.diagnostic().code == "sema.missing_capability"; }
    check(failed, "host source launched a kernel without its exact nominal context grant");
    std::cout << "Host executable context checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
