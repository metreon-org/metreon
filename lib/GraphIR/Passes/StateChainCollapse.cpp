#include "metreon/GraphIR/Passes.h"

#include "PassSupport.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace metreon::graphir::passes {

void StateChainCollapsePass::run(Module &module) const {
  for (auto &graph : module.resourceGraphs()) {
    detail::Dependencies dependencies;
    std::map<ResourceNodeId, std::vector<std::size_t>> outgoing;
    std::map<ResourceNodeId, std::size_t> incoming, stateIndices;
    for (std::size_t index = 0; index < graph.states().size(); ++index) {
      const auto id = graph.states()[index].id;
      dependencies[id];
      stateIndices.emplace(id, index);
    }
    for (std::size_t index = 0; index < graph.transitions().size(); ++index) {
      const auto &edge = graph.transitions()[index];
      dependencies.at(edge.source).insert(edge.target);
      outgoing[edge.source].push_back(index);
      ++incoming[edge.target];
    }
    const std::set<ResourceNodeId> entries(graph.entryStates().begin(), graph.entryStates().end());
    std::map<ResourceNodeId, ResourceNodeId> successor;
    std::set<ResourceNodeId> hasPredecessor;
    for (const auto &[source, edges] : outgoing) {
      if (edges.size() != 1) continue;
      const auto target = graph.transitions()[edges.front()].target;
      if (incoming[target] != 1 || entries.contains(target) ||
          detail::reachableFrom(dependencies, {target}).contains(source))
        continue;
      successor.emplace(source, target);
      hasPredecessor.insert(target);
    }
    std::map<ResourceNodeId, ResourceNodeId> representative;
    std::set<std::size_t> internalEdges;
    std::vector<ResourceStateNode> states;
    for (const auto &head : graph.states()) {
      if (hasPredecessor.contains(head.id)) continue;
      std::vector<ResourceNodeId> chain{head.id};
      while (successor.contains(chain.back())) chain.push_back(successor.at(chain.back()));
      if (chain.size() == 1) {
        representative.emplace(head.id, head.id);
        states.push_back(head);
        continue;
      }
      ResourceStateNode collapsed = head;
      collapsed.members.clear();
      collapsed.internalTransitions.clear();
      for (std::size_t index = 0; index < chain.size(); ++index) {
        const auto &state = graph.states()[stateIndices.at(chain[index])];
        representative.emplace(state.id, head.id);
        if (state.members.empty()) collapsed.members.push_back(state);
        else collapsed.members.insert(collapsed.members.end(), state.members.begin(), state.members.end());
        collapsed.internalTransitions.insert(collapsed.internalTransitions.end(),
            state.internalTransitions.begin(), state.internalTransitions.end());
        if (index + 1 == chain.size()) continue;
        const auto edgeIndex = outgoing.at(state.id).front();
        auto edge = graph.transitions()[edgeIndex];
        const auto &next = graph.states()[stateIndices.at(chain[index + 1])];
        edge.source = state.members.empty() ? state.id : state.members.back().id;
        edge.target = next.members.empty() ? next.id : next.members.front().id;
        collapsed.internalTransitions.push_back(std::move(edge));
        internalEdges.insert(edgeIndex);
      }
      states.push_back(std::move(collapsed));
    }
    std::vector<ResourceTransitionEdge> edges;
    for (std::size_t index = 0; index < graph.transitions().size(); ++index) {
      if (internalEdges.contains(index)) continue;
      auto edge = graph.transitions()[index];
      edge.source = representative.at(edge.source);
      edge.target = representative.at(edge.target);
      edges.push_back(std::move(edge));
    }
    graph.states() = std::move(states);
    graph.transitions() = std::move(edges);
  }
}

} // namespace metreon::graphir::passes
