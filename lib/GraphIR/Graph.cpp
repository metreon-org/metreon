#include "metreon/GraphIR/Graph.h"

#include <sstream>
#include <stdexcept>
#include <string>

namespace metreon::graphir {

namespace {

std::string escapeString(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }
  return escaped;
}

void printAttributes(std::ostringstream &output,
                     const std::map<std::string, std::string> &attributes) {
  output << " {";
  bool first = true;
  for (const auto &[key, value] : attributes) {
    if (!first) {
      output << ", ";
    }
    first = false;
    output << key << " = \"" << escapeString(value) << '"';
  }
  output << '}';
}

} // namespace

const char *nodeKindName(NodeKind kind) {
  switch (kind) {
  case NodeKind::Context:
    return "context";
  case NodeKind::ContextVariable:
    return "context_var";
  case NodeKind::Grant:
    return "grant";
  case NodeKind::ExternalContext:
    return "external_context";
  }
  return "unknown";
}

const char *edgeKindName(EdgeKind kind) {
  switch (kind) {
  case EdgeKind::Binds:
    return "binds";
  case EdgeKind::Grants:
    return "grants";
  case EdgeKind::Argument:
    return "argument";
  case EdgeKind::Transition:
    return "transition";
  }
  return "unknown";
}

NodeId Module::addNode(NodeKind kind, std::string name,
                       std::map<std::string, std::string> attributes,
                       SourceLocation location) {
  const NodeId id = nodes_.size();
  nodes_.push_back(
      Node{id, kind, std::move(name), std::move(attributes), location});
  return id;
}

void Module::addEdge(NodeId source, NodeId target, EdgeKind kind,
                     std::map<std::string, std::string> attributes) {
  if (source >= nodes_.size() || target >= nodes_.size()) {
    throw std::logic_error("GraphIR edge refers to an unknown node");
  }
  edges_.push_back(Edge{source, target, kind, std::move(attributes)});
}

std::string Module::print() const {
  std::ostringstream output;
  output << "graphir.module {\n";
  output << "  graphir.graph @contexts {\n";

  for (const Node &node : nodes_) {
    output << "    %n" << node.id << " = graphir."
           << nodeKindName(node.kind) << " \"" << escapeString(node.name)
           << '"';
    printAttributes(output, node.attributes);
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << node.location.line << ':' << node.location.column << ")\n";
  }

  if (!nodes_.empty() && !edges_.empty()) {
    output << '\n';
  }

  for (const Edge &edge : edges_) {
    output << "    graphir.edge %n" << edge.source << " -> %n" << edge.target;
    std::map<std::string, std::string> attributes = edge.attributes;
    attributes.emplace("kind", edgeKindName(edge.kind));
    printAttributes(output, attributes);
    output << '\n';
  }

  output << "  }\n";
  output << "}\n";
  return output.str();
}

} // namespace metreon::graphir
