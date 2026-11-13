#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Composition.h"
#include "metreon/Sema/CompositionValidator.h"

#include "metreon/Basic/Diagnostic.h"
#include "metreon/Sema/ContextValidator.h"
#include "metreon/Sema/CallableValidator.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
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

const char *literalKindName(ast::LiteralKind kind) {
  switch (kind) {
  case ast::LiteralKind::Integer:
    return "integer";
  case ast::LiteralKind::Floating:
    return "floating";
  case ast::LiteralKind::Boolean:
    return "boolean";
  case ast::LiteralKind::Infinity:
    return "infinity";
  }
  return "unknown";
}

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
    // Resource validation defines the first declared state as the entry.
    if (!resource.states.empty()) graph.addEntryState(states.at(resource.states.front().name));

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

class CallableLowering {
public:
  CallableLowering(CallableGraph &graph, const sema::CallableAnalysis &analysis,
                   const ContextMap &contexts)
      : graph_(graph), analysis_(analysis), contexts_(contexts) {}

  void lower(const ast::CallableDeclaration &callable, bool isKernel) {
    std::function<void(const ast::Statement &)> assignments = [&](const auto &statement) {
      if (statement.kind == ast::StatementKind::Assignment) assigned_.insert(statement.variable.name);
      for (const auto &nested : statement.statements) assignments(nested);
      for (const auto &nested : statement.alternative) assignments(nested);
    };
    assignments(callable.body);
    const auto *context = analysis_.contexts.at(&callable);
    const auto metadata = context ? contexts_.at(context->identifier) : ContextMetadataRef{};
    graph_.setSignature({{"return_type", callable.resultType.str()},
                         {"effects", joinTypeReferences(callable.effects)},
                         {"where", joinAllowsClauses(callable.allowsClauses)},
                         {"execution_context", context ? context->identifier : "unspecified"},
                         {"execution_domain", context ? context->name.components.front()
                                                      : isKernel ? "Gpu" : "context_independent"}},
                        metadata);
    scopes_.emplace_back();
    std::vector<CallableOperation> prefix;
    for (const auto &parameter : callable.parameters) {
      auto id = graph_.addParameter(parameter.name, parameter.type.str(),
                                    parameter.isContextEvidence ? metadata : ContextMetadataRef{},
                                    parameter.location);
      if (id) {
        const bool slot = mutableLocal(parameter.name, parameter.type);
        auto binding = *id;
        if (slot) {
          auto local = localOperation(parameter.name, parameter.type.str(), parameter.location, true);
          local.operands = {*id};
          binding = local.id;
          prefix.push_back(std::move(local));
        }
        scopes_.back().emplace(parameter.name, Binding{binding, slot});
      }
    }
    auto body = lowerBlock(callable.body, false);
    prefix.insert(prefix.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
    graph_.setBody(std::move(prefix));
  }

private:
  struct Binding { CallableValueId id; bool slot; };
  bool mutableLocal(const std::string &name, const ast::ValueType &type) const {
    return assigned_.contains(name) && type.ownership == ast::OwnershipQualifier::None &&
        type.reference.arguments.empty();
  }
  Binding binding(const std::string &name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
      if (const auto found = scope->find(name); found != scope->end()) return found->second;
    throw std::logic_error("validated local is missing during lowering");
  }
  CallableOperation localOperation(const std::string &name, const std::string &type,
                                   SourceLocation location, bool slot) {
    auto op = graph_.makeOperation(CallableOperationKind::Variable, name, location);
    op.attributes = {{"type", type}, {"initialized", "true"},
        {"mutability", "mutable"}, {"storage", "automatic"}};
    if (slot) op.attributes.emplace("mutable_local", "true");
    return op;
  }
  CallableValueId emit(CallableOperationKind kind, const std::string &type,
      std::vector<CallableValueId> operands, std::vector<CallableOperation> &body,
      SourceLocation location, const std::string &operation = {}) {
    auto op = graph_.makeOperation(kind, "", location);
    op.attributes.emplace("type", type);
    if (!operation.empty()) op.attributes.emplace("operator", operation);
    op.operands = std::move(operands);
    const auto id = op.id;
    body.push_back(std::move(op));
    return id;
  }
  std::optional<CallableValueId> lowerExpression(
      const ast::Expression &expression, std::vector<CallableOperation> &body) {
    const auto &info = analysis_.expressions.at(&expression);
    if (expression.kind == ast::ExpressionKind::ContextReference) {
      return std::nullopt;
    }
    if (expression.kind == ast::ExpressionKind::Reference) {
      auto local = binding(expression.name.str());
      return local.slot ? emit(CallableOperationKind::Load, info.type.str(), {local.id}, body, expression.location) : local.id;
    }
    if (expression.kind == ast::ExpressionKind::Binary && (expression.op == "&&" || expression.op == "||")) {
      auto result = localOperation("$logical", "bool", expression.location, true);
      result.attributes.emplace("initializer.kind", "boolean");
      result.attributes.emplace("initializer.value", expression.op == "||" ? "true" : "false");
      const auto slot = result.id;
      body.push_back(std::move(result));
      auto control = graph_.makeOperation(CallableOperationKind::If, "", expression.location);
      control.attributes.emplace("origin", "short_circuit");
      control.operands = {lowerExpression(expression.arguments[0], control.condition).value()};
      auto &rhs = expression.op == "&&" ? control.body : control.alternative;
      auto value = lowerExpression(expression.arguments[1], rhs).value();
      emit(CallableOperationKind::Assign, "void", {slot, value}, rhs, expression.location);
      body.push_back(std::move(control));
      return emit(CallableOperationKind::Load, "bool", {slot}, body, expression.location);
    }
    std::vector<CallableValueId> operands;
    for (const auto &argument : expression.arguments) {
      const auto id = lowerExpression(argument, body);
      if (id) {
        operands.push_back(*id);
      }
    }
    const bool isCall = expression.kind == ast::ExpressionKind::Call;
    const bool arithmetic = expression.kind == ast::ExpressionKind::Binary || expression.kind == ast::ExpressionKind::Unary;
    auto operation = graph_.makeOperation(isCall ? CallableOperationKind::Call
        : expression.kind == ast::ExpressionKind::Binary ? CallableOperationKind::Binary
        : expression.kind == ast::ExpressionKind::Unary ? CallableOperationKind::Unary : CallableOperationKind::Literal,
                                          isCall ? (info.calleeName.empty() ? expression.name.str() : info.calleeName) : "",
                                          expression.location);
    operation.attributes.emplace("type", info.type.str());
    operation.operands = std::move(operands);
    if (isCall) {
      operation.attributes.emplace("callee_kind", info.calleeKind);
      operation.attributes.emplace("effects", joinTypeReferences(info.effects));
      operation.attributes.emplace("await", info.isAwait ? "true" : "false");
      if (info.context) {
        operation.context = contexts_.at(info.context->identifier);
      }
    } else if (arithmetic) {
      operation.attributes.emplace("operator", expression.op);
    } else {
      operation.attributes.emplace("kind", literalKindName(expression.literal.kind));
      operation.attributes.emplace("value", expression.literal.value);
    }
    const auto id = operation.id;
    body.push_back(std::move(operation));
    return info.type.reference.name.str() == "void" ? std::nullopt
                                                   : std::optional<CallableValueId>{id};
  }

  CallableOperation nestedBlock(const ast::Statement &block) {
    auto op = graph_.makeOperation(CallableOperationKind::Block, "", block.location);
    op.body = lowerBlock(block, true);
    return op;
  }

  CallableOperation control(const ast::Statement &statement) {
    const bool isIf = statement.kind == ast::StatementKind::If;
    auto kind = isIf ? CallableOperationKind::If : statement.kind == ast::StatementKind::Loop ? CallableOperationKind::Loop
        : statement.kind == ast::StatementKind::For ? CallableOperationKind::For
        : statement.kind == ast::StatementKind::While ? CallableOperationKind::While : CallableOperationKind::Until;
    auto op = graph_.makeOperation(kind, "", statement.location);
    if (!isIf) op.loopContext = contexts_.at(analysis_.loopContexts.at(&statement)->identifier);
    if (kind == CallableOperationKind::Until) op.attributes.emplace("stop_value", statement.stopValue ? "true" : "false");
    if (kind == CallableOperationKind::For) {
      scopes_.emplace_back();
      const auto type = statement.variable.type.str();
      const auto &iterable = *statement.expression;
      CallableValueId element;
      if (iterable.kind == ast::ExpressionKind::Range) {
        op.attributes.emplace("iteration_kind", "range");
        auto begin = lowerExpression(iterable.arguments[0], op.initialization).value();
        auto end = lowerExpression(iterable.arguments[1], op.initialization).value();
        auto index = localOperation("$range_index", type, statement.location, true);
        index.operands = {begin};
        const auto slot = index.id;
        op.initialization.push_back(std::move(index));
        auto current = emit(CallableOperationKind::Load, type, {slot}, op.condition, statement.location);
        op.operands = {emit(CallableOperationKind::Binary, "bool", {current, end}, op.condition, statement.location, "<")};
        element = emit(CallableOperationKind::Load, type, {slot}, op.body, statement.location);
        auto previous = emit(CallableOperationKind::Load, type, {slot}, op.step, statement.location);
        auto one = graph_.makeOperation(CallableOperationKind::Literal, "", statement.location);
        one.attributes = {{"type", type}, {"kind", "integer"}, {"value", "1"}};
        const auto oneId = one.id;
        op.step.push_back(std::move(one));
        auto next = emit(CallableOperationKind::Binary, type, {previous, oneId}, op.step, statement.location, "+");
        emit(CallableOperationKind::Assign, "void", {slot, next}, op.step, statement.location);
      } else {
        op.attributes.emplace("iteration_kind", "view");
        auto container = lowerExpression(iterable, op.initialization).value();
        auto iterator = emit(CallableOperationKind::IteratorInit, "iterator<" + type + ">", {container}, op.initialization, statement.location);
        op.operands = {emit(CallableOperationKind::IteratorHasNext, "bool", {iterator}, op.condition, statement.location)};
        element = emit(CallableOperationKind::IteratorValue, type, {iterator}, op.body, statement.location);
        emit(CallableOperationKind::IteratorAdvance, "void", {iterator}, op.step, statement.location);
      }
      auto variable = localOperation(statement.variable.name, type, statement.location, false);
      variable.attributes["mutability"] = "const";
      variable.operands = {element};
      scopes_.back().emplace(statement.variable.name, Binding{variable.id, false});
      op.body.push_back(std::move(variable));
    } else if (statement.expression) {
      op.operands = {lowerExpression(*statement.expression, op.condition).value()};
    }
    op.body.push_back(nestedBlock(statement.statements.at(0)));
    if (isIf && !statement.alternative.empty()) {
      const auto &alternative = statement.alternative.front();
      op.alternative.push_back(alternative.kind == ast::StatementKind::If ? control(alternative) : nestedBlock(alternative));
    }
    if (kind == CallableOperationKind::For) scopes_.pop_back();
    return op;
  }

  std::vector<CallableOperation> lowerBlock(const ast::Statement &block, bool nested) {
    if (nested) {
      scopes_.emplace_back();
    }
    std::vector<CallableOperation> body;
    for (const auto &statement : block.statements) {
      if (statement.kind == ast::StatementKind::If || statement.kind == ast::StatementKind::Loop ||
          statement.kind == ast::StatementKind::For || statement.kind == ast::StatementKind::While ||
          statement.kind == ast::StatementKind::Until) {
        body.push_back(control(statement));
        continue;
      }
      if (statement.kind == ast::StatementKind::Break || statement.kind == ast::StatementKind::Continue) {
        body.push_back(graph_.makeOperation(statement.kind == ast::StatementKind::Break
            ? CallableOperationKind::Break : CallableOperationKind::Continue, "", statement.location));
        continue;
      }
      if (statement.kind == ast::StatementKind::Assignment) {
        auto value = lowerExpression(*statement.expression, body).value();
        const auto target = binding(statement.variable.name);
        if (!target.slot) throw std::logic_error("assignment requires a mutable local slot");
        emit(CallableOperationKind::Assign, "void", {target.id, value}, body, statement.location);
        continue;
      }
      if (statement.kind == ast::StatementKind::Call) {
        lowerExpression(*statement.expression, body);
        continue;
      }
      if (statement.kind == ast::StatementKind::Block) {
        auto operation = graph_.makeOperation(CallableOperationKind::Block, "", statement.location);
        operation.body = lowerBlock(statement, true);
        body.push_back(std::move(operation));
        continue;
      }
      if (statement.kind == ast::StatementKind::Return) {
        std::vector<CallableValueId> operands;
        if (statement.expression) {
          operands.push_back(lowerExpression(*statement.expression, body).value());
        }
        auto operation = graph_.makeOperation(CallableOperationKind::Return, "", statement.location);
        operation.operands = std::move(operands);
        body.push_back(std::move(operation));
        continue;
      }
      const auto &variable = statement.variable;
      std::vector<CallableValueId> operands;
      if (variable.initializer && variable.initializer->kind != ast::ExpressionKind::Literal) {
        operands.push_back(lowerExpression(*variable.initializer, body).value());
      }
      auto operation = graph_.makeOperation(CallableOperationKind::Variable,
                                            variable.name, variable.location);
      operation.attributes = {
          {"initialized", variable.initializer ? "true" : "false"},
          {"mutability", variable.isConstant ? "const" : "mutable"},
          {"storage", "automatic"}, {"type", variable.type.str()}};
      operation.operands = std::move(operands);
      if (variable.initializer && variable.initializer->kind == ast::ExpressionKind::Literal) {
        operation.attributes.emplace("initializer.kind", literalKindName(variable.initializer->literal.kind));
        operation.attributes.emplace("initializer.value", variable.initializer->literal.value);
      }
      const bool slot = !variable.isConstant && mutableLocal(variable.name, variable.type);
      if (slot) operation.attributes.emplace("mutable_local", "true");
      scopes_.back().emplace(variable.name, Binding{operation.id, slot});
      body.push_back(std::move(operation));
    }
    if (nested) {
      scopes_.pop_back();
    }
    return body;
  }

  CallableGraph &graph_;
  const sema::CallableAnalysis &analysis_;
  const ContextMap &contexts_;
  std::vector<std::unordered_map<std::string, Binding>> scopes_;
  std::unordered_set<std::string> assigned_;
};

void lowerCallables(const ast::Module &sourceModule, Module &module,
                    const sema::CallableAnalysis &analysis,
                    const ContextMap &contexts) {
  for (const auto &kernel : sourceModule.kernels) {
    auto &graph = module.addKernelGraph(kernel.name.str(), kernel.location);
    CallableLowering(graph, analysis, contexts).lower(kernel, true);
  }
  for (const auto &procedure : sourceModule.procedures) {
    auto &graph = module.addProcedureGraph(procedure.name.str(), procedure.location);
    CallableLowering(graph, analysis, contexts).lower(procedure, false);
  }
  for (const auto &pipeline : sourceModule.pipelines) {
    for (const auto &stage : pipeline.stages) {
      auto &graph = module.addStageGraph(stage.name.str(), stage.location);
      CallableLowering(graph, analysis, contexts).lower(stage, false);
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

// Add typed entries while structured types and resolved expressions are
// available, in addition to each resource's validated initial state.
void lowerEntryStates(const ast::Module &source, Module &module,
                      const sema::CallableAnalysis &analysis) {
  std::map<std::string, std::pair<std::size_t, ResourceNodeId>> resources;
  std::map<std::string, std::pair<std::size_t, DefinitionReference>> protocols;
  for (std::size_t index = 0; index < module.resourceGraphs().size(); ++index) {
    const auto &graph = module.resourceGraphs()[index];
    for (const auto &state : graph.states())
      resources.emplace(graph.name() + "::" + state.name, std::pair{index, state.id});
  }
  for (std::size_t index = 0; index < module.protocolGraphs().size(); ++index) {
    for (const auto &member : module.protocolGraphs()[index].members) {
      const auto target = module.resourceGraphs()[member.graph].references()[member.member].target;
      protocols.emplace(module.definitionName(target), std::pair{index, target});
    }
  }
  const auto protocolEntry = [&](const std::string &name) {
    const auto found = protocols.find(name);
    if (found == protocols.end()) return;
    const auto [index, reference] = found->second;
    auto &entries = module.protocolGraphs()[index].entryStates;
    if (std::none_of(entries.begin(), entries.end(), [&](const auto &entry) {
          return entry.kind == reference.kind && entry.index == reference.index;
        }))
      entries.push_back(reference);
  };
  std::function<void(const ast::TypeReference &)> type;
  type = [&](const auto &reference) {
    const auto name = reference.name.str();
    if (const auto found = resources.find(name); found != resources.end()) {
      const auto [graph, state] = found->second;
      module.resourceGraphs()[graph].addEntryState(state);
    }
    protocolEntry(name);
    if (reference.name.components.size() > 1 && !reference.arguments.empty()) {
      auto owner = reference.name.components;
      owner.pop_back();
      protocolEntry(ast::QualifiedName{owner, reference.location}.str() + "::" +
                    reference.arguments.front().name.str());
    }
    for (const auto &argument : reference.arguments) type(argument);
  };
  std::function<void(const ast::Expression &)> expression;
  expression = [&](const auto &value) {
    if (const auto found = analysis.expressions.find(&value); found != analysis.expressions.end())
      type(found->second.type.reference);
    for (const auto &argument : value.arguments) expression(argument);
  };
  std::function<void(const ast::Statement &)> statement;
  statement = [&](const auto &value) {
    if (value.kind == ast::StatementKind::Variable) {
      type(value.variable.type.reference);
      if (value.variable.initializer) expression(*value.variable.initializer);
    }
    if (value.expression) expression(*value.expression);
    for (const auto &nested : value.statements) statement(nested);
    for (const auto &nested : value.alternative) statement(nested);
  };
  const auto callable = [&](const ast::CallableDeclaration &value) {
    for (const auto &parameter : value.parameters) type(parameter.type.reference);
    type(value.resultType.reference);
    statement(value.body);
  };
  for (const auto &kernel : source.kernels) callable(kernel);
  for (const auto &procedure : source.procedures) callable(procedure);
  for (const auto &resource : source.resources) {
    for (const auto &state : resource.states)
      for (const auto &field : state.fields) type(field.type.reference);
    for (const auto &transition : resource.transitions)
      for (const auto &parameter : transition.parameters) type(parameter.type.reference);
  }
  for (const auto &protocol : source.protocols)
    for (const auto &state : protocol.states)
      for (const auto &message : state.messages)
        for (const auto &field : message.payload) type(field.type.reference);
  for (const auto &pipeline : source.pipelines) {
    for (const auto &parameter : pipeline.parameters) type(parameter.type.reference);
    for (const auto &state : pipeline.states) type(state.type.reference);
    for (const auto &stage : pipeline.stages) callable(stage);
    for (const auto &edge : pipeline.edges) {
      for (const auto &property : edge.properties)
        if (property.name == "initial")
          protocolEntry(edge.type.arguments.front().name.str() + "::" + property.str());
    }
  }
}

} // namespace

Module lowerModule(const ast::Module &sourceModule, std::string sourceName) {
  sema::validateContexts(sourceModule);
  sema::validateResources(sourceModule);
  sema::validateCompositions(sourceModule);
  const auto callableAnalysis = sema::validateCallables(sourceModule);

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

  lowerCallables(sourceModule, graph, callableAnalysis, contextsByIdentifier);
  lowerResources(sourceModule, graph);
  lowerCompositions(sourceModule, graph);
  lowerResourceContextTemplates(sourceModule, graph, contextsByIdentifier);
  lowerEntryStates(sourceModule, graph, callableAnalysis);

  return graph;
}

Module lowerContexts(const ast::Module &sourceModule, std::string sourceName) {
  return lowerModule(sourceModule, std::move(sourceName));
}

} // namespace metreon::graphir
