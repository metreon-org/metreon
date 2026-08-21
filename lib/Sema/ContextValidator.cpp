#include "metreon/Sema/ContextValidator.h"

#include "metreon/Basic/Diagnostic.h"

#include <string>
#include <unordered_set>

namespace metreon::sema {

void validateContexts(const ast::Module &module) {
  std::unordered_set<std::string> contextNames;

  for (const ast::ContextDeclaration &context : module.contexts) {
    const std::string contextName = context.name.str();
    if (!contextNames.insert(contextName).second) {
      throw DiagnosticError({"sema.duplicate_context",
                             "duplicate context declaration `" + contextName +
                                 "`",
                             context.location});
    }

    std::unordered_set<std::string> parameterNames;
    for (const ast::ContextParameter &parameter : context.parameters) {
      if (!parameterNames.insert(parameter.name).second) {
        throw DiagnosticError(
            {"sema.duplicate_context_variable",
             "duplicate context variable `" + parameter.name + "` in `" +
                 contextName + "`",
             parameter.location});
      }
    }

    std::unordered_set<std::string> grantNames;
    for (const ast::GrantDeclaration &grant : context.grants) {
      const std::string grantName = grant.capability.str();
      if (!grantNames.insert(grantName).second) {
        throw DiagnosticError({"sema.duplicate_grant",
                               "duplicate grant `" + grantName + "` in `" +
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

} // namespace metreon::sema
