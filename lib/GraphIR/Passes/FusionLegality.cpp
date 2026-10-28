#include "metreon/GraphIR/Passes.h"

#include "metreon/Basic/Diagnostic.h"
#include "PassSupport.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace metreon::graphir::passes {
namespace {

[[noreturn]] void fail(const std::string &code, const std::string &message,
                       SourceLocation location) {
  throw DiagnosticError({"graphir." + code, message, location});
}

void verifyPipeline(const Module &module, const PipelineGraph &pipeline) {
  std::map<std::string, std::size_t> stages;
  std::vector<const CompositionDefinition *> schedules, topologies;
  detail::Dependencies dependencies;
  for (const auto &member : pipeline.members) {
    const auto reference = module.resourceGraphs().at(member.graph)
                               .references().at(member.member).target;
    if (reference.kind == DefinitionKind::Stage) {
      stages.emplace(module.definitionName(reference), reference.index);
      dependencies[reference.index];
    } else if (reference.kind == DefinitionKind::Schedule) {
      schedules.push_back(&module.compositionDefinitions().at(reference.index));
    } else if (reference.kind == DefinitionKind::Topology) {
      topologies.push_back(&module.compositionDefinitions().at(reference.index));
    }
  }
  if (schedules.empty()) return;
  const auto endpointStage = [&](const std::string &endpoint) -> std::optional<std::size_t> {
    const auto found = stages.find(endpoint.substr(0, endpoint.find('.')));
    return found == stages.end() ? std::nullopt : std::optional{found->second};
  };
  for (const auto *topology : topologies) {
    for (const auto &connection : topology->records) {
      if (connection.kind != "connection") continue;
      const auto source = endpointStage(connection.attributes.at("source"));
      const auto target = endpointStage(connection.attributes.at("target"));
      if (source && target) dependencies.at(*source).insert(*target);
    }
  }
  for (const auto &[name, caller] : stages) {
    static_cast<void>(name);
    std::function<void(const std::vector<CallableOperation> &)> calls;
    calls = [&](const auto &body) {
      for (const auto &operation : body) {
        const auto kind = operation.attributes.find("callee_kind");
        if (operation.kind == CallableOperationKind::Call &&
            kind != operation.attributes.end() && kind->second == "stage") {
          const auto callee = stages.find(operation.name);
          if (callee == stages.end())
            fail("fusion_unknown_dependency", "fused stage calls a stage outside its pipeline", operation.location);
          dependencies.at(callee->second).insert(caller);
        }
        calls(operation.body);
      }
    };
    calls(module.stageGraphs().at(caller).body());
  }

  for (const auto *schedule : schedules) {
    std::map<std::size_t, std::size_t> owners, positions;
    std::size_t nextGroup = 0;
    bool hasFusion = false;
    for (const auto &worker : schedule->records) {
      if (worker.kind != "fused_kernel") continue;
      hasFusion = true;
      if (worker.references.empty())
        fail("empty_fusion", "fused worker requires at least one stage", worker.location);
      const auto group = nextGroup++;
      ContextMetadataRef context;
      for (std::size_t position = 0; position < worker.references.size(); ++position) {
        const auto reference = worker.references[position];
        if (reference.kind != DefinitionKind::Stage || !dependencies.contains(reference.index))
          fail("fusion_unknown_stage", "fused worker refers to a stage outside its pipeline", worker.location);
        if (!owners.emplace(reference.index, group).second)
          fail("duplicate_fused_stage", "stage appears in more than one fusion", worker.location);
        positions.emplace(reference.index, position);
        const auto &stageContext = module.stageGraphs().at(reference.index).context();
        if (!stageContext) continue;
        module.requireContextUse(stageContext, ContextUse::MetadataAttachment);
        if (context && !context->hasSameIdentity(*stageContext))
          fail("fusion_context_mismatch", "fused stages require different nominal execution contexts", worker.location);
        context = stageContext;
      }
    }
    if (!hasFusion) continue;
    if (detail::hasCycle(dependencies))
      fail("cyclic_fusion", "fusion requires acyclic topology and stage-call dependencies", schedule->location);
    for (const auto &[stage, targets] : dependencies) {
      static_cast<void>(targets);
      if (!owners.contains(stage)) owners.emplace(stage, nextGroup++);
    }
    detail::Dependencies contracted;
    for (const auto &[source, targets] : dependencies) {
      const auto sourceGroup = owners.at(source);
      contracted[sourceGroup];
      for (const auto target : targets) {
        const auto targetGroup = owners.at(target);
        if (sourceGroup != targetGroup) contracted[sourceGroup].insert(targetGroup);
        else if (positions.at(source) >= positions.at(target))
          fail("fusion_order", "fusion stage order reverses a dependency", schedule->location);
      }
    }
    if (detail::hasCycle(contracted))
      fail("fusion_dependency_cycle", "fusion introduces a cyclic dependency through another worker or stage", schedule->location);
  }
}

} // namespace

void FusionLegalityPass::run(const Module &module) const {
  for (const auto &pipeline : module.pipelineGraphs()) verifyPipeline(module, pipeline);
}

} // namespace metreon::graphir::passes
