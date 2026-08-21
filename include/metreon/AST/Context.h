#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace metreon::ast {

struct QualifiedName {
  std::vector<std::string> components;
  SourceLocation location;

  std::string str() const {
    std::ostringstream output;
    for (std::size_t index = 0; index < components.size(); ++index) {
      if (index != 0) {
        output << "::";
      }
      output << components[index];
    }
    return output.str();
  }
};

struct TypeReference {
  QualifiedName name;
  std::vector<TypeReference> arguments;
  SourceLocation location;

  std::string str() const {
    std::ostringstream output;
    output << name.str();
    if (!arguments.empty()) {
      output << '<';
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        output << arguments[index].str();
      }
      output << '>';
    }
    return output.str();
  }
};

struct ContextParameter {
  std::string name;
  TypeReference constraint;
  SourceLocation location;
};

struct GrantDeclaration {
  TypeReference capability;
  SourceLocation location;
};

struct ContextDeclaration {
  QualifiedName name;
  std::vector<ContextParameter> parameters;
  std::vector<GrantDeclaration> grants;
  SourceLocation location;
};

struct Module {
  std::vector<ContextDeclaration> contexts;
};

} // namespace metreon::ast
