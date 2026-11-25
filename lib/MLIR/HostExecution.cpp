#include "metreon/MLIR/HostExecution.h"
#include "metreon/MLIR/Dialect.h"
#include "CanonicalSyntax.h"
#include "ScalarLowering.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Transforms/DialectConversion.h"

#include <map>
#include <stdexcept>

using namespace mlir;
namespace metreon {
namespace {
using Syntax = parser::CanonicalSyntax;
ContextOp contextFor(Operation *op, SymbolRefAttr ref) {
  return ref ? dyn_cast_or_null<ContextOp>(SymbolTable::lookupNearestSymbolFrom(op, ref)) : ContextOp{};
}
StringRef nominal(ContextOp context) { return context.getNominalType().getAs<StringAttr>("name").getValue(); }
bool grant(ContextOp context, StringRef name, ContextOp target = {}) {
  for (auto item : context.getGrants()) {
    auto application = cast<DictionaryAttr>(item);
    if (application.getAs<StringAttr>("name").getValue() != name) continue;
    auto args = application.getAs<ArrayAttr>("arguments");
    if (!target && args.empty()) return true;
    if (target && args.size() == 1) {
      const auto identifier = cast<DictionaryAttr>(args[0]).getAs<StringAttr>("name").getValue();
      if (contextFor(context, FlatSymbolRefAttr::get(context.getContext(), identifier)) == target) return true;
    }
  }
  return false;
}

struct HostConversion : OpConversionPattern<HostIntrinsicOp> {
  ModuleOp module;
  std::vector<HostABI> &abi;
  HostConversion(MLIRContext *context, ModuleOp module, std::vector<HostABI> &abi)
      : OpConversionPattern(context), module(module), abi(abi) {}
  LogicalResult matchAndRewrite(HostIntrinsicOp op, OpAdaptor adaptor, ConversionPatternRewriter &rewriter) const override {
    auto args = adaptor.getInputs();
    const auto kind = op.getKind();
    if (kind == "host::alloc") rewriter.replaceOpWithNewOp<memref::AllocOp>(op, cast<MemRefType>(op.getResult(0).getType()), args);
    else if (kind == "host::free") {
      rewriter.create<memref::DeallocOp>(op.getLoc(), args[0]); rewriter.eraseOp(op);
    } else if (kind == "host::load") rewriter.replaceOpWithNewOp<memref::LoadOp>(op, args[0], ValueRange{args[1]});
    else if (kind == "host::store") {
      rewriter.create<memref::StoreOp>(op.getLoc(), args[2], args[0], ValueRange{args[1]}); rewriter.eraseOp(op);
    } else {
      auto container = module;
      const auto kernel = op.getCalleeAttr() ? op.getCalleeAttr().getRootReference().str() : std::string{};
      std::string symbol;
      for (const auto &item : abi) if (item.kind == kind && item.kernel == kernel) symbol = item.symbol;
      if (symbol.empty()) {
        symbol = "__metreon_abi_" + std::to_string(abi.size());
        while (SymbolTable::lookupSymbolIn(container, symbol)) symbol += '_';
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToEnd(container.getBody());
        auto declaration = rewriter.create<func::FuncOp>(op.getLoc(), symbol,
            rewriter.getFunctionType(args.getTypes(), op.getResultTypes()));
        declaration.setPrivate();
        abi.push_back({symbol, kind.str(), kernel});
      }
      rewriter.replaceOpWithNewOp<func::CallOp>(op, symbol, op.getResultTypes(), args);
    }
    return success();
  }
};
} // namespace

FailureOr<OwningOpRef<ModuleOp>> importHostExecution(const graphir::Module &source, MLIRContext &context) {
  try {
    if (!source.resourceGraphs().empty() || !source.protocolGraphs().empty() || !source.pipelineGraphs().empty())
      throw std::runtime_error("executable profile does not implement resource/protocol/pipeline execution");
    SmallVector<const graphir::CallableGraph *> graphs;
    for (const auto &graph : source.procedureGraphs()) {
      const auto domain = graph.attributes().at("execution_domain");
      if (domain == "Gpu") continue;
      if (domain != "Host" && domain != "context_independent") throw std::runtime_error("executable host body has a non-host domain");
      graphs.push_back(&graph);
      auto check = [&](const graphir::ContextMetadataRef &metadata, bool loop) {
        if (!metadata) return;
        source.requireContextUse(metadata, graphir::ContextUse::MetadataAttachment);
        const auto name = Syntax::type(metadata->name()).name.str();
        if (loop ? name != "Host::Loop" : !name.starts_with("Host::") || name == "Host::Loop")
          throw std::runtime_error("host execution requires Host execution/loop contexts");
        if (!loop && (!graph.context() || !graph.context()->hasSameIdentity(*metadata)))
          throw std::runtime_error("host operation changes nominal execution context");
      };
      check(graph.context(), false);
      for (const auto &parameter : graph.parameters()) check(parameter.context, false);
      graphir::walkCallableOperations(graph.body(), [&](const auto &op) {
        using Kind = graphir::CallableOperationKind;
        if ((op.kind == Kind::For || op.kind == Kind::While || op.kind == Kind::Until || op.kind == Kind::Loop) && !op.loopContext)
          throw std::runtime_error("host loop has no Host::Loop evidence");
        check(op.context, false); check(op.loopContext, true);
        if (op.kind == graphir::CallableOperationKind::Call && op.attributes.at("callee_kind") != "procedure" &&
            op.attributes.at("callee_kind") != "host_builtin")
          throw std::runtime_error("host execution cannot consume device/resource intrinsics");
      });
    }
    auto result = detail::lowerScalarCFG(source, graphs, context, 64, false, true);
    if (failed(result) || failed(verifyHostContexts(**result))) return failure();
    return std::move(*result);
  } catch (const std::exception &error) {
    emitError(FileLineColLoc::get(&context, source.sourceName(), 1, 1)) << "GraphIR host executable import: " << error.what();
    return failure();
  }
}

LogicalResult verifyHostContexts(ModuleOp module) {
  if (failed(verify(module))) return failure();
  bool valid = true;
  auto reject = [&](Operation *op, StringRef message) { op->emitError(message); valid = false; };
  module.walk([&](func::FuncOp function) {
    auto ambient = contextFor(function, function->getAttrOfType<SymbolRefAttr>("metreon.context"));
    if (function->hasAttr("metreon.context") && (!ambient || !nominal(ambient).starts_with("Host::") || nominal(ambient) == "Host::Loop"))
      reject(function, "host body requires a Host execution context");
    if (auto loops = function->getAttrOfType<ArrayAttr>("metreon.loop_contexts")) for (auto item : loops) {
      auto loop = contextFor(function, dyn_cast<SymbolRefAttr>(item));
      if (!loop || nominal(loop) != "Host::Loop" || !loop.getGrants().empty() || !loop.getGenericParameters().empty())
        reject(function, "host loops require grant-free Host::Loop evidence");
    }
    function.walk([&](Operation *op) {
      if (auto intrinsic = dyn_cast<HostIntrinsicOp>(op)) {
        if (!ambient || contextFor(op, intrinsic.getContextAttr()) != ambient) {
          reject(op, "host intrinsic requires the same nominal execution context as its body"); return;
        }
        const auto kind = intrinsic.getKind();
        StringRef required = kind.starts_with("host::") ? "host_memory" : "";
        ContextOp target;
        if (kind == "host::print_f32") required = "host_io";
        if (kind == "gpu::alloc") required = "gpu_alloc";
        if (kind == "gpu::free") required = "gpu_free";
        if (kind == "gpu::upload" || kind == "gpu::download") required = "gpu_copy";
        if (kind == "gpu::wait") required = "gpu_await";
        if (kind == "gpu::launch") {
          auto kernel = dyn_cast_or_null<KernelOp>(SymbolTable::lookupNearestSymbolFrom(op, intrinsic.getCalleeAttr()));
          target = kernel ? contextFor(kernel, kernel.getContextAttr()) : ContextOp{};
          if (!target || !nominal(target).starts_with("Gpu::") || nominal(target) == "Gpu::Loop") {
            reject(op, "launch requires a kernel with a nominal GPU execution context"); return;
          }
          required = "gpu_launch";
        }
        if (required.empty() || !grant(ambient, required, target)) reject(op, "host context lacks the required intrinsic/launch grant");
      } else if (auto call = dyn_cast<func::CallOp>(op)) {
        auto callee = dyn_cast_or_null<func::FuncOp>(SymbolTable::lookupNearestSymbolFrom(op, call.getCalleeAttr()));
        if (!callee || callee.isExternal()) { reject(op, "host call requires a source procedure definition"); return; }
        auto target = contextFor(callee, callee->getAttrOfType<SymbolRefAttr>("metreon.context"));
        if (target && (target != ambient || contextFor(op, op->getAttrOfType<SymbolRefAttr>("metreon.context")) != ambient))
          reject(op, "host procedure call changes nominal execution context");
      } else if (isa<memref::LoadOp, memref::StoreOp>(op)) {
        const auto memory = isa<memref::LoadOp>(op) ? cast<memref::LoadOp>(op).getMemref() : cast<memref::StoreOp>(op).getMemref();
        auto type = cast<MemRefType>(memory.getType());
        if (type.getMemorySpace()) reject(op, "host code cannot dereference a GPU address");
        if (!isa_and_nonnull<memref::AllocaOp>(memory.getDefiningOp()) && (!ambient || !grant(ambient, "host_memory")))
          reject(op, "host memory access requires host_memory authority");
      } else if (!isa<func::FuncOp, func::ReturnOp, memref::AllocaOp>(op) &&
          op->getName().getDialectNamespace() != "arith" && op->getName().getDialectNamespace() != "cf")
        reject(op, "operation is outside the host executable profile");
    });
  });
  return success(valid);
}

FailureOr<std::vector<HostABI>> lowerHostExecution(ModuleOp module) {
  if (failed(verifyHostContexts(module))) return failure();
  auto *context = module.getContext();
  context->getOrLoadDialect<LLVM::LLVMDialect>();
  std::vector<HostABI> abi;
  RewritePatternSet host(context);
  host.add<HostConversion>(context, module, abi);
  ConversionTarget semantic(*context);
  semantic.addIllegalOp<HostIntrinsicOp>();
  semantic.markUnknownOpDynamicallyLegal([](Operation *) { return true; });
  if (failed(applyFullConversion(module, semantic, std::move(host)))) return failure();
  SmallVector<Operation *> declarations;
  for (auto &op : module.getBody()->getOperations()) if (isa<ContextOp, KernelOp>(op)) declarations.push_back(&op);
  for (auto *op : declarations) op->erase();
  module.walk([](Operation *op) {
    SmallVector<StringAttr> names;
    for (auto attr : op->getAttrs()) if (attr.getName().strref().starts_with("metreon.")) names.push_back(attr.getName());
    for (auto name : names) op->removeAttr(name);
  });
  LowerToLLVMOptions options(context);
  options.overrideIndexBitwidth(64);
  LLVMTypeConverter converter(context, options);
  RewritePatternSet patterns(context);
  populateFuncToLLVMConversionPatterns(converter, patterns);
  arith::populateArithToLLVMConversionPatterns(converter, patterns);
  cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
  populateFinalizeMemRefToLLVMConversionPatterns(converter, patterns);
  LLVMConversionTarget target(*context);
  target.addLegalOp<ModuleOp>();
  if (failed(applyFullConversion(module, target, std::move(patterns))) || failed(verify(module))) return failure();
  return abi;
}
} // namespace metreon
