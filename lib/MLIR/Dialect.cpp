#include "metreon/MLIR/Dialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Bytecode/BytecodeImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include <set>
#include <functional>
#include <string>

// LLVM 18 TableGen emits unused parameters in generated builders/verifiers.
// Keep project warnings strict while limiting this suppression to generated code.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "MetreonDialect.cpp.inc"
#define GET_TYPEDEF_CLASSES
#include "MetreonTypes.cpp.inc"
#define GET_OP_CLASSES
#include "MetreonOps.cpp.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace mlir;

namespace metreon {

void MetreonDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "MetreonTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "MetreonOps.cpp.inc"
      >();
}

namespace {
bool application(Attribute value) {
  auto dict = dyn_cast_or_null<DictionaryAttr>(value);
  if (!dict) return false;
  auto name = dict.getAs<StringAttr>("name");
  auto args = dict.getAs<ArrayAttr>("arguments");
  if (!name || name.getValue().empty() || !args) return false;
  return llvm::all_of(args, [](Attribute arg) { return application(arg); });
}

LogicalResult applications(Operation *op, ArrayAttr values, StringRef label,
                           bool canonical = false) {
  std::set<std::string> seen;
  std::string previous;
  for (auto value : values) {
    if (!application(value)) return op->emitOpError() << "invalid " << label;
    std::string key;
    llvm::raw_string_ostream out(key);
    value.print(out);
    if (!seen.insert(key).second || (canonical && !previous.empty() && key < previous))
      return op->emitOpError() << label << " must be unique and canonical";
    previous = key;
  }
  return success();
}

LogicalResult generics(Operation *op, ArrayAttr values) {
  std::set<std::string> seen;
  for (auto value : values) {
    auto dict = dyn_cast<DictionaryAttr>(value);
    auto name = dict ? dict.getAs<StringAttr>("name") : StringAttr{};
    if (!name || !seen.insert(name.str()).second || !application(dict.get("constraint")))
      return op->emitOpError("invalid or duplicate generic parameter");
  }
  return success();
}

bool payloadType(Type type) {
  if (auto owned = dyn_cast<OwnedType>(type))
    return !isa<ResourceType, OwnedType>(owned.getValueType()) && payloadType(owned.getValueType());
  if (auto unresolved = dyn_cast<UnresolvedType>(type)) {
    auto source = unresolved.getSourceType();
    if (!application(source)) return false;
    auto name = source.getAs<StringAttr>("name").getValue();
    // Contexts are evidence, never payloads. Import additionally checks all
    // nominal context declarations and context parameter identities.
    if (name == "Context" || name.starts_with("Host::") ||
        name.starts_with("Gpu::") || source.get("context")) return false;
  }
  return !isa<NoneType>(type);
}

LogicalResult fields(Operation *op, ArrayAttr values, bool named) {
  std::set<std::string> seen;
  for (auto value : values) {
    auto dict = dyn_cast<DictionaryAttr>(value);
    if (!dict) return op->emitOpError("expected field/parameter dictionary");
    auto type = dict.getAs<TypeAttr>("type");
    auto ownership = dict.getAs<StringAttr>("ownership");
    if (!type || !payloadType(type.getValue()) || !ownership ||
        (ownership != "own" && ownership != "none") ||
        !application(dict.get("source_type")))
      return op->emitOpError("invalid payload type, ownership or source type");
    if (named) {
      auto name = dict.getAs<StringAttr>("name");
      if (!name || !seen.insert(name.str()).second)
        return op->emitOpError("missing or duplicate field/parameter name");
    }
  }
  return success();
}

Operation *resolve(Operation *op, SymbolRefAttr ref, SymbolTableCollection &tables) {
  // A parser/inspector can temporarily wrap the source module in another
  // module. Search enclosing symbol tables rather than assuming the outermost
  // operation is the module that owns a fully qualified type reference.
  for (auto *scope = op; scope; scope = scope->getParentOp())
    if (scope->hasTrait<OpTrait::SymbolTable>())
      if (auto *found = tables.lookupSymbolIn(scope, ref)) return found;
  return nullptr;
}

LogicalResult references(Operation *op, SymbolTableCollection &tables) {
  LogicalResult result = success();
  std::function<void(Attribute)> visit;
  std::function<void(Type)> visitType = [&](Type type) {
    if (auto resource = dyn_cast<ResourceType>(type)) {
      auto *state = resolve(op, resource.getState(), tables);
      if (!state || !isa<StateOp>(state)) {
        op->emitOpError("resource type must reference a resource state declaration");
        result = failure();
      }
      visit(resource.getState());
      visit(resource.getArguments());
    } else if (auto owned = dyn_cast<OwnedType>(type)) visitType(owned.getValueType());
    else if (auto unresolved = dyn_cast<UnresolvedType>(type)) visit(unresolved.getSourceType());
    else if (auto function = dyn_cast<FunctionType>(type)) {
      for (auto item : function.getInputs()) visitType(item);
      for (auto item : function.getResults()) visitType(item);
    }
  };
  visit = [&](Attribute value) {
    if (auto ref = dyn_cast<SymbolRefAttr>(value)) {
      if (!resolve(op, ref, tables)) {
        op->emitOpError() << "unresolved symbol " << ref;
        result = failure();
      }
    } else if (auto array = dyn_cast<ArrayAttr>(value)) {
      for (auto item : array) visit(item);
    } else if (auto dict = dyn_cast<DictionaryAttr>(value)) {
      for (auto item : dict) visit(item.getValue());
    } else if (auto type = dyn_cast<TypeAttr>(value)) visitType(type.getValue());
  };
  for (auto attr : op->getAttrs()) visit(attr.getValue());
  return result;
}

LogicalResult signature(Operation *op, FunctionType type, ArrayAttr params, ArrayAttr results) {
  if (params.size() != type.getNumInputs() || results.size() != type.getNumResults())
    return op->emitOpError("signature metadata length mismatch");
  if (failed(fields(op, params, true)) || failed(fields(op, results, false))) return failure();
  for (auto [index, metadata] : llvm::enumerate(params))
    if (cast<DictionaryAttr>(metadata).getAs<TypeAttr>("type").getValue() != type.getInput(index))
      return op->emitOpError("parameter type disagrees with signature");
  for (auto [index, metadata] : llvm::enumerate(results))
    if (cast<DictionaryAttr>(metadata).getAs<TypeAttr>("type").getValue() != type.getResult(index))
      return op->emitOpError("result type disagrees with signature");
  return success();
}

LogicalResult callable(Operation *op, FunctionType type, ArrayAttr params, ArrayAttr results,
                       ArrayAttr effects, StringRef domain) {
  if (domain != "Host" && domain != "Gpu" && domain != "context_independent")
    return op->emitOpError("only host and GPU callable domains are supported");
  if (failed(signature(op, type, params, results))) return failure();
  return applications(op, effects, "effects", true);
}

LogicalResult declarationGroup(Operation *op, Region &body) {
  if (!body.hasOneBlock() || body.front().getNumArguments())
    return op->emitOpError("requires one argument-free declaration block");
  for (auto &child : body.front()) {
    const bool allowed = isa<ProtocolOp>(op) ? isa<ProtocolStateOp>(child)
        : isa<ProtocolStateOp>(op) ? isa<MessageDeclOp>(child)
        : isa<PipelineStateOp, EdgeOp, TopologyOp, ScheduleOp, StageOp>(child);
    if (!allowed)
      return op->emitOpError("contains a non-declaration operation");
  }
  return success();
}
} // namespace

LogicalResult ContextOp::verify() {
  if (!application(getNominalType())) return emitOpError("invalid nominal type application");
  if (failed(generics(*this, getGenericParameters()))) return failure();
  return applications(*this, getGrants(), "grants", true);
}
LogicalResult ContextOp::verifySymbolUses(SymbolTableCollection &tables) {
  return references(*this, tables);
}
LogicalResult ResourceOp::verify() {
  if (failed(generics(*this, getGenericParameters()))) return failure();
  if (!getBody().hasOneBlock() || getBody().front().getNumArguments())
    return emitOpError("requires one argument-free declaration block");
  for (auto &child : getBody().front())
    if (!isa<StateOp, TransitionDeclOp>(child)) return emitOpError("contains a non-resource declaration");
  return success();
}
LogicalResult ResourceOp::verifySymbolUses(SymbolTableCollection &tables) {
  std::set<StringRef> seen;
  for (auto ref : getEntryStates()) {
    auto symbol = cast<FlatSymbolRefAttr>(ref);
    auto *target = tables.lookupSymbolIn(*this, symbol);
    if (!target || !isa<StateOp>(target) || !seen.insert(symbol.getValue()).second)
      return emitOpError("entry states must reference distinct direct state members");
  }
  return success();
}
LogicalResult StateOp::verify() {
  if (auto scope = getAccumulatorScope(); scope && !application(*scope))
    return emitOpError("invalid accumulator scope application");
  return fields(*this, getFields(), true);
}
LogicalResult StateOp::verifySymbolUses(SymbolTableCollection &tables) {
  return references(*this, tables);
}
LogicalResult TransitionDeclOp::verify() {
  auto type = getFunctionType();
  if (failed(signature(*this, type, getParameterAttrs(), getResultAttrs())) ||
      failed(generics(*this, getGenericParameters())) ||
      failed(applications(*this, getEffects(), "effects", true))) return failure();
  if (getSelfIndex() >= type.getNumInputs() || type.getNumResults() != 1 ||
      !isa<ResourceType>(type.getInput(getSelfIndex())) || !isa<ResourceType>(type.getResult(0)))
    return emitOpError("requires consumed self and exactly one resource successor");
  if (cast<DictionaryAttr>(getParameterAttrs()[getSelfIndex()]).getAs<StringAttr>("ownership") != "own")
    return emitOpError("self must be owned");
  return success();
}
LogicalResult TransitionDeclOp::verifyRegions() {
  if (!getBody().empty()) return emitOpError("implementation bodies are not supported by structural import");
  return success();
}
LogicalResult TransitionDeclOp::verifySymbolUses(SymbolTableCollection &tables) {
  auto signature = getFunctionType();
  if (getSelfIndex() >= signature.getNumInputs() || signature.getNumResults() != 1 ||
      !isa<ResourceType>(signature.getInput(getSelfIndex())) ||
      !isa<ResourceType>(signature.getResult(0)))
    return emitOpError("invalid resource transition signature");
  auto parent = (*this)->getParentOfType<ResourceOp>();
  auto *source = tables.lookupSymbolIn(parent, getSourceStateAttr());
  auto *target = tables.lookupSymbolIn(parent, getTargetStateAttr());
  if (!source || !target || !isa<StateOp>(source) || !isa<StateOp>(target))
    return emitOpError("transition endpoints must be sibling states");
  auto selfType = cast<ResourceType>(getFunctionType().getInput(getSelfIndex()));
  auto resultType = cast<ResourceType>(getFunctionType().getResult(0));
  if (resolve(*this, selfType.getState(), tables) != source ||
      resolve(*this, resultType.getState(), tables) != target)
    return emitOpError("signature resource types disagree with transition endpoints");
  return references(*this, tables);
}
LogicalResult TransitionOp::verify() {
  return isa<ResourceType>(getSuccessor().getType()) ? success()
      : emitOpError("successor must be an owned resource state");
}
LogicalResult TransitionOp::verifySymbolUses(SymbolTableCollection &tables) {
  auto *decl = resolve(*this, getCallee(), tables);
  if (!decl || !isa<TransitionDeclOp>(decl)) return emitOpError("callee must be a transition declaration");
  auto signature = cast<TransitionDeclOp>(decl).getFunctionType();
  if (signature.getNumResults() != 1)
    return emitOpError("callee has an invalid transition result signature");
  if (!getTypeArguments().empty())
    return emitOpError("executable generic transition substitution is not implemented");
  if (getArguments().getTypes() != signature.getInputs() ||
      getSuccessor().getType() != signature.getResult(0))
    return emitOpError("transition operands/result disagree with the declaration");
  return references(*this, tables);
}

#define METREON_CALLABLE_VERIFY(NAME) \
  LogicalResult NAME::verify() { \
    return callable(*this, getFunctionType(), getParameterAttrs(), getResultAttrs(), \
                    getEffects(), getExecutionDomain()); \
  } \
  LogicalResult NAME::verifySymbolUses(SymbolTableCollection &tables) { \
    return references(*this, tables); \
  }
METREON_CALLABLE_VERIFY(ProcedureOp)
METREON_CALLABLE_VERIFY(KernelOp)
METREON_CALLABLE_VERIFY(StageOp)
#undef METREON_CALLABLE_VERIFY

#define METREON_GROUP_VERIFY(NAME) \
  LogicalResult NAME::verify() { return declarationGroup(*this, getBody()); } \
  LogicalResult NAME::verifySymbolUses(SymbolTableCollection &tables) { \
    return references(*this, tables); \
  }
METREON_GROUP_VERIFY(ProtocolOp)
METREON_GROUP_VERIFY(PipelineOp)
METREON_GROUP_VERIFY(ProtocolStateOp)
#undef METREON_GROUP_VERIFY

#define METREON_COMPOSITION_VERIFY(NAME) \
  LogicalResult NAME::verify() { \
    for (auto value : getRecords()) { \
      auto record = dyn_cast<DictionaryAttr>(value); \
      if (!record || !record.getAs<StringAttr>("kind") || \
          !record.getAs<DictionaryAttr>("properties") || \
          !record.getAs<ArrayAttr>("references")) \
        return emitOpError("invalid composition record schema"); \
    } \
    return success(); \
  } \
  LogicalResult NAME::verifySymbolUses(SymbolTableCollection &tables) { \
    return references(*this, tables); \
  }
METREON_COMPOSITION_VERIFY(MessageDeclOp)
METREON_COMPOSITION_VERIFY(PipelineStateOp)
METREON_COMPOSITION_VERIFY(EdgeOp)
METREON_COMPOSITION_VERIFY(TopologyOp)
METREON_COMPOSITION_VERIFY(ScheduleOp)
#undef METREON_COMPOSITION_VERIFY

} // namespace metreon
