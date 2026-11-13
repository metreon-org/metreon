#include "metreon/MLIR/GPULowering.h"
#include "ScalarLowering.h"
#include "CanonicalSyntax.h"

#include "mlir/Conversion/ControlFlowToSCF/ControlFlowToSCF.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRefMemorySlot.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Rewrite/PatternApplicator.h"
#include "mlir/Transforms/FoldUtils.h"
#include "mlir/Transforms/Passes.h"

#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;
namespace metreon {
namespace {
using Kind = graphir::CallableOperationKind;
using Syntax = parser::CanonicalSyntax;

// MLIR's generic canonicalizer may DCE a result-free, effect-free scf.while
// even if it never terminates. Metreon preserves that control-flow behavior.
// Apply the upstream SCF signature/branch patterns and scalar folds directly;
// dead-code removal is restricted to regionless operations. No loop or enclosing
// branch is erased merely because its memory effects and results are empty.
LogicalResult normalizeStructuredCFG(ModuleOp module) {
  auto *context = module.getContext();
  for (auto function : module.getOps<func::FuncOp>()) {
    SmallVector<Operation *> constants;
    function.walk([&](Operation *op) {
      if (isa<arith::ConstantOp, ub::PoisonOp>(op)) constants.push_back(op);
    });
    for (auto *op : constants) op->moveBefore(&function.getBody().front(), function.getBody().front().begin());
  }
  RewritePatternSet patterns(context);
  scf::WhileOp::getCanonicalizationPatterns(patterns, context);
  scf::IfOp::getCanonicalizationPatterns(patterns, context);
  for (auto operation : context->getRegisteredOperations())
    if (operation.getDialectNamespace() == "arith") operation.getCanonicalizationPatterns(patterns, context);
  FrozenRewritePatternSet frozen(std::move(patterns));
  PatternApplicator applicator(frozen);
  applicator.applyDefaultCostModel();
  struct NormalizationRewriter : PatternRewriter {
    explicit NormalizationRewriter(MLIRContext *context) : PatternRewriter(context) {}
  } rewriter(context);
  for (unsigned rewrites = 0; rewrites < 100000; ++rewrites) {
    OperationFolder folder(context);
    const auto changed = module.walk<WalkOrder::PostOrder>([&](Operation *op) {
      if (!op->getNumRegions() && isOpTriviallyDead(op)) {
        rewriter.eraseOp(op);
        return WalkResult::interrupt();
      }
      if (!op->getNumRegions() && !op->hasTrait<OpTrait::ConstantLike>() &&
          succeeded(folder.tryToFold(op))) return WalkResult::interrupt();
      rewriter.setInsertionPoint(op);
      if (succeeded(applicator.matchAndRewrite(op, rewriter))) return WalkResult::interrupt();
      return WalkResult::advance();
    });
    if (!changed.wasInterrupted()) return success();
  }
  return module.emitError("GPU structured CFG normalization did not converge");
}

// Match the deliberately rotated while produced by MLIR's CFG-to-SCF lift:
// compare; if (compare) { body; next-state } else { unused-state };
// condition(compare, next-state, exit-state). Only an exact signed < / +1
// recurrence is counted. Breaks, modified bounds, unsigned tests and live final
// induction values remain scf.while. The unit step cannot overshoot a
// representable upper bound, including the source integer maximum.
void promoteCountedLoop(scf::WhileOp loop) {
  auto &before = loop.getBefore().front();
  auto &after = loop.getAfter().front();
  if (before.getOperations().size() != 3 || after.getOperations().size() != 1) return;
  auto compare = dyn_cast<arith::CmpIOp>(before.front());
  auto branch = dyn_cast<scf::IfOp>(*std::next(before.begin()));
  auto condition = dyn_cast<scf::ConditionOp>(before.back());
  auto repeat = dyn_cast<scf::YieldOp>(after.back());
  if (!compare || !branch || !condition || !repeat ||
      compare.getPredicate() != arith::CmpIPredicate::slt ||
      branch.getCondition() != compare.getResult() || condition.getCondition() != compare.getResult() ||
      branch.getElseRegion().empty()) return;
  if (branch.getElseRegion().front().getOperations().size() != 1) return;
  auto induction = dyn_cast<BlockArgument>(compare.getLhs());
  if (!induction || induction.getOwner() != &before || !loop.isDefinedOutsideOfLoop(compare.getRhs())) return;
  const auto iv = induction.getArgNumber();
  auto thenYield = cast<scf::YieldOp>(branch.getThenRegion().front().getTerminator());
  SmallVector<Value> next;
  for (auto value : repeat.getOperands()) {
    auto argument = dyn_cast<BlockArgument>(value);
    if (!argument || argument.getOwner() != &after) return;
    auto result = dyn_cast<OpResult>(condition.getArgs()[argument.getArgNumber()]);
    if (!result || result.getOwner() != branch.getOperation()) return;
    next.push_back(thenYield.getOperand(result.getResultNumber()));
  }
  if (next.size() != before.getNumArguments()) return;
  auto increment = next[iv].getDefiningOp<arith::AddIOp>();
  if (!increment || increment.getLhs() != induction || !matchPattern(increment.getRhs(), m_One())) return;
  // Every observable loop exit must be a current carried value, not the
  // untaken branch's synthetic poison. The IV is scoped to the counted loop.
  for (auto [result, value] : llvm::zip(loop.getResults(), condition.getArgs())) {
    if (result.use_empty()) continue;
    auto argument = dyn_cast<BlockArgument>(value);
    if (!argument || argument.getOwner() != &before || argument == induction) return;
  }
  OpBuilder builder(loop);
  SmallVector<Value> inits;
  for (auto [index, value] : llvm::enumerate(loop.getInits())) if (index != iv) inits.push_back(value);
  auto counted = builder.create<scf::ForOp>(loop.getLoc(), loop.getInits()[iv], compare.getRhs(),
      increment.getRhs(), inits, [&](OpBuilder &body, Location loc, Value newIV, ValueRange carried) {
        IRMapping mapping;
        unsigned carriedIndex = 0;
        for (auto argument : before.getArguments())
          mapping.map(argument, argument == induction ? newIV : carried[carriedIndex++]);
        mapping.map(compare.getResult(), body.create<arith::ConstantIntOp>(loc, 1, 1));
        for (auto &op : branch.getThenRegion().front().without_terminator()) body.clone(op, mapping);
        SmallVector<Value> yielded;
        for (auto [index, value] : llvm::enumerate(next)) if (index != iv) yielded.push_back(mapping.lookupOrDefault(value));
        body.create<scf::YieldOp>(loc, yielded);
      });
  for (auto [result, value] : llvm::zip(loop.getResults(), condition.getArgs())) {
    if (result.use_empty()) continue;
    const auto index = cast<BlockArgument>(value).getArgNumber();
    result.replaceAllUsesWith(counted.getResult(index < iv ? index : index - 1));
  }
  loop.erase();
}

bool promoteAffineLoop(scf::ForOp loop) {
  if (!matchPattern(loop.getStep(), m_One())) return false;
  auto type = loop.getInductionVar().getType();
  // This stage uses 64-bit GPU indices. Narrow signed integer induction can
  // be widened without truncation; i64 and unsigned recurrences stay SCF.
  if (!type.isIndex() && (!isa<IntegerType>(type) || cast<IntegerType>(type).getWidth() > 32)) return false;
  OpBuilder builder(loop);
  SmallVector<Operation *> casts;
  auto indexValue = [&](Value value) -> Value {
    if (value.getType().isIndex()) return value;
    IntegerAttr constant;
    if (matchPattern(value, m_Constant(&constant))) {
      auto op = builder.create<arith::ConstantIndexOp>(loop.getLoc(), constant.getValue().getSExtValue());
      casts.push_back(op);
      return op;
    }
    auto op = builder.create<arith::IndexCastOp>(loop.getLoc(), builder.getIndexType(), value);
    casts.push_back(op);
    return op;
  };
  auto lower = indexValue(loop.getLowerBound()), upper = indexValue(loop.getUpperBound());
  auto *scope = affine::getAffineScope(loop);
  if (!affine::isValidDim(lower, scope) || !affine::isValidDim(upper, scope)) {
    for (auto *op : llvm::reverse(casts)) op->erase();
    return false;
  }
  auto identity = AffineMap::get(1, 0, builder.getAffineDimExpr(0));
  auto lowerMap = identity, upperMap = identity;
  SmallVector<Value> lowerOperands{lower}, upperOperands{upper};
  IntegerAttr constant;
  if (matchPattern(lower, m_Constant(&constant))) {
    lowerMap = AffineMap::getConstantMap(constant.getValue().getSExtValue(), loop.getContext());
    lowerOperands.clear();
  }
  if (matchPattern(upper, m_Constant(&constant))) {
    upperMap = AffineMap::getConstantMap(constant.getValue().getSExtValue(), loop.getContext());
    upperOperands.clear();
  }
  auto affineLoop = builder.create<affine::AffineForOp>(loop.getLoc(), lowerOperands, lowerMap,
      upperOperands, upperMap, 1, loop.getInitArgs(),
      [&](OpBuilder &body, Location loc, Value induction, ValueRange carried) {
        IRMapping mapping;
        Value sourceIV = induction;
        if (!type.isIndex()) sourceIV = body.create<arith::IndexCastOp>(loc, type, induction);
        mapping.map(loop.getInductionVar(), sourceIV);
        mapping.map(loop.getRegionIterArgs(), carried);
        for (auto &op : loop.getBody()->without_terminator()) body.clone(op, mapping);
        SmallVector<Value> values;
        for (auto value : cast<scf::YieldOp>(loop.getBody()->getTerminator()).getOperands())
          values.push_back(mapping.lookupOrDefault(value));
        body.create<affine::AffineYieldOp>(loc, values);
      });
  loop.replaceAllUsesWith(affineLoop.getResults());
  loop.erase();
  return true;
}

void promoteAffineConditional(scf::IfOp branch) {
  auto compare = branch.getCondition().getDefiningOp<arith::CmpIOp>();
  if (!compare || !compare.getLhs().getType().isIndex()) return;
  auto *scope = affine::getAffineScope(branch);
  if (!affine::isValidDim(compare.getLhs(), scope) || !affine::isValidDim(compare.getRhs(), scope)) return;
  // Affine constraint lowering subtracts operands. Restrict this promotion to
  // nonnegative indices so the signed difference (and the strict -1) fits the
  // 64-bit profile. An arbitrary signed index comparison stays scf.if.
  auto nonnegative = [](Value value) {
    IntegerAttr constant;
    if (matchPattern(value, m_Constant(&constant))) return !constant.getValue().isNegative();
    auto owner = affine::getForInductionVarOwner(value);
    return owner && owner.hasConstantLowerBound() && owner.getConstantLowerBound() >= 0;
  };
  if (!nonnegative(compare.getLhs()) || !nonnegative(compare.getRhs())) return;
  OpBuilder builder(branch);
  auto left = builder.getAffineDimExpr(0), right = builder.getAffineDimExpr(1);
  AffineExpr expression;
  bool equal = false;
  switch (compare.getPredicate()) {
  case arith::CmpIPredicate::eq: expression = left - right; equal = true; break;
  case arith::CmpIPredicate::slt: expression = right - left - 1; break;
  case arith::CmpIPredicate::sle: expression = right - left; break;
  case arith::CmpIPredicate::sgt: expression = left - right - 1; break;
  case arith::CmpIPredicate::sge: expression = left - right; break;
  default: return; // != is a disjunction; unsigned comparisons are not signed sets.
  }
  auto set = IntegerSet::get(2, 0, {expression}, {equal});
  auto affineIf = builder.create<affine::AffineIfOp>(branch.getLoc(), branch.getResultTypes(), set,
      ValueRange{compare.getLhs(), compare.getRhs()}, !branch.getElseRegion().empty());
  auto transfer = [&](Region &from, Region &to) {
    if (from.empty()) return;
    to.takeBody(from);
    auto yield = cast<scf::YieldOp>(to.front().getTerminator());
    OpBuilder atYield(yield);
    atYield.create<affine::AffineYieldOp>(yield.getLoc(), yield.getOperands());
    yield.erase();
  };
  transfer(branch.getThenRegion(), affineIf.getThenRegion());
  transfer(branch.getElseRegion(), affineIf.getElseRegion());
  branch.replaceAllUsesWith(affineIf.getResults());
  branch.erase();
}

class GPULowering {
  const graphir::Module &source;
  MLIRContext &context;
  OwningOpRef<ModuleOp> output;
  SourceLocation current;
  std::map<std::string, const graphir::CallableGraph *> procedures;
  SmallVector<const graphir::CallableGraph *> selected;
  std::set<std::string> selectedNames, kernels;

  [[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
  Location location(SourceLocation loc) {
    return FileLineColLoc::get(&context, source.sourceName(), loc.line, loc.column);
  }
  void gpuContext(const graphir::ContextMetadataRef &metadata, bool loop) {
    if (!metadata) {
      if (loop) fail("GPU loops require Gpu::Loop evidence");
      return;
    }
    source.requireContextUse(metadata, graphir::ContextUse::MetadataAttachment);
    const auto type = Syntax::type(metadata->name());
    if (type.name.components.empty() || type.name.components.front() != "Gpu" ||
        (loop ? type.name.str() != "Gpu::Loop" : type.name.components.back() == "Loop"))
      fail(loop ? "GPU loops require Gpu::Loop evidence" : "GPU lowering cannot consume a non-GPU execution context");
    if (loop) {
      if (metadata->genericArity()) fail("GPU loop evidence cannot have generic arguments");
      for (const auto &node : source.nodes())
        if (node.kind == graphir::NodeKind::Grant && node.context && node.context->hasSameIdentity(*metadata))
          fail("GPU loop contexts cannot grant execution authority");
    }
  }
  void select(const graphir::CallableGraph &graph) {
    if (selectedNames.insert(graph.name()).second) selected.push_back(&graph);
  }
  void preflight() {
    for (const auto &graph : source.procedureGraphs()) {
      procedures.emplace(graph.name(), &graph);
      if (graph.attributes().at("execution_domain") == "Gpu") select(graph);
    }
    for (const auto &graph : source.kernelGraphs()) {
      kernels.insert(graph.name());
      select(graph);
    }
    if (selected.empty()) fail("GPU emission requires a GPU kernel or GPU-context procedure");
    // Extend the device call graph only through checked procedure calls. An
    // unrelated context-independent host body is not silently moved to a GPU.
    for (std::size_t index = 0; index < selected.size(); ++index) {
      const auto &graph = *selected[index];
      current = graph.location();
      const auto domain = graph.attributes().at("execution_domain");
      if (domain != "Gpu" && domain != "context_independent")
        fail("GPU calls cannot cross into host or hardware execution domains");
      gpuContext(graph.context(), false);
      for (const auto &parameter : graph.parameters())
        if (parameter.context) gpuContext(parameter.context, false);
      graphir::walkCallableOperations(graph.body(), [&](const auto &op) {
        current = op.location;
        gpuContext(op.context, false);
        const bool loop = op.kind == Kind::For || op.kind == Kind::While || op.kind == Kind::Until || op.kind == Kind::Loop;
        if (loop || op.loopContext) gpuContext(op.loopContext, true);
        switch (op.kind) {
        case Kind::IteratorInit: case Kind::IteratorHasNext:
        case Kind::IteratorValue: case Kind::IteratorAdvance:
          fail("GPU container iteration requires an explicit extent, layout and access contract");
        case Kind::Call: {
          if (op.attributes.at("callee_kind") != "procedure")
            fail("GPU resource transitions require a checked device implementation");
          auto target = procedures.find(op.name);
          if (target == procedures.end()) fail("GPU call has no declared procedure");
          select(*target->second);
          break;
        }
        case Kind::Until:
          if (op.attributes.at("stop_value") != "true" && op.attributes.at("stop_value") != "false")
            fail("invalid GraphIR until stop value");
          break;
        default: break;
        }
      });
    }
  }

public:
  GPULowering(const graphir::Module &source, MLIRContext &context, OwningOpRef<ModuleOp> semantic)
      : source(source), context(context), output(std::move(semantic)) {}
  FailureOr<OwningOpRef<ModuleOp>> run() {
    try {
      preflight();
      DialectRegistry registry;
      memref::registerMemorySlotExternalModels(registry);
      context.appendDialectRegistry(registry);
      context.getOrLoadDialect<scf::SCFDialect>();
      context.getOrLoadDialect<affine::AffineDialect>();
      context.getOrLoadDialect<gpu::GPUDialect>();
      context.getOrLoadDialect<ub::UBDialect>();
      // The scalar importer realizes exactly the current GraphIR CFG, including
      // early exits and short circuiting. MLIR performs the CFG restructuring.
      auto device = detail::lowerScalarCFG(source, selected, context, 64);
      if (failed(device)) return failure();
      PassManager promote(&context);
      promote.addPass(createMem2Reg());
      if (failed(promote.run(**device))) return failure();
      ControlFlowToSCFTransformation conversion;
      for (auto function : (*device)->getOps<func::FuncOp>()) {
        DominanceInfo dominance(function);
        if (failed(transformCFGToSCF(function.getBody(), conversion, dominance))) return failure();
      }
      if (failed(normalizeStructuredCFG(**device))) return failure();
      if (failed(verify(**device))) return failure();

      OpBuilder builder(&context);
      builder.setInsertionPointToEnd(output->getBody());
      std::string name = "__metreon_gpu";
      for (unsigned suffix = 0; SymbolTable::lookupSymbolIn(*output, name); ++suffix)
        name = "__metreon_gpu_" + std::to_string(suffix);
      auto gpuModule = builder.create<gpu::GPUModuleOp>(location({}), name);
      SmallVector<func::FuncOp> functions((*device)->getOps<func::FuncOp>());
      for (auto function : functions) {
        if (kernels.contains(function.getSymName().str())) {
          builder.setInsertionPoint(gpuModule.getBody()->getTerminator());
          auto kernel = builder.create<gpu::GPUFuncOp>(function.getLoc(), function.getSymName(), function.getFunctionType());
          kernel->setAttr(gpu::GPUDialect::getKernelFuncAttrName(), builder.getUnitAttr());
          kernel.getBody().takeBody(function.getBody());
          kernel.walk([&](func::ReturnOp ret) {
            OpBuilder atReturn(ret);
            atReturn.create<gpu::ReturnOp>(ret.getLoc(), ret.getOperands());
            ret.erase();
          });
          function.erase();
        } else function->moveBefore(gpuModule.getBody()->getTerminator());
      }
      SmallVector<scf::WhileOp> whileLoops;
      gpuModule.walk<WalkOrder::PostOrder>([&](scf::WhileOp loop) { whileLoops.push_back(loop); });
      for (auto loop : whileLoops) promoteCountedLoop(loop);
      // Outside-in conversion exposes enclosing affine IVs to nested bounds.
      // Loop conversion clones its body, so restart the walk after each success
      // instead of retaining handles to operations in a replaced outer loop.
      while (gpuModule.walk<WalkOrder::PreOrder>([&](scf::ForOp loop) {
        return promoteAffineLoop(loop) ? WalkResult::interrupt() : WalkResult::advance();
      }).wasInterrupted()) {}
      SmallVector<scf::IfOp> branches;
      gpuModule.walk<WalkOrder::PostOrder>([&](scf::IfOp branch) { branches.push_back(branch); });
      for (auto branch : branches) promoteAffineConditional(branch);
      for (auto *graph : selected) {
        auto *contract = SymbolTable::lookupSymbolIn(*output, graph->name());
        contract->setAttr("metreon.device_body", SymbolRefAttr::get(&context, name,
            {FlatSymbolRefAttr::get(&context, graph->name())}));
      }
      (*output)->setAttr("metreon.gpu_lowering", builder.getStringAttr("structured_scalar_control_flow"));
      (*output)->setAttr("metreon.gpu_index_bits", builder.getI64IntegerAttr(64));
      (*output)->setAttr("metreon.ordinary_lowering", builder.getStringAttr("partial_gpu_scalar"));
      bool complete = true;
      gpuModule.walk([&](Operation *op) {
        if (op->getName().getDialectNamespace() == "cf" || op->getName().getDialectNamespace() == "memref") {
          op->emitError("GPU scalar restructuring left CFG branches or scalar storage");
          complete = false;
        }
      });
      if (!complete || failed(verify(*output))) return failure();
      return std::move(output);
    } catch (const std::exception &error) {
      emitError(location(current)) << "GraphIR GPU control-flow lowering: " << error.what();
      return failure();
    }
  }
};
} // namespace

FailureOr<OwningOpRef<ModuleOp>> lowerGPUControlFlow(const graphir::Module &source, MLIRContext &context) {
  auto semantic = importGraphIR(source, context);
  if (failed(semantic)) return failure();
  return GPULowering(source, context, std::move(*semantic)).run();
}
} // namespace metreon
