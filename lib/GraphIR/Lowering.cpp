#include "metreon/GraphIR/Lowering.h"

#include "metreon/Sema/ContextValidator.h"

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace metreon::graphir {

namespace {

using NodeMap = std::unordered_map<std::string, NodeId>;

bool isContextVariableReference(const ast::TypeReference &reference,
                                const NodeMap &variables) {
  return reference.arguments.empty() && reference.name.components.size() == 1 &&
         variables.find(reference.name.components.front()) != variables.end();
}

void addArgumentEdges(Module &graph, NodeId grantNode,
                      const ast::TypeReference &reference,
                      const std::string &argumentPath,
                      const NodeMap &variables) {
  if (isContextVariableReference(reference, variables)) {
    graph.addEdge(grantNode, variables.at(reference.name.components.front()),
                  EdgeKind::Argument, {{"path", argumentPath}});
  }

  for (std::size_t index = 0; index < reference.arguments.size(); ++index) {
    const std::string childPath =
        argumentPath.empty() ? std::to_string(index)
                             : argumentPath + "." + std::to_string(index);
    addArgumentEdges(graph, grantNode, reference.arguments[index], childPath,
                     variables);
  }
}

NodeId getTransitionTarget(Module &graph,
                           const ast::TypeReference &targetReference,
                           const NodeMap &contexts, const NodeMap &variables,
                           NodeMap &externalContexts) {
  if (isContextVariableReference(targetReference, variables)) {
    return variables.at(targetReference.name.components.front());
  }

  const std::string contextName = targetReference.name.str();
  const auto declared = contexts.find(contextName);
  if (declared != contexts.end()) {
    return declared->second;
  }

  const std::string externalName = targetReference.str();
  const auto external = externalContexts.find(externalName);
  if (external != externalContexts.end()) {
    return external->second;
  }

  const NodeId node = graph.addNode(
      NodeKind::ExternalContext, externalName,
      {{"resolution", "external"}}, targetReference.location);
  externalContexts.emplace(externalName, node);
  return node;
}

} // namespace

Module lowerContexts(const ast::Module &sourceModule, std::string sourceName) {
  sema::validateContexts(sourceModule);

  Module graph(std::move(sourceName));
  NodeMap contexts;
  NodeMap externalContexts;

  for (const ast::ContextDeclaration &context : sourceModule.contexts) {
    const std::string name = context.name.str();
    const NodeId node = graph.addNode(
        NodeKind::Context, name,
        {{"generic_arity", std::to_string(context.parameters.size())}},
        context.location);
    contexts.emplace(name, node);
  }

  for (const ast::ContextDeclaration &context : sourceModule.contexts) {
    const std::string contextName = context.name.str();
    const NodeId contextNode = contexts.at(contextName);
    NodeMap variables;

    for (const ast::ContextParameter &parameter : context.parameters) {
      const NodeId variableNode = graph.addNode(
          NodeKind::ContextVariable, parameter.name,
          {{"constraint", parameter.constraint.str()}, {"owner", contextName}},
          parameter.location);
      variables.emplace(parameter.name, variableNode);
      graph.addEdge(contextNode, variableNode, EdgeKind::Binds);
    }

    for (const ast::GrantDeclaration &grant : context.grants) {
      const NodeId grantNode = graph.addNode(
          NodeKind::Grant, grant.capability.str(),
          {{"arity", std::to_string(grant.capability.arguments.size())},
           {"capability", grant.capability.name.str()},
           {"owner", contextName}},
          grant.location);
      graph.addEdge(contextNode, grantNode, EdgeKind::Grants);

      for (std::size_t index = 0; index < grant.capability.arguments.size();
           ++index) {
        addArgumentEdges(graph, grantNode, grant.capability.arguments[index],
                         std::to_string(index), variables);
      }

      if (grant.capability.name.str() == "defer" &&
          grant.capability.arguments.size() == 1) {
        const ast::TypeReference &target = grant.capability.arguments.front();
        const NodeId targetNode = getTransitionTarget(
            graph, target, contexts, variables, externalContexts);
        graph.addEdge(grantNode, targetNode, EdgeKind::Transition,
                      {{"target_type", target.str()}});
      }
    }
  }

  return graph;
}

} // namespace metreon::graphir
