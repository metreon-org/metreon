#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/MLIR/Dialect.h"
#include "CanonicalSyntax.h"
#include "ScalarLowering.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Transforms/DialectConversion.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

using namespace mlir;
namespace metreon {
namespace {
using Kind = graphir::CallableOperationKind;
using Syntax = parser::CanonicalSyntax;

DictionaryAttr application(OpBuilder &builder, const ast::TypeReference &type) {
  SmallVector<Attribute> arguments;
  for (const auto &argument : type.arguments) arguments.push_back(application(builder, argument));
  return builder.getDictionaryAttr({builder.getNamedAttr("name", builder.getStringAttr(type.name.str())),
      builder.getNamedAttr("arguments", builder.getArrayAttr(arguments))});
}
void graphContextChecks(const graphir::Module &source, ArrayRef<const graphir::CallableGraph *> graphs) {
  if (!source.resourceGraphs().empty() || !source.stageGraphs().empty() || !source.protocolGraphs().empty() || !source.pipelineGraphs().empty())
    throw std::runtime_error("device execution profile does not implement resource/protocol/pipeline execution");
  std::map<std::string, const graphir::CallableGraph *> procedures;
  for (const auto &graph : source.procedureGraphs()) procedures.emplace(graph.name(), &graph);
  auto owned = [&](const graphir::ContextMetadataRef &metadata) {
    source.requireContextUse(metadata, graphir::ContextUse::MetadataAttachment);
    if (!metadata->hasIdentifier()) throw std::runtime_error("device context must have a declared identity");
  };
  for (const auto *graph : graphs) {
    const auto ambient = graph->context();
    if (ambient) {
      owned(ambient);
      const auto type = Syntax::type(ambient->name()).name.str();
      if (!type.starts_with("Gpu::") || type == "Gpu::Loop") throw std::runtime_error("device execution requires a Gpu execution context");
    }
    const auto domain = graph->attributes().at("execution_domain");
    if (domain != (ambient ? "Gpu" : "context_independent") && !(domain == "Gpu" && !ambient))
      throw std::runtime_error("device execution cannot lower host or hardware procedures");
    for (const auto &parameter : graph->parameters()) if (parameter.context) {
      owned(parameter.context);
      if (!ambient || !ambient->hasSameIdentity(*parameter.context)) throw std::runtime_error("parameter changes nominal execution context");
    }
    graphir::walkCallableOperations(graph->body(), [&](const auto &op) {
      if (op.context) {
        owned(op.context);
        if (!ambient || !ambient->hasSameIdentity(*op.context)) throw std::runtime_error("operation changes nominal execution context");
      }
      const bool loop = op.kind == Kind::For || op.kind == Kind::Loop || op.kind == Kind::While || op.kind == Kind::Until;
      if (loop || op.loopContext) {
        if (!op.loopContext) throw std::runtime_error("device loop has no Gpu::Loop context");
        owned(op.loopContext);
        if (op.loopContext->name() != "Gpu::Loop" || op.loopContext->genericArity()) throw std::runtime_error("device loops require Gpu::Loop evidence");
        for (const auto &node : source.nodes()) if (node.kind == graphir::NodeKind::Grant && node.context && node.context->hasSameIdentity(*op.loopContext))
          throw std::runtime_error("loop contexts cannot grant execution authority");
      }
      if (op.kind == Kind::Call) {
        const auto kind = op.attributes.at("callee_kind");
        if (kind == "gpu_builtin") {
          if (!op.context || (op.name != "gpu::global_id" && op.name != "gpu::load" && op.name != "gpu::store"))
            throw std::runtime_error("unknown or context-free GPU intrinsic");
        } else if (kind == "procedure") {
          auto found = procedures.find(op.name);
          if (found == procedures.end()) throw std::runtime_error("device call has no procedure declaration");
          const auto target = found->second->context();
          if (target && (!ambient || !target->hasSameIdentity(*ambient))) throw std::runtime_error("call requires a different nominal execution context");
        } else throw std::runtime_error("device execution does not implement this call kind");
      }
    });
  }
}

ContextOp resolveContext(Operation *op, SymbolRefAttr reference) {
  return reference ? dyn_cast_or_null<ContextOp>(SymbolTable::lookupNearestSymbolFrom(op, reference)) : ContextOp{};
}
bool grants(ContextOp context, StringRef name) {
  for (auto value : context.getGrants())
    if (cast<DictionaryAttr>(value).getAs<StringAttr>("name").getValue() == name) return true;
  return false;
}
bool deviceSpan(Type type) {
  auto span = dyn_cast<MemRefType>(type);
  auto space = span ? dyn_cast_or_null<IntegerAttr>(span.getMemorySpace()) : IntegerAttr{};
  return span && span.getRank() == 1 && span.isDynamicDim(0) && span.getElementType().isF32() &&
      span.getLayout().isIdentity() && space && space.getInt() == 1;
}
bool scalar(Type type) { return isa<IntegerType, FloatType, IndexType>(type); }

struct IntrinsicConversion : OpConversionPattern<DeviceIntrinsicOp> {
  using OpConversionPattern::OpConversionPattern;
  LogicalResult matchAndRewrite(DeviceIntrinsicOp op, OpAdaptor adaptor, ConversionPatternRewriter &rewriter) const override {
    const auto args = adaptor.getInputs();
    if (op.getKind() == "global_id") {
      auto block = rewriter.create<gpu::BlockIdOp>(op.getLoc(), gpu::Dimension::x);
      auto width = rewriter.create<gpu::BlockDimOp>(op.getLoc(), gpu::Dimension::x);
      auto thread = rewriter.create<gpu::ThreadIdOp>(op.getLoc(), gpu::Dimension::x);
      auto offset = rewriter.create<arith::MulIOp>(op.getLoc(), block, width);
      rewriter.replaceOpWithNewOp<arith::AddIOp>(op, offset, thread);
    } else if (op.getKind() == "load") {
      rewriter.replaceOpWithNewOp<memref::LoadOp>(op, args[0], ValueRange{args[1]});
    } else {
      rewriter.create<memref::StoreOp>(op.getLoc(), args[2], args[0], ValueRange{args[1]});
      rewriter.eraseOp(op);
    }
    return success();
  }
};
template <typename Contract> struct CallableConversion : OpConversionPattern<Contract> {
  gpu::GPUModuleOp destination;
  CallableConversion(MLIRContext *context, gpu::GPUModuleOp destination)
      : OpConversionPattern<Contract>(context), destination(destination) {}
  LogicalResult matchAndRewrite(Contract op, typename Contract::Adaptor,
      ConversionPatternRewriter &rewriter) const override {
    auto targetModule = destination;
    auto body = cast<func::FuncOp>(SymbolTable::lookupNearestSymbolFrom(op, op.getBodyRefAttr()));
    rewriter.setInsertionPoint(targetModule.getBody()->getTerminator());
    if constexpr (std::is_same_v<Contract, KernelOp>) {
      auto kernel = rewriter.template create<gpu::GPUFuncOp>(op.getLoc(), op.getSymName(), op.getFunctionType());
      kernel->setAttr(gpu::GPUDialect::getKernelFuncAttrName(), rewriter.getUnitAttr());
      rewriter.inlineRegionBefore(body.getBody(), kernel.getBody(), kernel.getBody().end());
      // GPUFuncOp's builder creates its entry block. Remove that empty block
      // before transferring the checked function body.
      if (kernel.getBody().front().empty()) rewriter.eraseBlock(&kernel.getBody().front());
      SmallVector<func::ReturnOp> returns;
      kernel.walk([&](func::ReturnOp ret) { returns.push_back(ret); });
      for (auto ret : returns) {
        rewriter.setInsertionPoint(ret);
        rewriter.template replaceOpWithNewOp<gpu::ReturnOp>(ret, ret.getOperands());
      }
      rewriter.eraseOp(body);
    } else {
      rewriter.modifyOpInPlace(body, [&] { body->moveBefore(targetModule.getBody()->getTerminator()); });
    }
    rewriter.eraseOp(op);
    return success();
  }
};
struct ContextConversion : OpConversionPattern<ContextOp> {
  using OpConversionPattern::OpConversionPattern;
  LogicalResult matchAndRewrite(ContextOp op, OpAdaptor, ConversionPatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};
} // namespace

FailureOr<OwningOpRef<ModuleOp>> importDeviceExecution(const graphir::Module &source, MLIRContext &context) {
  try {
    SmallVector<const graphir::CallableGraph *> graphs;
    for (const auto &graph : source.kernelGraphs()) graphs.push_back(&graph);
    std::set<std::string> selected;
    if (graphs.empty()) for (const auto &graph : source.procedureGraphs())
      if (graph.attributes().at("execution_domain") != "Host") { graphs.push_back(&graph); selected.insert(graph.name()); }
    for (size_t index = 0; index < graphs.size(); ++index)
      graphir::walkCallableOperations(graphs[index]->body(), [&](const auto &op) {
        if (op.kind != Kind::Call || op.attributes.at("callee_kind") != "procedure") return;
        for (const auto &procedure : source.procedureGraphs()) if (procedure.name() == op.name && selected.insert(op.name).second)
          graphs.push_back(&procedure);
      });
    graphContextChecks(source, graphs);
    auto bodies = detail::lowerScalarCFG(source, graphs, context, 64, true);
    if (failed(bodies)) return failure();
    auto semantic = OwningOpRef<ModuleOp>(ModuleOp::create((*bodies)->getLoc()));
    (*semantic)->setAttrs((**bodies)->getAttrs());
    OpBuilder builder(&context);
    std::string name = "__metreon_bodies";
    for (unsigned suffix = 0; llvm::any_of(graphs, [&](const auto *graph) { return graph->name() == name; }); ++suffix)
      name = "__metreon_bodies_" + std::to_string(suffix);
    (**bodies)->setAttr(SymbolTable::getSymbolAttrName(), builder.getStringAttr(name));
    (**bodies)->setAttr("metreon.ordinary_lowering", builder.getStringAttr("primary_with_context_intrinsics"));
    semantic->getBody()->push_back(bodies->release().getOperation());
    for (const auto *graph : graphs) {
      auto function = cast<func::FuncOp>(SymbolTable::lookupSymbolIn(*semantic,
          SymbolRefAttr::get(&context, name, {FlatSymbolRefAttr::get(&context, graph->name())})));
      SmallVector<Attribute> params, results;
      unsigned runtime = 0;
      for (const auto &parameter : graph->parameters()) if (!parameter.context) {
        const auto sourceType = Syntax::valueType(parameter.type);
        params.push_back(builder.getDictionaryAttr({builder.getNamedAttr("name", builder.getStringAttr(parameter.name)),
            builder.getNamedAttr("type", TypeAttr::get(function.getArgumentTypes()[runtime++])),
            builder.getNamedAttr("ownership", builder.getStringAttr("none")),
            builder.getNamedAttr("source_type", application(builder, sourceType.reference))}));
      }
      if (function.getNumResults()) {
        results.push_back(builder.getDictionaryAttr({builder.getNamedAttr("name", builder.getStringAttr("")),
            builder.getNamedAttr("type", TypeAttr::get(function.getResultTypes()[0])),
            builder.getNamedAttr("ownership", builder.getStringAttr("none")),
            builder.getNamedAttr("source_type", application(builder, Syntax::valueType(graph->attributes().at("return_type")).reference))}));
      }
      const bool kernel = std::any_of(source.kernelGraphs().begin(), source.kernelGraphs().end(), [&](const auto &item) { return &item == graph; });
      OperationState state(function.getLoc(), kernel ? KernelOp::getOperationName() : ProcedureOp::getOperationName());
      state.addAttribute("sym_name", builder.getStringAttr(graph->name()));
      state.addAttribute("function_type", TypeAttr::get(function.getFunctionType()));
      state.addAttribute("parameter_attrs", builder.getArrayAttr(params));
      state.addAttribute("result_attrs", builder.getArrayAttr(results));
      for (const auto *key : {"context_requirements", "effects", "deferred_body", "deferred_cfg"}) state.addAttribute(key, builder.getArrayAttr({}));
      state.addAttribute("execution_domain", builder.getStringAttr(graph->attributes().at("execution_domain")));
      state.addAttribute("body_ref", SymbolRefAttr::get(&context, name, {FlatSymbolRefAttr::get(&context, graph->name())}));
      if (graph->context()) state.addAttribute("context", SymbolRefAttr::get(&context, name,
          {FlatSymbolRefAttr::get(&context, graph->context()->identifier())}));
      builder.setInsertionPointToEnd(semantic->getBody());
      builder.create(state);
    }
    (*semantic)->setAttr("metreon.verification_profile", builder.getStringAttr("contexts_only"));
    (*semantic)->setAttr("metreon.ordinary_lowering", builder.getStringAttr("live_device_bodies"));
    if (failed(verify(*semantic)) || failed(verifyDeviceContexts(*semantic))) return failure();
    return semantic;
  } catch (const std::exception &error) {
    emitError(FileLineColLoc::get(&context, source.sourceName(), 1, 1)) << "GraphIR device import: " << error.what();
    return failure();
  }
}

LogicalResult verifyDeviceContexts(ModuleOp module) {
  if (failed(verify(module))) return failure();
  std::map<std::string, Operation *> contracts;
  std::set<Operation *> bodies;
  for (auto &op : module.getBody()->getOperations()) if (isa<KernelOp, ProcedureOp>(op))
    contracts.emplace(cast<StringAttr>(op.getAttr("sym_name")).str(), &op);
  bool valid = true;
  auto reject = [&](Operation *op, StringRef message) { op->emitError(message); valid = false; };
  for (const auto &[name, contract] : contracts) {
    auto reference = contract->getAttrOfType<SymbolRefAttr>("body_ref");
    auto function = reference ? dyn_cast_or_null<func::FuncOp>(SymbolTable::lookupNearestSymbolFrom(contract, reference)) : func::FuncOp{};
    if (!function || !bodies.insert(function).second) { reject(contract, "device contract requires one unique live function body"); continue; }
    if (function.isExternal() || function.getSymName() != name)
      reject(contract, "device contract requires a defined function body with the same name");
    if (function.getFunctionType() != cast<TypeAttr>(contract->getAttr("function_type")).getValue())
      reject(contract, "device body signature disagrees with contract");
    for (auto type : function.getArgumentTypes()) if (!scalar(type) && !deviceSpan(type))
      reject(contract, "device parameters must be scalars or dynamic f32 global spans");
    for (auto type : function.getResultTypes()) if (!scalar(type))
      reject(contract, "device helper results must be scalars");
    auto ambient = resolveContext(contract, contract->getAttrOfType<SymbolRefAttr>("context"));
    if (contract->hasAttr("context") && !ambient) reject(contract, "unknown nominal execution context");
    const auto domain = contract->getAttrOfType<StringAttr>("execution_domain").getValue();
    if (domain != (ambient ? "Gpu" : "context_independent") && !(isa<KernelOp>(contract) && domain == "Gpu" && !ambient))
      reject(contract, "device body execution domain disagrees with its context");
    if (ambient) {
      const auto nominal = ambient.getNominalType().getAs<StringAttr>("name").getValue();
      if (!nominal.starts_with("Gpu::") || nominal == "Gpu::Loop") reject(contract, "device body requires a Gpu execution context");
    }
    if (auto loops = function->getAttrOfType<ArrayAttr>("metreon.loop_contexts")) for (auto item : loops) {
      auto loop = resolveContext(function, dyn_cast<SymbolRefAttr>(item));
      if (!loop || loop.getNominalType().getAs<StringAttr>("name").getValue() != "Gpu::Loop" ||
          !loop.getGrants().empty() || !loop.getGenericParameters().empty())
        reject(function, "device loop evidence must reference a grant-free Gpu::Loop context");
    }
    function.walk([&](Operation *op) {
      auto require = [&](ContextOp selected, StringRef grant) {
        if (!ambient || !selected || selected != ambient) { reject(op, "device operation requires the same nominal execution context as its body"); return; }
        if (!grant.empty() && !grants(selected, grant)) reject(op, ("device context lacks required grant: " + grant).str());
      };
      if (auto intrinsic = dyn_cast<DeviceIntrinsicOp>(op)) {
        require(resolveContext(op, intrinsic.getContextAttr()), intrinsic.getKind() == "load" ? "gpu_load" : intrinsic.getKind() == "store" ? "gpu_store" : "");
      } else if (auto call = dyn_cast<func::CallOp>(op)) {
        auto target = contracts.find(call.getCallee().str());
        if (target == contracts.end() || !isa<ProcedureOp>(target->second)) { reject(op, "device call has no procedure contract"); return; }
        auto targetContext = resolveContext(target->second, target->second->getAttrOfType<SymbolRefAttr>("context"));
        if (targetContext) {
          require(resolveContext(op, op->getAttrOfType<SymbolRefAttr>("metreon.context")), "");
          if (targetContext != ambient) reject(op, "callee requires a different nominal execution context");
        }
      } else if (auto load = dyn_cast<memref::LoadOp>(op)) {
        if (load.getMemRefType().getRank() != 0 && !deviceSpan(load.getMemRefType()))
          reject(op, "device memory access requires an f32 global span");
        if (load.getMemRefType().getMemorySpace() || !isa_and_nonnull<memref::AllocaOp>(load.getMemref().getDefiningOp()))
          require(ambient, "gpu_load");
      } else if (auto store = dyn_cast<memref::StoreOp>(op)) {
        if (store.getMemRefType().getRank() != 0 && !deviceSpan(store.getMemRefType()))
          reject(op, "device memory access requires an f32 global span");
        if (store.getMemRefType().getMemorySpace() || !isa_and_nonnull<memref::AllocaOp>(store.getMemref().getDefiningOp()))
          require(ambient, "gpu_store");
      } else if (isa<gpu::BlockIdOp, gpu::BlockDimOp, gpu::ThreadIdOp>(op)) {
        require(ambient, "");
      } else if (auto local = dyn_cast<memref::AllocaOp>(op)) {
        if (local.getType().getRank() || local.getType().getMemorySpace() || !scalar(local.getType().getElementType()))
          reject(op, "device local slots must hold scalars in the default address space");
      } else {
        const auto dialect = op->getName().getDialectNamespace();
        if (dialect != "arith" && dialect != "cf" &&
            !isa<func::FuncOp, func::ReturnOp>(op))
          reject(op, "operation is outside the context-only device profile");
      }
    });
  }
  module.walk([&](func::FuncOp function) { if (!bodies.contains(function)) reject(function, "uncontracted function in device module"); });
  return success(valid);
}

LogicalResult lowerMetreonDeviceToGPU(ModuleOp module) {
  // Recheck the live MLIR, including parsed or edited bodies. No GraphIR object
  // or cached `verified` flag is consulted by this conversion.
  if (failed(verifyDeviceContexts(module))) return failure();
  auto *context = module.getContext();
  context->getOrLoadDialect<gpu::GPUDialect>();
  OpBuilder builder(context);
  builder.setInsertionPointToEnd(module.getBody());
  std::string name = "__metreon_device";
  for (unsigned suffix = 0; SymbolTable::lookupSymbolIn(module, name); ++suffix) name = "__metreon_device_" + std::to_string(suffix);
  auto device = builder.create<gpu::GPUModuleOp>(module.getLoc(), name);
  RewritePatternSet patterns(context);
  patterns.add<IntrinsicConversion, ContextConversion>(context);
  patterns.add<CallableConversion<KernelOp>, CallableConversion<ProcedureOp>>(context, device);
  ConversionTarget target(*context);
  target.addIllegalDialect<MetreonDialect>();
  target.markUnknownOpDynamicallyLegal([](Operation *) { return true; });
  if (failed(applyFullConversion(module, target, std::move(patterns)))) return failure();
  SmallVector<ModuleOp> empty;
  for (auto nested : module.getOps<ModuleOp>()) if (nested.getBody()->empty()) empty.push_back(nested);
  for (auto nested : empty) nested.erase();
  module.walk([](Operation *op) {
    SmallVector<StringAttr> remove;
    for (auto attribute : op->getAttrs()) if (attribute.getName().getValue().starts_with("metreon.")) remove.push_back(attribute.getName());
    for (auto name : remove) op->removeAttr(name);
  });
  return verify(module);
}

llvm::json::Object deviceLaunchManifest(ModuleOp semantic) {
  llvm::json::Array kernels;
  for (auto kernel : semantic.getOps<KernelOp>()) {
    llvm::json::Array parameters;
    for (auto item : kernel.getParameterAttrs()) {
      auto metadata = cast<DictionaryAttr>(item);
      auto type = metadata.getAs<TypeAttr>("type").getValue();
      std::string kind;
      const auto sourceType = metadata.getAs<DictionaryAttr>("source_type").getAs<StringAttr>("name").getValue();
      if (auto span = dyn_cast<MemRefType>(type)) {
        if (sourceType != "gpu::span" || !deviceSpan(span))
          throw std::runtime_error("launch profile requires contiguous dynamic rank-one f32 global spans");
        kind = "span_f32";
      }
      else if (type.isIndex()) kind = "index";
      else if (type.isInteger(32) && sourceType == "i32") kind = "i32";
      else if (type.isF32()) kind = "f32";
      else throw std::runtime_error("launch profile supports gpu::span<f32>, index, i32 and f32 arguments");
      parameters.push_back(llvm::json::Object{{"name", metadata.getAs<StringAttr>("name").str()}, {"kind", kind}});
    }
    kernels.push_back(llvm::json::Object{{"name", kernel.getSymName().str()}, {"parameters", std::move(parameters)}});
  }
  return llvm::json::Object{{"schema_version", 1}, {"index_bits", 64}, {"memref_abi", "expanded_rank_one"}, {"kernels", std::move(kernels)}};
}
} // namespace metreon
