#include "metreon/GraphIR/ControlFlow.h"

#include <stdexcept>
#include <utility>
#include <set>
#include <algorithm>

namespace metreon::graphir {

namespace {

class ControlFlowBuilder {
public:
  CallableControlFlow build(const CallableGraph &callable) {
    const auto entry = addBlock({}, callable.location());
    graph_.entry = entry;
    const auto end = appendBody(callable.body(), entry, {});
    if (end) {
      const auto returnType = callable.attributes().find("return_type");
      if (returnType == callable.attributes().end() || returnType->second != "void") {
        throw std::logic_error("GraphIR non-void callable falls through without a return");
      }
      graph_.blocks[*end].terminator = {
          ControlFlowTerminatorKind::Return, std::nullopt, std::nullopt, {},
          callable.location()};
    }
    std::set<ControlFlowBlockId> reachable;
    std::vector<ControlFlowBlockId> pending{graph_.entry};
    while (!pending.empty()) {
      const auto id = pending.back(); pending.pop_back();
      if (!reachable.insert(id).second) continue;
      const auto &term = graph_.blocks.at(id).terminator;
      if (term.successor) pending.push_back(*term.successor);
      if (term.falseSuccessor) pending.push_back(*term.falseSuccessor);
    }
    std::erase_if(graph_.blocks, [&](const auto &block) { return !reachable.contains(block.id); });
    return std::move(graph_);
  }

private:
  ControlFlowBlockId addBlock(const std::vector<CallableValueId> &scopes,
                             SourceLocation location) {
    const auto id = graph_.blocks.size();
    graph_.blocks.push_back({id, scopes, {}, {}, location});
    if (!loops_.empty()) graph_.blocks.back().loopContext = loops_.back().context;
    return id;
  }

  void conditionalBranch(ControlFlowBlockId source, CallableValueId condition,
      ControlFlowBlockId yes, ControlFlowBlockId no, const CallableOperation &origin) {
    graph_.blocks[source].terminator = {ControlFlowTerminatorKind::ConditionalBranch,
        yes, origin.id, {condition}, origin.location, no};
  }

  std::optional<ControlFlowBlockId> conditional(const CallableOperation &op,
      ControlFlowBlockId initial, const std::vector<CallableValueId> &scopes) {
    auto tested = appendBody(op.condition, initial, scopes);
    if (!tested || op.operands.size() != 1) throw std::logic_error("invalid conditional predicate");
    auto yes = addBlock(scopes, op.location), no = addBlock(scopes, op.location);
    graph_.blocks[yes].attributes.emplace("role", "if.then");
    graph_.blocks[no].attributes.emplace("role", "if.else");
    conditionalBranch(*tested, op.operands.front(), yes, no, op);
    auto yesEnd = appendBody(op.body, yes, scopes);
    auto noEnd = appendBody(op.alternative, no, scopes);
    if (!yesEnd && !noEnd) return std::nullopt;
    const auto join = addBlock(scopes, op.location);
    graph_.blocks[join].attributes.emplace("role", "if.join");
    if (yesEnd) branch(*yesEnd, join, op.location);
    if (noEnd) branch(*noEnd, join, op.location);
    return join;
  }

  std::optional<ControlFlowBlockId> loop(const CallableOperation &op,
      ControlFlowBlockId initial, const std::vector<CallableValueId> &outerScopes) {
    if (!op.loopContext) throw std::logic_error("loop has no context metadata");
    auto scopes = outerScopes;
    scopes.push_back(op.id);
    const bool isFor = op.kind == CallableOperationKind::For;
    const bool plain = op.kind == CallableOperationKind::Loop;
    const auto entry = addBlock(scopes, op.location);
    graph_.blocks[entry].loopContext = op.loopContext;
    graph_.blocks[entry].attributes.emplace("role", std::string(callableOperationKindName(op.kind)) + ".init");
    branch(initial, entry, op.location);
    auto initialized = appendBody(op.initialization, entry, scopes);
    if (!initialized) throw std::logic_error("loop initialization cannot terminate control flow");
    const auto header = addBlock(scopes, op.location);
    const auto body = addBlock(scopes, op.location);
    const auto step = isFor ? addBlock(scopes, op.location) : header;
    const auto exit = addBlock(outerScopes, op.location);
    for (auto id : {header, body, step}) {
      graph_.blocks[id].loopContext = op.loopContext;
      graph_.blocks[id].attributes["loop"] = std::to_string(op.id);
    }
    graph_.blocks[header].attributes["role"] = std::string(callableOperationKindName(op.kind)) + ".header";
    graph_.blocks[body].attributes["role"] = std::string(callableOperationKindName(op.kind)) + ".body";
    if (isFor) graph_.blocks[step].attributes["role"] = "for.step";
    graph_.blocks[exit].attributes["role"] = std::string(callableOperationKindName(op.kind)) + ".exit";
    branch(*initialized, header, op.location);
    loops_.push_back({exit, step, op.loopContext, false});
    auto tested = appendBody(op.condition, header, scopes);
    if (!tested) throw std::logic_error("loop predicate cannot terminate control flow");
    if (plain) branch(*tested, body, op.location);
    else {
      if (op.operands.size() != 1) throw std::logic_error("invalid loop predicate");
      const bool stopOnTrue = op.kind == CallableOperationKind::Until && op.attributes.at("stop_value") == "true";
      conditionalBranch(*tested, op.operands.front(), stopOnTrue ? exit : body,
          stopOnTrue ? body : exit, op);
    }
    auto end = appendBody(op.body, body, scopes);
    if (end) branch(*end, step, op.location);
    if (isFor) {
      auto advanced = appendBody(op.step, step, scopes);
      if (advanced) branch(*advanced, header, op.location);
    }
    const bool hasBreak = loops_.back().hasBreak;
    loops_.pop_back();
    if (plain && !hasBreak) return std::nullopt;
    return exit;
  }

  void branch(ControlFlowBlockId source, ControlFlowBlockId target,
              SourceLocation location) {
    graph_.blocks[source].terminator = {
        ControlFlowTerminatorKind::Branch, target, std::nullopt, {}, location};
  }

  std::optional<ControlFlowBlockId>
  appendBody(const std::vector<CallableOperation> &body,
             ControlFlowBlockId initial,
             const std::vector<CallableValueId> &scopes) {
    std::optional<ControlFlowBlockId> current = initial;
    for (const auto &operation : body) {
      if (!current) {
        throw std::logic_error("GraphIR operation follows an unconditional control-flow exit");
      }
      switch (operation.kind) {
      case CallableOperationKind::If:
        current = conditional(operation, *current, scopes);
        break;
      case CallableOperationKind::Loop: case CallableOperationKind::For:
      case CallableOperationKind::While: case CallableOperationKind::Until:
        current = loop(operation, *current, scopes);
        break;
      case CallableOperationKind::Break: case CallableOperationKind::Continue:
        if (loops_.empty()) throw std::logic_error("loop control outside a loop");
        branch(*current, operation.kind == CallableOperationKind::Break
            ? loops_.back().exit : loops_.back().next, operation.location);
        graph_.blocks[*current].terminator.operation = operation.id;
        graph_.blocks[*current].terminator.control = operation.kind == CallableOperationKind::Break ? "break" : "continue";
        if (operation.kind == CallableOperationKind::Break) loops_.back().hasBreak = true;
        current = std::nullopt;
        break;
      case CallableOperationKind::ContextRegion:
        // Evidence regions are transparent to SSA and lexical ownership scopes.
        current = appendBody(operation.body, *current, scopes);
        break;
      case CallableOperationKind::Block: {
        auto nestedScopes = scopes;
        nestedScopes.push_back(operation.id);
        const auto nested = addBlock(nestedScopes, operation.location);
        branch(*current, nested, operation.location);
        const auto nestedEnd = appendBody(operation.body, nested, nestedScopes);
        if (nestedEnd) {
          const auto continuation = addBlock(scopes, operation.location);
          branch(*nestedEnd, continuation, operation.location);
          current = continuation;
        } else {
          current = std::nullopt;
        }
        break;
      }
      case CallableOperationKind::Return:
        graph_.blocks[*current].terminator = {
            ControlFlowTerminatorKind::Return, std::nullopt, operation.id,
            operation.operands, operation.location};
        current = std::nullopt;
        break;
      case CallableOperationKind::Literal:
      case CallableOperationKind::Variable:
      case CallableOperationKind::Call:
      case CallableOperationKind::Unary: case CallableOperationKind::Binary:
      case CallableOperationKind::Load: case CallableOperationKind::Assign:
      case CallableOperationKind::IteratorInit: case CallableOperationKind::IteratorHasNext:
      case CallableOperationKind::IteratorValue: case CallableOperationKind::IteratorAdvance:
        graph_.blocks[*current].operations.push_back(operation.id);
        break;
      }
    }
    return current;
  }

  CallableControlFlow graph_;
  struct LoopTargets {
    ControlFlowBlockId exit, next;
    ContextMetadataRef context;
    bool hasBreak;
  };
  std::vector<LoopTargets> loops_;
};

} // namespace

CallableControlFlow buildControlFlow(const CallableGraph &callable) {
  return ControlFlowBuilder{}.build(callable);
}

} // namespace metreon::graphir
