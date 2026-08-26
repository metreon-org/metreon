#include "metreon/GraphIR/Lowering.h"

#include "metreon/Basic/Diagnostic.h"
#include "metreon/Sema/ContextValidator.h"

#include <cstddef>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace metreon::graphir {

namespace {

using NodeMap = std::unordered_map<std::string, NodeId>;
using ContextMap = std::unordered_map<std::string, ContextMetadataRef>;
using ContextTypeMap =
    std::unordered_map<std::string, std::vector<ContextMetadataRef>>;
using ResourceStateMap =
    std::unordered_map<std::string, ResourceNodeId>;

std::string joinTypeReferences(
    const std::vector<ast::TypeReference> &references) {
  std::ostringstream output;
  for (std::size_t index = 0; index < references.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << references[index].str();
  }
  return output.str();
}

std::string joinGenericParameters(
    const std::vector<ast::ContextParameter> &parameters) {
  std::ostringstream output;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << parameters[index].name << ": "
           << parameters[index].constraint.str();
  }
  return output.str();
}

std::string joinTransitionParameters(
    const std::vector<ast::TransitionParameter> &parameters,
    bool contextEvidenceOnly = false) {
  std::ostringstream output;
  bool first = true;
  for (const ast::TransitionParameter &parameter : parameters) {
    if (contextEvidenceOnly && !parameter.isContextEvidence) {
      continue;
    }
    if (!first) {
      output << ", ";
    }
    first = false;
    output << parameter.str();
  }
  return output.str();
}

std::string joinAllowsClauses(
    const std::vector<ast::AllowsClause> &clauses) {
  std::ostringstream output;
  for (std::size_t index = 0; index < clauses.size(); ++index) {
    if (index != 0) {
      output << "; ";
    }
    output << clauses[index].str();
  }
  return output.str();
}

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

EdgeTarget getTransitionTarget(Module &graph,
                               const ast::TypeReference &targetReference,
                               const ContextMap &contextsByIdentifier,
                               const ContextTypeMap &contextsByType,
                               const NodeMap &variables,
                               ContextMap &externalContexts) {
  if (isContextVariableReference(targetReference, variables)) {
    return EdgeTarget{variables.at(targetReference.name.components.front())};
  }

  if (targetReference.arguments.empty() &&
      targetReference.name.components.size() == 1) {
    const auto declaredIdentifier = contextsByIdentifier.find(
        targetReference.name.components.front());
    if (declaredIdentifier != contextsByIdentifier.end()) {
      return EdgeTarget{declaredIdentifier->second};
    }
  }

  const std::string exactContextName = targetReference.str();
  auto declaredType = contextsByType.find(exactContextName);
  if (declaredType == contextsByType.end() &&
      !targetReference.arguments.empty()) {
    // A generic declaration is keyed by its base name, while a concrete
    // declaration is keyed by its full instantiated type. Prefer the exact
    // specialization and fall back to a generic declaration when one exists.
    declaredType = contextsByType.find(targetReference.name.str());
  }
  if (declaredType != contextsByType.end()) {
    if (declaredType->second.size() == 1) {
      return EdgeTarget{declaredType->second.front()};
    }
    throw DiagnosticError(
        {"sema.ambiguous_context_target",
         "context type `" + exactContextName +
             "` has multiple identifiers; use a context identifier as the "
             "transition target",
         targetReference.location});
  }

  const std::string externalName = targetReference.str();
  const auto external = externalContexts.find(externalName);
  if (external != externalContexts.end()) {
    return EdgeTarget{external->second};
  }

  ContextMetadataRef metadata = graph.addContextMetadata(
      externalName, "", targetReference.arguments.size(),
      ContextResolution::External, targetReference.location);
  externalContexts.emplace(externalName, metadata);
  return EdgeTarget{std::move(metadata)};
}

void lowerResources(const ast::Module &sourceModule, Module &module) {
  for (const ast::ResourceDeclaration &resource : sourceModule.resources) {
    ResourceGraph &graph = module.addResourceGraph(
        resource.name.str(),
        {{"generic_arity", std::to_string(resource.parameters.size())},
         {"generic_parameters", joinGenericParameters(resource.parameters)}},
        resource.location);
    ResourceStateMap states;
    std::unordered_map<std::string, std::string> accumulatorScopes;
    for (const ast::ResourceAccumulatorDeclaration &accumulator :
         resource.accumulators) {
      accumulatorScopes.emplace(accumulator.stateName, accumulator.scope.str());
    }

    for (const ast::ResourceStateDeclaration &state : resource.states) {
      std::map<std::string, std::string> attributes = {
          {"field_count", std::to_string(state.fields.size())}};
      const auto accumulator = accumulatorScopes.find(state.name);
      if (accumulator != accumulatorScopes.end()) {
        attributes.emplace("accumulator", accumulator->second);
      }
      for (std::size_t index = 0; index < state.fields.size(); ++index) {
        const std::string prefix = "field." + std::to_string(index) + ".";
        attributes.emplace(prefix + "name", state.fields[index].name);
        attributes.emplace(prefix + "type", state.fields[index].type.str());
      }
      const ResourceNodeId stateNode = graph.addState(
          state.name, std::move(attributes), state.location);
      states.emplace(state.name, stateNode);
    }

    for (const ast::ResourceTransitionDeclaration &transition :
         resource.transitions) {
      const ast::TransitionParameter *selfParameter = nullptr;
      for (const ast::TransitionParameter &parameter : transition.parameters) {
        if (parameter.name == "self") {
          selfParameter = &parameter;
          break;
        }
      }

      // Resource validation guarantees `self` and both state names exist.
      const std::string sourceState =
          selfParameter->type.reference.name.str();
      const std::string targetState = transition.resultState.name.str();
      std::map<std::string, std::string> attributes = {
          {"await", transition.isAwait ? "true" : "false"},
          {"context_evidence",
           joinTransitionParameters(transition.parameters, true)},
          {"effects", joinTypeReferences(transition.effects)},
          {"generic_parameters",
           joinGenericParameters(transition.genericParameters)},
          {"name", transition.name},
          {"parameters", joinTransitionParameters(transition.parameters)},
          {"source_type", selfParameter->type.str()},
          {"target_type", transition.resultState.str()},
          {"where", joinAllowsClauses(transition.allowsClauses)},
      };
      if (transition.isAwait) {
        attributes.emplace(
            "await_condition",
            transition.allowsClauses.empty()
                ? "declared by await modifier"
                : joinAllowsClauses(transition.allowsClauses));
      }

      graph.addTransition(states.at(sourceState), states.at(targetState),
                          std::move(attributes), transition.location);
    }
  }
}

bool contextHasGrant(const Module &module,
                     const ContextMetadataRef &context,
                     const std::string &requiredCapability) {
  for (const Node &node : module.nodes()) {
    if (node.kind == NodeKind::Grant && node.context &&
        node.context->hasSameIdentity(*context) &&
        node.name == requiredCapability) {
      return true;
    }
  }
  return false;
}

bool clauseUsesGenericContext(
    const ast::AllowsClause &clause,
    const ast::ResourceTransitionDeclaration &transition) {
  if (!clause.context.arguments.empty() ||
      clause.context.name.components.size() != 1) {
    return false;
  }
  const std::string &name = clause.context.name.components.front();
  for (const ast::ContextParameter &parameter :
       transition.genericParameters) {
    if (parameter.name == name) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> collectContextTemplateParameters(
    const ast::ResourceDeclaration &resource) {
  std::vector<std::string> parameters;
  std::unordered_set<std::string> seen;
  for (const ast::ResourceTransitionDeclaration &transition :
       resource.transitions) {
    for (const ast::AllowsClause &clause : transition.allowsClauses) {
      if (!clauseUsesGenericContext(clause, transition)) {
        continue;
      }
      const std::string name = clause.context.name.components.front();
      if (seen.insert(name).second) {
        parameters.push_back(name);
      }
    }
  }
  return parameters;
}

std::vector<std::string> collectRequiredCapabilities(
    const ast::ResourceTransitionDeclaration &transition,
    const std::string &contextParameter) {
  std::vector<std::string> capabilities;
  std::unordered_set<std::string> seen;
  for (const ast::AllowsClause &clause : transition.allowsClauses) {
    if (!clauseUsesGenericContext(clause, transition) ||
        clause.context.name.components.front() != contextParameter) {
      continue;
    }
    for (const ast::TypeReference &capability : clause.capabilities) {
      const std::string canonicalCapability = capability.str();
      if (seen.insert(canonicalCapability).second) {
        capabilities.push_back(canonicalCapability);
      }
    }
  }
  return capabilities;
}

void lowerResourceContextTemplates(const ast::Module &sourceModule,
                                   Module &module,
                                   const ContextMap &contextsByIdentifier) {
  for (const ast::ResourceDeclaration &resource : sourceModule.resources) {
    for (const std::string &contextParameter :
         collectContextTemplateParameters(resource)) {
      ResourceContextTemplate &resourceTemplate =
          module.addResourceContextTemplate(resource.name.str(),
                                            contextParameter,
                                            resource.location);

      // Only locally declared contexts are candidates. External context
      // metadata is intentionally unavailable for a local grant proof.
      for (const ast::ContextDeclaration &context : sourceModule.contexts) {
        const ContextMetadataRef &contextMetadata =
            contextsByIdentifier.at(context.identifier);
        ContextTemplateSpecialization &specialization =
            resourceTemplate.addSpecialization(contextMetadata);

        for (const ast::ResourceTransitionDeclaration &transition :
             resource.transitions) {
          std::vector<std::string> requiredCapabilities =
              collectRequiredCapabilities(transition, contextParameter);
          if (requiredCapabilities.empty()) {
            continue;
          }

          std::vector<std::string> missingCapabilities;
          for (const std::string &requiredCapability :
               requiredCapabilities) {
            // Grant node names are canonical TypeReference strings, so this
            // exact comparison includes all nested generic arguments.
            if (!contextHasGrant(module, contextMetadata,
                                 requiredCapability)) {
              missingCapabilities.push_back(requiredCapability);
            }
          }

          specialization.checks.push_back(ResourceTypeCheck{
              transition.name, std::move(requiredCapabilities),
              std::move(missingCapabilities), transition.location});
        }
      }
    }
  }
}

} // namespace

Module lowerModule(const ast::Module &sourceModule, std::string sourceName) {
  sema::validateContexts(sourceModule);
  sema::validateResources(sourceModule);

  Module graph(std::move(sourceName));
  ContextMap contextsByIdentifier;
  ContextTypeMap contextsByType;
  ContextMap externalContexts;

  for (const ast::ContextDeclaration &context : sourceModule.contexts) {
    const std::string name = context.typeName();
    ContextMetadataRef metadata = graph.addContextMetadata(
        name, context.identifier, context.genericArity(),
        ContextResolution::Declared, context.location);
    contextsByIdentifier.emplace(context.identifier, metadata);
    contextsByType[name].push_back(std::move(metadata));
  }

  for (const ast::ContextDeclaration &context : sourceModule.contexts) {
    const ContextMetadataRef &contextMetadata =
        contextsByIdentifier.at(context.identifier);
    NodeMap variables;

    for (const ast::ContextParameter &parameter : context.parameters) {
      const NodeId variableNode = graph.addNode(
          NodeKind::ContextVariable, parameter.name,
          {{"constraint", parameter.constraint.str()}}, contextMetadata,
          parameter.location);
      variables.emplace(parameter.name, variableNode);
    }

    for (const ast::GrantDeclaration &grant : context.grants) {
      const NodeId grantNode = graph.addNode(
          NodeKind::Grant, grant.capability.str(),
          {{"arity", std::to_string(grant.capability.arguments.size())},
           {"capability", grant.capability.name.str()}},
          contextMetadata, grant.location);

      for (std::size_t index = 0; index < grant.capability.arguments.size();
           ++index) {
        addArgumentEdges(graph, grantNode, grant.capability.arguments[index],
                         std::to_string(index), variables);
      }

      if (grant.capability.name.str() == "defer" &&
          grant.capability.arguments.size() == 1) {
        const ast::TypeReference &target = grant.capability.arguments.front();
        EdgeTarget targetEntity = getTransitionTarget(
            graph, target, contextsByIdentifier, contextsByType, variables,
            externalContexts);
        if (const NodeId *targetNode = std::get_if<NodeId>(&targetEntity)) {
          graph.addEdge(grantNode, *targetNode, EdgeKind::Transition,
                        {{"target_type", target.str()}});
        } else {
          graph.addContextEdge(
              grantNode, std::get<ContextMetadataRef>(std::move(targetEntity)),
              EdgeKind::Transition, {{"target_type", target.str()}});
        }
      }
    }
  }

  lowerResources(sourceModule, graph);
  lowerResourceContextTemplates(sourceModule, graph, contextsByIdentifier);

  return graph;
}

Module lowerContexts(const ast::Module &sourceModule, std::string sourceName) {
  return lowerModule(sourceModule, std::move(sourceName));
}

} // namespace metreon::graphir
