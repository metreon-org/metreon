#include "metreon/GraphIR/Passes.h"

#include "PassSupport.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace metreon::graphir::passes {

void DeadTransitionEliminationPass::run(Module &module) const {
  std::map<std::string, std::set<std::string>> liveTransitions;
  for (auto &graph : module.resourceGraphs()) {
    // Graphs supplied without entry information have unknown reachability.
    if (graph.entryStates().empty()) continue;
    detail::Dependencies dependencies;
    for (const auto &state : graph.states()) dependencies[state.id];
    for (const auto &edge : graph.transitions())
      dependencies.at(edge.source).insert(edge.target);
    const auto reachable = detail::reachableFrom(dependencies, graph.entryStates());
    std::erase_if(graph.transitions(), [&](const auto &edge) {
      return !reachable.contains(edge.source) || !reachable.contains(edge.target);
    });
    std::erase_if(graph.states(), [&](const auto &state) {
      return !reachable.contains(state.id);
    });
    auto &names = liveTransitions[graph.name()];
    const auto record = [&](const ResourceTransitionEdge &edge) {
      if (const auto name = edge.attributes.find("name"); name != edge.attributes.end())
        names.insert(name->second);
    };
    for (const auto &edge : graph.transitions()) record(edge);
    for (const auto &state : graph.states())
      for (const auto &edge : state.internalTransitions) record(edge);
  }
  for (auto &resourceTemplate : module.resourceContextTemplates()) {
    const auto found = liveTransitions.find(resourceTemplate.resourceName());
    if (found == liveTransitions.end()) continue;
    for (auto &specialization : resourceTemplate.specializations())
      std::erase_if(specialization.checks, [&](const auto &check) {
        return !found->second.contains(check.transitionName);
      });
  }

  std::vector<std::size_t> removed;
  for (const auto &protocol : module.protocolGraphs()) {
    if (protocol.entryStates.empty()) continue;
    detail::Dependencies dependencies;
    for (const auto &member : protocol.members) {
      const auto reference = module.resourceGraphs().at(member.graph)
                                 .references().at(member.member).target;
      if (reference.kind == DefinitionKind::ProtocolState)
        dependencies[reference.index];
    }
    for (auto &[index, successors] : dependencies) {
      const auto &state = module.compositionDefinitions().at(index);
      for (const auto &message : state.records)
        if (message.kind == "message")
          for (const auto target : message.references)
            if (target.kind == DefinitionKind::ProtocolState)
              successors.insert(target.index);
    }
    std::vector<std::size_t> entries;
    for (const auto entry : protocol.entryStates) entries.push_back(entry.index);
    const auto reachable = detail::reachableFrom(dependencies, std::move(entries));
    for (const auto &[index, successors] : dependencies) {
      static_cast<void>(successors);
      if (!reachable.contains(index)) removed.push_back(index);
    }
  }
  // Compact definitions and every associated numeric reference together.
  module.eraseCompositionDefinitions(removed);
}

} // namespace metreon::graphir::passes
