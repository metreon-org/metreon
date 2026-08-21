#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace metreon::graphir {

using NodeId = std::size_t;

enum class NodeKind {
  Context,
  ContextVariable,
  Grant,
  ExternalContext,
};

enum class EdgeKind {
  Binds,
  Grants,
  Argument,
  Transition,
};

struct Node {
  NodeId id = 0;
  NodeKind kind = NodeKind::Context;
  std::string name;
  std::map<std::string, std::string> attributes;
  SourceLocation location;
};

struct Edge {
  NodeId source = 0;
  NodeId target = 0;
  EdgeKind kind = EdgeKind::Grants;
  std::map<std::string, std::string> attributes;
};

class Module {
public:
  explicit Module(std::string sourceName) : sourceName_(std::move(sourceName)) {}

  NodeId addNode(NodeKind kind, std::string name,
                 std::map<std::string, std::string> attributes,
                 SourceLocation location);
  void addEdge(NodeId source, NodeId target, EdgeKind kind,
               std::map<std::string, std::string> attributes = {});

  const std::vector<Node> &nodes() const noexcept { return nodes_; }
  const std::vector<Edge> &edges() const noexcept { return edges_; }
  const std::string &sourceName() const noexcept { return sourceName_; }

  std::string print() const;

private:
  std::string sourceName_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
};

const char *nodeKindName(NodeKind kind);
const char *edgeKindName(EdgeKind kind);

} // namespace metreon::graphir
