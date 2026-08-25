#include "metreon/Sema/ContextValidator.h"

#include "metreon/Basic/Diagnostic.h"

#include <string>
#include <unordered_set>

namespace metreon::sema {

void validateContexts(const ast::Module &module) {
  std::unordered_set<std::string> contextIdentifiers;

  for (const ast::ContextDeclaration &context : module.contexts) {
    const std::string contextName = context.name.str();
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
    for (const ast::ResourceStateDeclaration &state : resource.states) {
      if (!stateNames.insert(state.name).second) {
        throw DiagnosticError({"sema.duplicate_resource_state",
                               "duplicate state `" + state.name + "` in `" +
                                   resourceName + "`",
                               state.location});
      }

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
    }
  }
}

} // namespace metreon::sema
