#include "ScalarLowering.h"
#include "CanonicalSyntax.h"
#include "metreon/GraphIR/ControlFlow.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;
namespace metreon::detail {
namespace {
using Attributes = std::map<std::string, std::string>;
using Syntax = parser::CanonicalSyntax;
using Id = graphir::CallableValueId;
std::string required(const Attributes &attrs, const std::string &name) {
  auto value = attrs.find(name);
  if (value == attrs.end()) throw std::runtime_error("missing GraphIR attribute `" + name + "`");
  return value->second;
}
class ScalarCFGBuilder {
  const graphir::Module &source;
  ArrayRef<const graphir::CallableGraph *> graphs;
  MLIRContext &context;
  unsigned indexBits;
  OpBuilder builder;
  OwningOpRef<ModuleOp> output;
  SourceLocation current;
  std::map<std::string, func::FuncOp> functions;
  [[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
  Location location(SourceLocation loc) {
    return FileLineColLoc::get(&context, source.sourceName(), loc.line, loc.column);
  }
  Type scalar(const std::string &text, bool allowVoid = false) {
    auto type = Syntax::valueType(text);
    auto name = type.reference.name.str();
    if (type.ownership != ast::OwnershipQualifier::None)
      fail("scalar lowering does not erase ownership/resource obligations");
    if (name == "view" || name == "buffer")
      fail("scalar view/buffer lowering requires an explicit extent, layout and access contract");
    if (!type.reference.arguments.empty()) fail("scalar lowering requires a concrete scalar type");
    if (name == "void" && allowVoid) return builder.getNoneType();
    if (name == "bool") return builder.getI1Type();
    if (name == "index") return builder.getIndexType();
    if (name == "f16") return builder.getF16Type();
    if (name == "f32") return builder.getF32Type();
    if (name == "f64") return builder.getF64Type();
    if (name == "i8" || name == "u8") return builder.getIntegerType(8);
    if (name == "i16" || name == "u16") return builder.getIntegerType(16);
    if (name == "i32" || name == "u32") return builder.getIntegerType(32);
    if (name == "i64" || name == "u64") return builder.getIntegerType(64);
    fail("unsupported scalar type `" + text + "`");
  }
  bool unsignedType(const std::string &text) {
    return Syntax::valueType(text).reference.name.str().starts_with('u');
  }
  bool slot(const graphir::CallableOperation &op) const {
    const auto found = op.attributes.find("mutable_local");
    return op.kind == graphir::CallableOperationKind::Variable &&
        found != op.attributes.end() && found->second == "true";
  }
  Value constant(const std::string &sourceType, const std::string &kind,
                 const std::string &text, Location loc) {
    auto type = scalar(sourceType);
    if (type.isInteger(1)) {
      if (kind != "boolean" || (text != "true" && text != "false")) fail("invalid Boolean constant");
      return builder.create<arith::ConstantOp>(loc, type, builder.getBoolAttr(text == "true"));
    }
    if (isa<IntegerType, IndexType>(type)) {
      if (kind != "integer" || text.empty()) fail("invalid integer constant");
      const bool negative = text.front() == '-';
      auto magnitude = StringRef(text).drop_front(negative ? 1 : 0);
      llvm::APInt value;
      if (magnitude.empty() || magnitude.getAsInteger(10, value)) fail("invalid integer literal");
      const auto bits = isa<IndexType>(type) ? indexBits : cast<IntegerType>(type).getWidth();
      const bool isUnsigned = unsignedType(sourceType);
      if ((negative && isUnsigned) || value.getActiveBits() > bits) fail("integer literal is outside its source type range");
      auto extended = value.zextOrTrunc(bits + 1);
      auto boundary = llvm::APInt::getOneBitSet(bits + 1, bits - 1);
      if (!isUnsigned && (negative ? extended.ugt(boundary) : extended.uge(boundary)))
        fail("integer literal is outside its source type range");
      value = value.zextOrTrunc(bits);
      if (negative) value = -value;
      return builder.create<arith::ConstantOp>(loc, type, IntegerAttr::get(type, value));
    }
    auto floating = cast<FloatType>(type);
    llvm::APFloat value(floating.getFloatSemantics());
    if (kind == "infinity") {
      if (text != "infinity" && text != "-infinity") fail("invalid infinity literal");
      value = llvm::APFloat::getInf(floating.getFloatSemantics(), text.front() == '-');
    } else {
      if (kind != "floating") fail("invalid floating-point constant");
      auto status = value.convertFromString(text, llvm::APFloat::rmNearestTiesToEven);
      if (!status) { llvm::consumeError(status.takeError()); fail("invalid floating-point literal"); }
      if (*status & llvm::APFloat::opOverflow) fail("floating-point literal overflows its destination type");
    }
    return builder.create<arith::ConstantOp>(loc, type, FloatAttr::get(type, value));
  }

  Value binary(const graphir::CallableOperation &op, Value left, Value right,
               const std::string &sourceType, Location loc) {
    if (left.getType() != right.getType()) fail("GraphIR binary operand types disagree");
    const auto kind = required(op.attributes, "operator");
    const bool floating = isa<FloatType>(left.getType());
    const bool isUnsigned = unsignedType(sourceType);
    if (left.getType().isInteger(1) && kind != "==" && kind != "!=")
      fail("Boolean GraphIR values support equality comparisons only");
    if (floating) {
      if (kind == "+") return builder.create<arith::AddFOp>(loc, left, right);
      if (kind == "-") return builder.create<arith::SubFOp>(loc, left, right);
      if (kind == "*") return builder.create<arith::MulFOp>(loc, left, right);
      if (kind == "/") return builder.create<arith::DivFOp>(loc, left, right);
      arith::CmpFPredicate predicate;
      if (kind == "==") predicate = arith::CmpFPredicate::OEQ;
      else if (kind == "!=") predicate = arith::CmpFPredicate::UNE;
      else if (kind == "<") predicate = arith::CmpFPredicate::OLT;
      else if (kind == "<=") predicate = arith::CmpFPredicate::OLE;
      else if (kind == ">") predicate = arith::CmpFPredicate::OGT;
      else if (kind == ">=") predicate = arith::CmpFPredicate::OGE;
      else fail("unsupported floating-point GraphIR operator");
      return builder.create<arith::CmpFOp>(loc, predicate, left, right);
    }
    if (kind == "+") return builder.create<arith::AddIOp>(loc, left, right);
    if (kind == "-") return builder.create<arith::SubIOp>(loc, left, right);
    if (kind == "*") return builder.create<arith::MulIOp>(loc, left, right);
    if (kind == "/") {
      if (isUnsigned) return builder.create<arith::DivUIOp>(loc, left, right);
      return builder.create<arith::DivSIOp>(loc, left, right);
    }
    if (kind == "%") {
      if (isUnsigned) return builder.create<arith::RemUIOp>(loc, left, right);
      return builder.create<arith::RemSIOp>(loc, left, right);
    }
    arith::CmpIPredicate predicate;
    if (kind == "==") predicate = arith::CmpIPredicate::eq;
    else if (kind == "!=") predicate = arith::CmpIPredicate::ne;
    else if (kind == "<") predicate = isUnsigned ? arith::CmpIPredicate::ult : arith::CmpIPredicate::slt;
    else if (kind == "<=") predicate = isUnsigned ? arith::CmpIPredicate::ule : arith::CmpIPredicate::sle;
    else if (kind == ">") predicate = isUnsigned ? arith::CmpIPredicate::ugt : arith::CmpIPredicate::sgt;
    else if (kind == ">=") predicate = isUnsigned ? arith::CmpIPredicate::uge : arith::CmpIPredicate::sge;
    else fail("unsupported integer GraphIR operator");
    return builder.create<arith::CmpIOp>(loc, predicate, left, right);
  }

  void declareFunctions() {
    for (const auto *graphPointer : graphs) {
      const auto &graph = *graphPointer;
      current = graph.location();
      SmallVector<Type> parameters, results;
      for (const auto &param : graph.parameters()) if (!param.context) parameters.push_back(scalar(param.type));
      auto result = scalar(required(graph.attributes(), "return_type"), true);
      if (!isa<NoneType>(result)) results.push_back(result);
      builder.setInsertionPointToEnd(output->getBody());
      auto function = builder.create<func::FuncOp>(location(current), graph.name(), builder.getFunctionType(parameters, results));
      functions.emplace(graph.name(), function);
    }
  }

  void lowerFunction(const graphir::CallableGraph &graph) {
    const auto cfg = graphir::buildControlFlow(graph);
    auto function = functions.at(graph.name());
    auto *entry = function.addEntryBlock();
    std::map<graphir::ControlFlowBlockId, Block *> blocks{{cfg.entry, entry}};
    std::map<graphir::ControlFlowBlockId, const graphir::ControlFlowBlock *> sourceBlocks;
    std::map<Id, const graphir::CallableOperation *> operations;
    std::map<Id, Value> values, locals;
    std::map<Id, std::string> types;
    unsigned parameterIndex = 0;
    for (const auto &param : graph.parameters()) if (param.id) {
      values.emplace(*param.id, entry->getArgument(parameterIndex++));
      types.emplace(*param.id, param.type);
    }
    graphir::walkCallableOperations(graph.body(), [&](const auto &op) {
      operations.emplace(op.id, &op);
      if (auto type = op.attributes.find("type"); type != op.attributes.end()) types.emplace(op.id, type->second);
    });
    for (const auto &block : cfg.blocks) {
      sourceBlocks.emplace(block.id, &block);
      if (block.id != cfg.entry) {
        auto *target = new Block;
        function.getBody().push_back(target);
        blocks.emplace(block.id, target);
      }
    }
    builder.setInsertionPointToStart(entry);
    // Scalar slot allocation is hoisted once per invocation, while each source
    // declaration's initialization remains at its original CFG execution point.
    // Unrefined source buffers/views are never converted to these rank-0 slots.
    for (const auto &block : cfg.blocks)
      for (auto id : block.operations) {
        const auto &op = *operations.at(id);
        if (slot(op)) locals.emplace(id, builder.create<memref::AllocaOp>(
            location(op.location), MemRefType::get({}, scalar(types.at(id)))));
      }

    std::set<graphir::ControlFlowBlockId> visited;
    std::vector<graphir::ControlFlowBlockId> order;
    std::function<void(graphir::ControlFlowBlockId)> visit = [&](auto id) {
      if (!visited.insert(id).second) return;
      const auto &term = sourceBlocks.at(id)->terminator;
      if (term.successor) visit(*term.successor);
      if (term.falseSuccessor) visit(*term.falseSuccessor);
      order.push_back(id);
    };
    visit(cfg.entry);
    std::reverse(order.begin(), order.end());
    for (auto blockId : order) {
      const auto &block = *sourceBlocks.at(blockId);
      builder.setInsertionPointToEnd(blocks.at(blockId));
      for (auto id : block.operations) {
        const auto &op = *operations.at(id);
        current = op.location;
        auto loc = location(current);
        auto operand = [&](std::size_t index) {
          if (index >= op.operands.size()) fail("missing GraphIR operand");
          auto found = values.find(op.operands[index]);
          if (found == values.end()) fail("GraphIR operand has no dominating scalar value");
          return found->second;
        };
        Value result;
        switch (op.kind) {
        case graphir::CallableOperationKind::Literal:
          result = constant(types.at(id), required(op.attributes, "kind"), required(op.attributes, "value"), loc);
          break;
        case graphir::CallableOperationKind::Variable: {
          Value initial;
          if (!op.operands.empty()) initial = operand(0);
          else if (required(op.attributes, "initialized") == "true")
            initial = constant(types.at(id), required(op.attributes, "initializer.kind"), required(op.attributes, "initializer.value"), loc);
          if (initial && !op.operands.empty() && types.at(op.operands[0]) != types.at(id))
            fail("GraphIR local initializer source types disagree");
          if (slot(op)) {
            if (initial) builder.create<memref::StoreOp>(loc, initial, locals.at(id), ValueRange{});
          } else result = initial;
          break;
        }
        case graphir::CallableOperationKind::Load:
          if (op.operands.size() != 1 || !locals.contains(op.operands[0])) fail("invalid GraphIR local read");
          if (types.at(id) != types.at(op.operands[0])) fail("GraphIR local read source types disagree");
          result = builder.create<memref::LoadOp>(loc, locals.at(op.operands[0]), ValueRange{});
          break;
        case graphir::CallableOperationKind::Assign:
          if (op.operands.size() != 2 || !locals.contains(op.operands[0])) fail("invalid GraphIR local write");
          if (types.at(op.operands[0]) != types.at(op.operands[1])) fail("GraphIR assignment source types disagree");
          builder.create<memref::StoreOp>(loc, operand(1), locals.at(op.operands[0]), ValueRange{});
          break;
        case graphir::CallableOperationKind::Binary:
          if (op.operands.size() != 2) fail("binary GraphIR operation requires two operands");
          if (types.at(op.operands[0]) != types.at(op.operands[1])) fail("GraphIR binary source types disagree");
          {
            const auto kind = required(op.attributes, "operator");
            const bool comparison = kind == "==" || kind == "!=" || kind == "<" || kind == "<=" || kind == ">" || kind == ">=";
            if (types.at(id) != (comparison ? "bool" : types.at(op.operands[0])))
              fail("GraphIR binary result source type disagrees");
          }
          result = binary(op, operand(0), operand(1), types.at(op.operands[0]), loc);
          break;
        case graphir::CallableOperationKind::Unary: {
          if (op.operands.size() != 1) fail("unary GraphIR operation requires one operand");
          auto value = operand(0);
          const auto kind = required(op.attributes, "operator");
          if (types.at(id) != types.at(op.operands[0])) fail("GraphIR unary source types disagree");
          if (kind != "!" && (value.getType().isInteger(1) || (kind == "-" && unsignedType(types.at(id)))))
            fail("invalid GraphIR unary operand type");
          if (kind == "+") result = value;
          else if (kind == "!" && value.getType().isInteger(1))
            result = builder.create<arith::XOrIOp>(loc, value, constant("bool", "boolean", "true", loc));
          else if (kind == "-" && isa<FloatType>(value.getType())) result = builder.create<arith::NegFOp>(loc, value);
          else if (kind == "-") result = builder.create<arith::SubIOp>(loc,
              constant(types.at(id), "integer", "0", loc), value);
          else fail("unsupported GraphIR unary operator");
          break;
        }
        case graphir::CallableOperationKind::Call: {
          auto found = functions.find(op.name);
          if (found == functions.end()) fail("scalar call has no declared procedure");
          SmallVector<Value> arguments;
          for (std::size_t arg = 0; arg < op.operands.size(); ++arg) arguments.push_back(operand(arg));
          const auto declaration = std::find_if(graphs.begin(), graphs.end(),
              [&](const auto *item) { return item->name() == op.name; });
          std::size_t runtimeIndex = 0;
          for (const auto &param : (*declaration)->parameters()) if (!param.context) {
            if (runtimeIndex >= op.operands.size() || types.at(op.operands[runtimeIndex++]) != param.type)
              fail("GraphIR call argument source types disagree");
          }
          if (runtimeIndex != arguments.size() || types.at(id) != required((*declaration)->attributes(), "return_type"))
            fail("GraphIR call source signature mismatch");
          auto call = builder.create<func::CallOp>(loc, op.name, found->second.getResultTypes(), arguments);
          if (call.getNumResults()) result = call.getResult(0);
          break;
        }
        default: fail("unsupported executable GraphIR operation in scalar CFG");
        }
        if (result) {
          if (result.getType() != scalar(types.at(id))) fail("scalar operation result disagrees with its GraphIR type");
          values.emplace(id, result);
        }
      }
      const auto &term = block.terminator;
      current = term.location;
      auto loc = location(current);
      if (term.kind == graphir::ControlFlowTerminatorKind::Branch)
        builder.create<cf::BranchOp>(loc, blocks.at(term.successor.value()));
      else if (term.kind == graphir::ControlFlowTerminatorKind::ConditionalBranch)
        builder.create<cf::CondBranchOp>(loc, values.at(term.operands.at(0)),
            blocks.at(term.successor.value()), ValueRange{}, blocks.at(term.falseSuccessor.value()), ValueRange{});
      else {
        SmallVector<Value> results;
        for (auto id : term.operands) {
          if (types.at(id) != required(graph.attributes(), "return_type")) fail("GraphIR return source types disagree");
          results.push_back(values.at(id));
        }
        builder.create<func::ReturnOp>(loc, results);
      }
    }
  }

public:
  ScalarCFGBuilder(const graphir::Module &source, ArrayRef<const graphir::CallableGraph *> graphs,
                   MLIRContext &context, unsigned indexBits)
      : source(source), graphs(graphs), context(context), indexBits(indexBits), builder(&context) {
    context.getOrLoadDialect<arith::ArithDialect>();
    context.getOrLoadDialect<cf::ControlFlowDialect>();
    context.getOrLoadDialect<func::FuncDialect>();
    context.getOrLoadDialect<memref::MemRefDialect>();
    output = ModuleOp::create(location({}));
  }
  FailureOr<OwningOpRef<ModuleOp>> run() {
    try {
      declareFunctions();
      for (const auto *graph : graphs) {
        graphir::walkCallableOperations(graph->body(), [&](const auto &op) {
          current = op.location;
          if (auto type = op.attributes.find("type"); type != op.attributes.end())
            (void)scalar(type->second, true);
        });
        lowerFunction(*graph);
      }
      if (failed(verify(*output))) return failure();
      return std::move(output);
    } catch (const std::exception &error) {
      emitError(location(current)) << "GraphIR scalar CFG lowering: " << error.what();
      return failure();
    }
  }
};
} // namespace
FailureOr<OwningOpRef<ModuleOp>> lowerScalarCFG(const graphir::Module &source,
    ArrayRef<const graphir::CallableGraph *> graphs, MLIRContext &context, unsigned indexBits) {
  return ScalarCFGBuilder(source, graphs, context, indexBits).run();
}
} // namespace metreon::detail
