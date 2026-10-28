#include "metreon/GraphIR/Passes.h"

namespace metreon::graphir::passes {

void runDefaultPipeline(Module &module) {
  FusionLegalityPass{}.run(module);
  DeadTransitionEliminationPass{}.run(module);
  StateChainCollapsePass{}.run(module);
  ContextHoistingPass{}.run(module);
}

} // namespace metreon::graphir::passes
