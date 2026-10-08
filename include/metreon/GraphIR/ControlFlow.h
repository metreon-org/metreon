#pragma once

#include "metreon/GraphIR/Graph.h"

#include <optional>
#include <vector>

namespace metreon::graphir {

using ControlFlowBlockId = std::size_t;

enum class ControlFlowTerminatorKind { Branch, Return };

struct ControlFlowTerminator {
  ControlFlowTerminatorKind kind = ControlFlowTerminatorKind::Return;
  std::optional<ControlFlowBlockId> successor;
  // An absent operation denotes a synthetic branch or implicit void return.
  std::optional<CallableValueId> operation;
  std::vector<CallableValueId> operands;
  SourceLocation location;
};

struct ControlFlowBlock {
  ControlFlowBlockId id = 0;
  // Lexical block operation IDs, ordered from outermost to innermost.
  std::vector<CallableValueId> scopes;
  // References to existing operations, not copies or additional executions.
  std::vector<CallableValueId> operations;
  ControlFlowTerminator terminator;
  SourceLocation location;
};

struct CallableControlFlow {
  ControlFlowBlockId entry = 0;
  std::vector<ControlFlowBlock> blocks;
};

// Project the currently supported straight-line callable bodies into basic
// blocks. Scope entry/exit uses branches; any nested return exits the callable.
// The projection is rebuilt from the body so mutations cannot leave stale CFGs.
CallableControlFlow buildControlFlow(const CallableGraph &callable);

} // namespace metreon::graphir
