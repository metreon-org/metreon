#include "metreon/MLIR/HostLowering.h"
#include "ScalarLowering.h"
#include "CanonicalSyntax.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/LoweringOptions.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/TargetParser/Host.h"

#include <map>
#include <stdexcept>

using namespace mlir;

namespace metreon {
namespace {
using Attributes = std::map<std::string, std::string>;
using Syntax = parser::CanonicalSyntax;

std::string required(const Attributes &attrs, const std::string &name) {
  auto value = attrs.find(name);
  if (value == attrs.end()) throw std::runtime_error("missing GraphIR attribute `" + name + "`");
  return value->second;
}

class HostLowering {
  const graphir::Module &source;
  MLIRContext &context;
  OpBuilder builder;
  OwningOpRef<ModuleOp> output;
  SourceLocation current;
  static constexpr unsigned indexBits = sizeof(void *) * 8;

  [[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
  Location location(SourceLocation loc) {
    return FileLineColLoc::get(&context, source.sourceName(), loc.line, loc.column);
  }
  void hostContext(const graphir::ContextMetadataRef &metadata, bool loop) {
    if (!metadata) return;
    source.requireContextUse(metadata, graphir::ContextUse::MetadataAttachment);
    auto type = Syntax::type(metadata->name());
    const auto &parts = type.name.components;
    if (parts.empty() || parts.front() != "Host" ||
        (loop ? type.name.str() != "Host::Loop" : parts.back() == "Loop"))
      fail(loop ? "host LLVM lowering requires Host::Loop evidence" : "host LLVM lowering cannot consume a non-host execution context");
    if (loop) {
      if (metadata->genericArity()) fail("host loop context cannot have generic arguments");
      for (const auto &node : source.nodes())
        if (node.kind == graphir::NodeKind::Grant && node.context && node.context->hasSameIdentity(*metadata))
          fail("host loop contexts cannot grant execution authority");
    }
  }
  void preflight() {
    if (!source.kernelGraphs().empty() || !source.stageGraphs().empty() ||
        !source.pipelineGraphs().empty() || !source.protocolGraphs().empty())
      fail("host LLVM emission does not lower kernels, stages, protocols or pipelines");
    for (const auto &graph : source.procedureGraphs()) {
      current = graph.location();
      const auto domain = required(graph.attributes(), "execution_domain");
      if (domain != "Host" && domain != "context_independent")
        fail("host LLVM emission supports host procedures only");
      hostContext(graph.context(), false);
      for (const auto &param : graph.parameters())
        if (param.context) hostContext(param.context, false);
      graphir::walkCallableOperations(graph.body(), [&](const auto &op) {
        current = op.location;
        hostContext(op.context, false);
        hostContext(op.loopContext, true);
        switch (op.kind) {
        case graphir::CallableOperationKind::IteratorInit:
        case graphir::CallableOperationKind::IteratorHasNext:
        case graphir::CallableOperationKind::IteratorValue:
        case graphir::CallableOperationKind::IteratorAdvance:
          fail("container iteration requires an explicit host view extent/layout contract; integer range loops are supported");
        case graphir::CallableOperationKind::Call:
          if (required(op.attributes, "callee_kind") != "procedure")
            fail("host LLVM lowering requires a checked implementation for resource transitions and other non-procedure calls");
          break;
        default: break;
        }
        if (op.kind == graphir::CallableOperationKind::Until) {
          const auto stop = required(op.attributes, "stop_value");
          if (stop != "true" && stop != "false") fail("invalid GraphIR until stop value");
        }
      });
    }
  }

public:
  HostLowering(const graphir::Module &source, MLIRContext &context)
      : source(source), context(context), builder(&context) {
    context.getOrLoadDialect<LLVM::LLVMDialect>();
  }
  FailureOr<OwningOpRef<ModuleOp>> run() {
    try {
      preflight();
      SmallVector<const graphir::CallableGraph *> graphs;
      for (const auto &graph : source.procedureGraphs()) graphs.push_back(&graph);
      auto lowered = detail::lowerScalarCFG(source, graphs, context, indexBits);
      if (failed(lowered)) return failure();
      output = std::move(*lowered);
      (*output)->setAttr("llvm.target_triple", builder.getStringAttr(llvm::sys::getDefaultTargetTriple()));
      LowerToLLVMOptions options(&context);
      options.overrideIndexBitwidth(indexBits);
      LLVMTypeConverter converter(&context, options);
      RewritePatternSet patterns(&context);
      populateFuncToLLVMConversionPatterns(converter, patterns);
      arith::populateArithToLLVMConversionPatterns(converter, patterns);
      cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
      populateFinalizeMemRefToLLVMConversionPatterns(converter, patterns);
      LLVMConversionTarget target(context);
      target.addLegalOp<ModuleOp>();
      if (failed(applyFullConversion(*output, target, std::move(patterns)))) return failure();
      bool onlyLLVM = true;
      (**output).walk([&](Operation *op) {
        if (!isa<ModuleOp>(op) && op->getName().getDialectNamespace() != "llvm") {
          op->emitError("host lowering left an operation outside the LLVM dialect");
          onlyLLVM = false;
        }
      });
      if (!onlyLLVM || failed(verify(*output))) return failure();
      return std::move(output);
    } catch (const std::exception &error) {
      emitError(location(current)) << "GraphIR host LLVM lowering: " << error.what();
      return failure();
    }
  }
};
} // namespace

FailureOr<OwningOpRef<ModuleOp>> lowerHostToLLVM(const graphir::Module &source, MLIRContext &context) {
  // Repeat the existing semantic importer on this exact GraphIR object, including
  // nominal context checks, references, CFG dominance/scope and initialization.
  // A previous run or a serialized verification flag cannot authorize erasure.
  auto semantic = importGraphIR(source, context);
  if (failed(semantic)) return failure();
  return HostLowering(source, context).run();
}
} // namespace metreon
