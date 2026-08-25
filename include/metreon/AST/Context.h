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
  std::vector<TypeReference> arguments;
  std::vector<GrantDeclaration> grants;
  std::string identifier;
  SourceLocation identifierLocation;
  SourceLocation location;

  std::string typeName() const {
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

  std::size_t genericArity() const noexcept {
    return parameters.size() + arguments.size();
  }
};

enum class OwnershipQualifier {
  None,
  Own,
};

struct ValueType {
  OwnershipQualifier ownership = OwnershipQualifier::None;
  TypeReference reference;
  SourceLocation location;

  std::string str() const {
    return ownership == OwnershipQualifier::Own
               ? "own " + reference.str()
               : reference.str();
  }
};

struct ResourceField {
  std::string name;
  ValueType type;
  SourceLocation location;
};

struct ResourceStateDeclaration {
  std::string name;
  std::vector<ResourceField> fields;
  SourceLocation location;
};

struct ResourceAccumulatorDeclaration {
  std::string stateName;
  QualifiedName scope;
  SourceLocation stateLocation;
  SourceLocation location;
};

struct TransitionParameter {
  std::string name;
  ValueType type;
  bool isContextEvidence = false;
  SourceLocation location;

  std::string str() const {
    return std::string(isContextEvidence ? "@" : "") + name + ": " +
           type.str();
  }
};

struct AllowsClause {
  TypeReference context;
  std::vector<TypeReference> capabilities;
  SourceLocation location;

  std::string str() const {
    std::ostringstream output;
    output << context.str() << " allows {";
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << capabilities[index].str();
    }
    output << '}';
    return output.str();
  }
};

struct ResourceTransitionDeclaration {
  std::string name;
  std::vector<ContextParameter> genericParameters;
  std::vector<TransitionParameter> parameters;
  TypeReference resultState;
  std::vector<TypeReference> effects;
  std::vector<AllowsClause> allowsClauses;
  bool isAwait = false;
  SourceLocation location;
};

struct ResourceDeclaration {
  QualifiedName name;
  std::vector<ContextParameter> parameters;
  std::vector<ResourceStateDeclaration> states;
  std::vector<ResourceAccumulatorDeclaration> accumulators;
  std::vector<ResourceTransitionDeclaration> transitions;
  SourceLocation location;
};

struct Module {
  std::vector<ContextDeclaration> contexts;
  std::vector<ResourceDeclaration> resources;
};

} // namespace metreon::ast
