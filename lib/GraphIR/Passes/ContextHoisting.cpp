#include "metreon/GraphIR/Passes.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace metreon::graphir::passes {
namespace {

void hoistBody(CallableGraph &callable, std::vector<CallableOperation> &body) {
  for (auto &operation : body)
    if (operation.kind == CallableOperationKind::Block)
      hoistBody(callable, operation.body);
  std::vector<CallableOperation> result;
  for (std::size_t index = 0; index < body.size();) {
    const auto &first = body[index];
    if (first.kind != CallableOperationKind::Call || !first.context) {
      result.push_back(std::move(body[index++]));
      continue;
    }
    std::size_t last = index;
    for (std::size_t next = index + 1; next < body.size(); ++next) {
      const auto &operation = body[next];
      // Scalar setup between calls stays in exactly the same execution order.
      if ((operation.kind == CallableOperationKind::Literal ||
           operation.kind == CallableOperationKind::Variable) && !operation.context)
        continue;
      if (operation.kind != CallableOperationKind::Call || !operation.context ||
          !operation.context->hasSameIdentity(*first.context))
        break;
      last = next;
    }
    if (last == index) {
      result.push_back(std::move(body[index++]));
      continue;
    }
    auto region = callable.makeOperation(CallableOperationKind::ContextRegion, "", first.location);
    region.context = first.context;
    for (; index <= last; ++index) {
      // The region now supplies this evidence. Value IDs, operands, effects,
      // await markers and source locations remain on the original operations.
      body[index].context.reset();
      region.body.push_back(std::move(body[index]));
    }
    result.push_back(std::move(region));
  }
  body = std::move(result);
}

void hoistCallables(std::vector<CallableGraph> &callables) {
  for (auto &callable : callables) {
    auto body = callable.body();
    hoistBody(callable, body);
    callable.setBody(std::move(body));
  }
}

} // namespace

void ContextHoistingPass::run(Module &module) const {
  hoistCallables(module.kernelGraphs());
  hoistCallables(module.procedureGraphs());
  hoistCallables(module.stageGraphs());
  for (auto &definition : module.compositionDefinitions()) {
    if (definition.kind != DefinitionKind::Schedule) continue;
    for (auto &worker : definition.records) {
      if (worker.kind != "fused_kernel") continue;
      worker.contextRegions.clear();
      for (std::size_t index = 0; index < worker.references.size();) {
        const auto first = worker.references[index];
        if (first.kind != DefinitionKind::Stage)
          throw std::logic_error("GraphIR fused worker must refer to stages");
        const auto &stage = module.stageGraphs().at(first.index);
        if (!stage.context()) {
          ++index;
          continue;
        }
        StageContextRegion region{stage.context(), {first}, stage.location()};
        ++index;
        while (index < worker.references.size()) {
          const auto next = worker.references[index];
          if (next.kind != DefinitionKind::Stage) break;
          const auto &context = module.stageGraphs().at(next.index).context();
          if (!context || !context->hasSameIdentity(*region.context)) break;
          region.stages.push_back(next);
          ++index;
        }
        if (region.stages.size() > 1) worker.contextRegions.push_back(std::move(region));
      }
    }
  }
}

} // namespace metreon::graphir::passes
