#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace metreon::graphir::passes::detail {

using Dependencies = std::map<std::size_t, std::set<std::size_t>>;

inline std::set<std::size_t>
reachableFrom(const Dependencies &graph, std::vector<std::size_t> pending) {
  std::set<std::size_t> reached;
  while (!pending.empty()) {
    const auto node = pending.back();
    pending.pop_back();
    if (!reached.insert(node).second) continue;
    const auto found = graph.find(node);
    if (found == graph.end())
      throw std::logic_error("GraphIR dependency refers to an unknown node");
    pending.insert(pending.end(), found->second.begin(), found->second.end());
  }
  return reached;
}

inline bool hasCycle(const Dependencies &graph) {
  std::map<std::size_t, std::size_t> incoming;
  for (const auto &[node, targets] : graph) {
    incoming.try_emplace(node, 0);
    for (const auto target : targets) ++incoming[target];
  }
  std::vector<std::size_t> ready;
  for (const auto &[node, count] : incoming)
    if (count == 0) ready.push_back(node);
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto node = ready.back();
    ready.pop_back();
    ++visited;
    for (const auto target : graph.at(node))
      if (--incoming.at(target) == 0) ready.push_back(target);
  }
  return visited != graph.size();
}

} // namespace metreon::graphir::passes::detail
