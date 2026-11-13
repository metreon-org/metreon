#pragma once

#include "metreon/GraphIR/Graph.h"

#include <optional>
#include <vector>

namespace metreon::graphir {

using ControlFlowBlockId = std::size_t;

enum class ControlFlowTerminatorKind { Branch, Return, ConditionalBranch };

struct ControlFlowTerminator {
  ControlFlowTerminatorKind kind = ControlFlowTerminatorKind::Return;
  std::optional<ControlFlowBlockId> successor;
  // An absent operation denotes a synthetic branch or implicit void return.
  std::optional<CallableValueId> operation;
  std::vector<CallableValueId> operands;
  SourceLocation location;
  std::optional<ControlFlowBlockId> falseSuccessor{};
  std::string control{};
};

struct ControlFlowBlock {
  ControlFlowBlockId id = 0;
  // Lexical block operation IDs, ordered from outermost to innermost.
  std::vector<CallableValueId> scopes;
  // References to existing operations, not copies or additional executions.
  std::vector<CallableValueId> operations;
  ControlFlowTerminator terminator;
  SourceLocation location;
  std::map<std::string, std::string> attributes{};
  ContextMetadataRef loopContext{};
};

struct CallableControlFlow {
  ControlFlowBlockId entry = 0;
  std::vector<ControlFlowBlock> blocks;
};

// Project structured callable bodies into basic blocks, including conditional
// edges, loop backedges and nearest-loop break/continue destinations.
// The projection is rebuilt from the body so mutations cannot leave stale CFGs.
CallableControlFlow buildControlFlow(const CallableGraph &callable);

} // namespace metreon::graphir
