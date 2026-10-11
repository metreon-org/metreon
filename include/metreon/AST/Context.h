#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <variant>

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

enum class LiteralKind {
  Integer,
  Floating,
  Boolean,
  Infinity,
};

struct LiteralInitializer {
  LiteralKind kind = LiteralKind::Integer;
  std::string value;
  SourceLocation location;
};

enum class ExpressionKind { Literal, Reference, ContextReference, Call };

struct Expression {
  ExpressionKind kind = ExpressionKind::Literal;
  LiteralInitializer literal;
  QualifiedName name;
  std::vector<Expression> arguments;
  SourceLocation location;
};

struct VariableDeclaration {
  ValueType type;
  std::string name;
  bool isConstant = false;
  std::optional<Expression> initializer;
  SourceLocation location;
};

enum class StatementKind { Variable, Call, Block, Return };

struct Statement {
  StatementKind kind = StatementKind::Block;
  VariableDeclaration variable;
  std::optional<Expression> expression;
  std::vector<Statement> statements;
  SourceLocation location;
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

// Kernels and procedures share a body grammar. Module membership preserves the
// declaration kind; only procedures have an explicit result type in source.
struct CallableDeclaration {
  QualifiedName name;
  std::vector<TransitionParameter> parameters;
  ValueType resultType;
  std::vector<TypeReference> effects;
  std::vector<AllowsClause> allowsClauses;
  Statement body;
  SourceLocation location;
};

using KernelDeclaration = CallableDeclaration;
using ProcedureDeclaration = CallableDeclaration;

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

struct NamedReference {
  std::string name;
  SourceLocation location;
};

struct ProtocolMessage {
  NamedReference sender;
  NamedReference receiver;
  std::string name;
  std::vector<ResourceField> payload;
  NamedReference successor;
  SourceLocation location;
};

struct ProtocolState {
  std::string name;
  bool isFinal = false;
  std::vector<ProtocolMessage> messages;
  SourceLocation location;
};

struct ProtocolDeclaration {
  QualifiedName name;
  std::vector<NamedReference> roles;
  std::vector<ProtocolState> states;
  SourceLocation location;
};

struct CompositionProperty {
  std::string name;
  std::variant<LiteralInitializer, TypeReference> value;
  SourceLocation location;

  std::string str() const {
    if (const auto *literal = std::get_if<LiteralInitializer>(&value)) {
      return literal->value;
    }
    return std::get<TypeReference>(value).str();
  }
};

enum class PipelineDirection { Input, Output };

struct PipelineParameter {
  PipelineDirection direction = PipelineDirection::Input;
  std::string name;
  ValueType type;
  SourceLocation location;
};

struct PipelineState {
  std::string name;
  ValueType type;
  std::vector<CompositionProperty> properties;
  SourceLocation location;
};

struct PipelineEdge {
  std::string name;
  TypeReference type;
  std::vector<CompositionProperty> properties;
  SourceLocation location;
};

struct TopologyEndpoint {
  std::string node;
  std::string port;
  SourceLocation location;

  std::string str() const { return node + (port.empty() ? "" : "." + port); }
};

struct TopologyConnection {
  TopologyEndpoint source;
  TopologyEndpoint target;
  std::optional<NamedReference> edge;
  SourceLocation location;
};

struct PipelineTopology {
  std::vector<TopologyConnection> connections;
  SourceLocation location;
};

struct ScheduleKernel {
  std::string name;
  std::vector<NamedReference> stages;
  std::vector<CompositionProperty> properties;
  SourceLocation location;
};

struct SchedulePlacement {
  NamedReference stage;
  NamedReference worker;
  std::string unit;
  LiteralInitializer first;
  std::optional<LiteralInitializer> last;
  SourceLocation location;
};

struct PipelineSchedule {
  std::string name;
  std::vector<ScheduleKernel> kernels;
  std::vector<SchedulePlacement> placements;
  SourceLocation location;
};

struct PipelineDeclaration {
  QualifiedName name;
  std::vector<PipelineParameter> parameters;
  std::vector<PipelineState> states;
  std::vector<PipelineEdge> edges;
  std::vector<CallableDeclaration> stages;
  std::optional<PipelineTopology> topology;
  std::vector<PipelineSchedule> schedules;
  SourceLocation location;
};

struct Module {
  std::vector<ContextDeclaration> contexts;
  std::vector<ResourceDeclaration> resources;
  std::vector<KernelDeclaration> kernels;
  std::vector<ProcedureDeclaration> procedures;
  std::vector<ProtocolDeclaration> protocols;
  std::vector<PipelineDeclaration> pipelines;
};

} // namespace metreon::ast
