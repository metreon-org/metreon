#include "metreon/GraphIR/ControlFlow.h"

#include <stdexcept>
#include <utility>

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
    return std::move(graph_);
  }

private:
  ControlFlowBlockId addBlock(const std::vector<CallableValueId> &scopes,
                             SourceLocation location) {
    const auto id = graph_.blocks.size();
    graph_.blocks.push_back({id, scopes, {}, {}, location});
    return id;
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
        throw std::logic_error("GraphIR operation follows a callable return");
      }
      switch (operation.kind) {
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
        graph_.blocks[*current].operations.push_back(operation.id);
        break;
      }
    }
    return current;
  }

  CallableControlFlow graph_;
};

} // namespace

CallableControlFlow buildControlFlow(const CallableGraph &callable) {
  return ControlFlowBuilder{}.build(callable);
}

} // namespace metreon::graphir
