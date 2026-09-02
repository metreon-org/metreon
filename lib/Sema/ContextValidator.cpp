#include "metreon/Sema/ContextValidator.h"

#include "metreon/Basic/Diagnostic.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace metreon::sema {

namespace {

bool isSimpleTypeNamed(const ast::TypeReference &type,
                       const std::string &name) {
  return type.arguments.empty() && type.name.components.size() == 1 &&
         type.name.components.front() == name;
}

bool hasOwnedEventEvidence(const ast::ResourceStateDeclaration &state) {
  for (const ast::ResourceField &field : state.fields) {
    if (field.type.ownership == ast::OwnershipQualifier::Own &&
        field.type.reference.name.str() == "event" &&
        !field.type.reference.arguments.empty()) {
      return true;
    }
  }
  return false;
}

bool hasAwaitCapabilityEvidence(
    const ast::ResourceTransitionDeclaration &transition) {
  for (const ast::TransitionParameter &evidence : transition.parameters) {
    if (!evidence.isContextEvidence || evidence.name != "cx" ||
        evidence.type.ownership != ast::OwnershipQualifier::None ||
        !evidence.type.reference.arguments.empty() ||
        evidence.type.reference.name.components.size() != 1) {
      continue;
    }

    const std::string &contextName =
        evidence.type.reference.name.components.front();
    bool isContextParameter = false;
    for (const ast::ContextParameter &parameter :
         transition.genericParameters) {
      if (parameter.name == contextName &&
          isSimpleTypeNamed(parameter.constraint, "Context")) {
        isContextParameter = true;
        break;
      }
    }
    if (!isContextParameter) {
      continue;
    }

    for (const ast::AllowsClause &clause : transition.allowsClauses) {
      if (!isSimpleTypeNamed(clause.context, contextName)) {
        continue;
      }
      for (const ast::TypeReference &capability : clause.capabilities) {
        if (isSimpleTypeNamed(capability, "gpu_await")) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

void validateContexts(const ast::Module &module) {
  std::unordered_set<std::string> contextIdentifiers;

  for (const ast::ContextDeclaration &context : module.contexts) {
    const std::string contextName = context.typeName();
    if (!contextIdentifiers.insert(context.identifier).second) {
      throw DiagnosticError(
          {"sema.duplicate_context_identifier",
           "duplicate context identifier `" + context.identifier + "`",
           context.identifierLocation});
    }

    std::unordered_set<std::string> parameterNames;
    for (const ast::ContextParameter &parameter : context.parameters) {
      if (!parameterNames.insert(parameter.name).second) {
        throw DiagnosticError(
            {"sema.duplicate_context_variable",
             "duplicate context variable `" + parameter.name + "` in `" +
                 context.identifier + "` of type `" + contextName + "`",
             parameter.location});
      }
    }

    std::unordered_set<std::string> grantNames;
    for (const ast::GrantDeclaration &grant : context.grants) {
      const std::string grantName = grant.capability.str();
      if (!grantNames.insert(grantName).second) {
        throw DiagnosticError({"sema.duplicate_grant",
                               "duplicate grant `" + grantName + "` in `" +
                                   context.identifier + "` of type `" +
                                   contextName + "`",
                               grant.location});
      }

      if (grant.capability.name.str() == "defer" &&
          grant.capability.arguments.size() != 1) {
        throw DiagnosticError(
            {"sema.defer_arity",
             "`defer` requires exactly one target context argument",
             grant.location});
      }
    }
  }
}

void validateResources(const ast::Module &module) {
  std::unordered_set<std::string> resourceNames;

  for (const ast::ResourceDeclaration &resource : module.resources) {
    const std::string resourceName = resource.name.str();
    if (!resourceNames.insert(resourceName).second) {
      throw DiagnosticError({"sema.duplicate_resource",
                             "duplicate resource declaration `" +
                                 resourceName + "`",
                             resource.location});
    }

    std::unordered_set<std::string> resourceParameterNames;
    for (const ast::ContextParameter &parameter : resource.parameters) {
      if (!resourceParameterNames.insert(parameter.name).second) {
        throw DiagnosticError(
            {"sema.duplicate_resource_variable",
             "duplicate resource variable `" + parameter.name + "` in `" +
                 resourceName + "`",
             parameter.location});
      }
    }

    std::unordered_set<std::string> stateNames;
    std::unordered_map<std::string, SourceLocation> stateLocations;
    std::unordered_map<std::string, const ast::ResourceStateDeclaration *>
        statesByName;
    for (const ast::ResourceStateDeclaration &state : resource.states) {
      if (!stateNames.insert(state.name).second) {
        throw DiagnosticError({"sema.duplicate_resource_state",
                               "duplicate state `" + state.name + "` in `" +
                                   resourceName + "`",
                               state.location});
      }
      stateLocations.emplace(state.name, state.location);
      statesByName.emplace(state.name, &state);

      std::unordered_set<std::string> fieldNames;
      for (const ast::ResourceField &field : state.fields) {
        if (!fieldNames.insert(field.name).second) {
          throw DiagnosticError(
              {"sema.duplicate_resource_field",
               "duplicate field `" + field.name + "` in state `" +
                   state.name + "`",
               field.location});
        }
      }
    }

    const std::unordered_set<std::string> allowedAccumulatorScopes = {
        "thread", "warp", "block", "device", "cluster", "host::pinned"};
    std::unordered_set<std::string> accumulatedStates;
    for (const ast::ResourceAccumulatorDeclaration &accumulator :
         resource.accumulators) {
      const auto stateLocation = stateLocations.find(accumulator.stateName);
      if (stateLocation == stateLocations.end()) {
        throw DiagnosticError(
            {"sema.unknown_accumulator_state",
             "accumulator refers to unknown state `" + accumulator.stateName +
                 "` in `" + resourceName + "`",
             accumulator.stateLocation});
      }
      if (stateLocation->second.offset >= accumulator.location.offset) {
        throw DiagnosticError(
            {"sema.accumulator_before_state",
             "state `" + accumulator.stateName +
                 "` must be declared before its accumulator",
             accumulator.stateLocation});
      }
      if (!accumulatedStates.insert(accumulator.stateName).second) {
        throw DiagnosticError(
            {"sema.duplicate_state_accumulator",
             "state `" + accumulator.stateName +
                 "` has more than one accumulator in `" + resourceName + "`",
             accumulator.stateLocation});
      }

      const std::string scope = accumulator.scope.str();
      if (allowedAccumulatorScopes.find(scope) ==
          allowedAccumulatorScopes.end()) {
        throw DiagnosticError(
            {"sema.invalid_accumulator_scope",
             "invalid accumulator scope `" + scope +
                 "`; expected thread, warp, block, device, cluster, or "
                 "host::pinned",
             accumulator.scope.location});
      }
    }

    std::unordered_map<std::string, std::vector<std::string>>
        transitionsByState;
    for (const ast::ResourceTransitionDeclaration &transition :
         resource.transitions) {
      std::unordered_set<std::string> genericParameterNames;
      for (const ast::ContextParameter &parameter :
           transition.genericParameters) {
        if (!genericParameterNames.insert(parameter.name).second) {
          throw DiagnosticError(
              {"sema.duplicate_transition_variable",
               "duplicate transition variable `" + parameter.name +
                   "` in transition `" + transition.name + "`",
               parameter.location});
        }
      }

      std::unordered_set<std::string> parameterNames;
      const ast::TransitionParameter *selfParameter = nullptr;
      for (const ast::TransitionParameter &parameter : transition.parameters) {
        if (!parameterNames.insert(parameter.name).second) {
          throw DiagnosticError(
              {"sema.duplicate_transition_parameter",
               "duplicate parameter `" + parameter.name +
                   "` in transition `" + transition.name + "`",
               parameter.location});
        }
        if (parameter.name == "self") {
          selfParameter = &parameter;
        }
      }

      if (selfParameter == nullptr) {
        throw DiagnosticError(
            {"sema.missing_transition_self",
             "resource transition `" + transition.name +
                 "` requires a `self` state parameter",
             transition.location});
      }
      if (selfParameter->type.ownership != ast::OwnershipQualifier::Own) {
        throw DiagnosticError(
            {"sema.transition_self_not_owned",
             "resource transition `" + transition.name +
                 "` must consume `self` with `own`",
             selfParameter->location});
      }

      const std::string sourceState =
          selfParameter->type.reference.name.str();
      if (stateNames.find(sourceState) == stateNames.end()) {
        throw DiagnosticError(
            {"sema.unknown_transition_source",
             "transition `" + transition.name + "` refers to unknown state `" +
                 sourceState + "`",
             selfParameter->location});
      }

      const std::string targetState = transition.resultState.name.str();
      if (stateNames.find(targetState) == stateNames.end()) {
        throw DiagnosticError(
            {"sema.unknown_transition_target",
             "transition `" + transition.name + "` refers to unknown state `" +
                 targetState + "`",
             transition.resultState.location});
      }

      transitionsByState[sourceState].push_back(targetState);

      if (transition.isAwait) {
        const ast::ResourceStateDeclaration &source =
            *statesByName.at(sourceState);
        if (!hasOwnedEventEvidence(source)) {
          throw DiagnosticError(
              {"sema.await_missing_event_evidence",
               "await transition `" + transition.name + "` in `" +
                   resourceName + "` requires its source state `" +
                   sourceState + "` to contain an `own event<...>` field",
               transition.location});
        }
        if (!hasAwaitCapabilityEvidence(transition)) {
          throw DiagnosticError(
              {"sema.await_missing_capability_evidence",
               "await transition `" + transition.name + "` in `" +
                   resourceName +
                   "` requires `@cx: C` evidence for a `C: Context` "
                   "parameter and `where C allows {gpu_await}`",
               transition.location});
        }
      }
    }

    if (!resource.states.empty()) {
      std::unordered_set<std::string> reachableStates;
      std::vector<std::string> worklist = {resource.states.front().name};
      reachableStates.insert(resource.states.front().name);

      for (std::size_t index = 0; index < worklist.size(); ++index) {
        const auto outgoing = transitionsByState.find(worklist[index]);
        if (outgoing == transitionsByState.end()) {
          continue;
        }
        for (const std::string &target : outgoing->second) {
          if (reachableStates.insert(target).second) {
            worklist.push_back(target);
          }
        }
      }

      for (const ast::ResourceStateDeclaration &state : resource.states) {
        if (reachableStates.find(state.name) == reachableStates.end()) {
          throw DiagnosticError(
              {"sema.unreachable_resource_state",
               "state `" + state.name + "` in resource `" + resourceName +
                   "` is unreachable from entry state `" +
                   resource.states.front().name + "`",
               state.location});
        }
      }
    }
  }
}

void validateKernels(const ast::Module &module) {
  std::unordered_set<std::string> kernelNames;

  for (const ast::KernelDeclaration &kernel : module.kernels) {
    const std::string kernelName = kernel.name.str();
    if (!kernelNames.insert(kernelName).second) {
      throw DiagnosticError({"sema.duplicate_kernel",
                             "duplicate kernel declaration `" + kernelName +
                                 "`",
                             kernel.location});
    }

    std::unordered_set<std::string> variableNames;
    for (const ast::VariableDeclaration &variable : kernel.variables) {
      if (!variableNames.insert(variable.name).second) {
        throw DiagnosticError(
            {"sema.duplicate_kernel_variable",
             "duplicate variable `" + variable.name + "` in kernel `" +
                 kernelName + "`",
             variable.location});
      }
      if (variable.isConstant && !variable.initializer.has_value()) {
        throw DiagnosticError(
            {"sema.uninitialized_const_variable",
             "const variable `" + variable.name + "` in kernel `" +
                 kernelName + "` requires an initializer",
             variable.location});
      }
    }
  }
}

} // namespace metreon::sema
