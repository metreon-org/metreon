#pragma once

#include "metreon/GraphIR/Graph.h"

namespace metreon::graphir::passes {

// Passes can be invoked independently on validated, lowered GraphIR. The
// default pipeline verifies fusion before performing structural rewrites.
class DeadTransitionEliminationPass {
public:
  void run(Module &module) const;
};

class StateChainCollapsePass {
public:
  void run(Module &module) const;
};

class ContextHoistingPass {
public:
  void run(Module &module) const;
};

class FusionLegalityPass {
public:
  void run(const Module &module) const;
};

void runDefaultPipeline(Module &module);

} // namespace metreon::graphir::passes
