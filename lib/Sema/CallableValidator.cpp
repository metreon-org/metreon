#include "metreon/Sema/CallableValidator.h"

#include "metreon/Basic/Diagnostic.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace metreon::sema {
namespace {

using Bindings = std::unordered_map<std::string, ast::TypeReference>;

[[noreturn]] void fail(const std::string &code, const std::string &message,
                       SourceLocation location) {
  throw DiagnosticError({"sema." + code, message, location});
}

ast::ValueType simpleType(const std::string &name, SourceLocation location) {
  ast::ValueType type;
  type.location = location;
  type.reference.location = location;
  type.reference.name = {{name}, location};
  return type;
}

bool isVoid(const ast::ValueType &type) {
  return type.reference.name.str() == "void";
}

bool isInteger(const std::string &name) {
  static const std::unordered_set<std::string> names = {
      "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "index"};
  return names.contains(name);
}

bool isFloating(const std::string &name) {
  return name == "f16" || name == "f32" || name == "f64";
}

struct Callable {
  const ast::CallableDeclaration *declaration;
  bool isKernel;
};

struct Transition {
  const ast::ResourceDeclaration *resource;
  const ast::ResourceTransitionDeclaration *declaration;
};

struct Local {
  ast::ValueType type;
  bool initialized;
  bool consumed = false;
};

class Validator {
public:
  explicit Validator(const ast::Module &module) : module_(module) {}
  CallableAnalysis run();

private:
  void validateSignature(const Callable &callable);
  void validateType(const ast::ValueType &type, bool allowVoid = false) const;
  void requireCapabilities(const std::vector<ast::TypeReference> &capabilities,
                           const ast::ContextDeclaration *context,
                           SourceLocation location) const;
  void requireEffects(const std::vector<ast::TypeReference> &effects,
                      SourceLocation location) const;
  Local &lookup(const ast::Expression &expression);
  ast::ValueType expression(const ast::Expression &expression);
  ast::ValueType call(const ast::Expression &expression);
  void match(const ast::Expression &expression, const ast::ValueType &actual,
             const ast::ValueType &expected);
  bool block(const ast::Statement &block, bool nested);
  const ast::ContextDeclaration *contextArgument(const ast::Expression &arg);
  ast::TypeReference substitute(const ast::TypeReference &type,
                                const Bindings &bindings) const;
  void infer(const ast::TypeReference &pattern, const ast::TypeReference &actual,
             const std::unordered_set<std::string> &generics, Bindings &bindings,
             SourceLocation location) const;
  ast::ValueType transitionCall(const ast::Expression &expression,
                               const Transition &transition);

  const ast::Module &module_;
  CallableAnalysis analysis_;
  std::unordered_map<std::string, const ast::ContextDeclaration *> contexts_;
  std::unordered_map<std::string, const ast::ResourceDeclaration *> states_;
  std::unordered_map<std::string, Callable> callables_;
  std::unordered_map<std::string, Transition> transitions_;
  std::vector<std::unordered_map<std::string, Local>> scopes_;
  const ast::CallableDeclaration *current_ = nullptr;
  const ast::ContextDeclaration *context_ = nullptr;
  std::string contextParameter_;
  bool isKernel_ = false;
};

void Validator::validateType(const ast::ValueType &type, bool allowVoid) const {
  const auto &reference = type.reference;
  const std::string name = reference.name.str();
  const auto state = states_.find(name);
  if (state != states_.end()) {
    if (reference.arguments.size() != state->second->parameters.size()) {
      fail("type_arity", "wrong generic arity for resource state `" + name + "`",
           type.location);
    }
    if (type.ownership != ast::OwnershipQualifier::Own) {
      fail("resource_not_owned", "resource state values require `own`", type.location);
    }
    for (std::size_t index = 0; index < reference.arguments.size(); ++index) {
      if (state->second->parameters[index].constraint.str() == "Context" &&
          !contexts_.contains(reference.arguments[index].str())) {
        fail("invalid_context_argument",
             "a resource's `Context` argument must name a declared context",
             type.location);
      }
    }
    return;
  }
  if (contexts_.contains(name) || name == "Context" ||
      std::any_of(module_.contexts.begin(), module_.contexts.end(),
                  [&](const auto &context) { return context.name.str() == name; })) {
    fail("runtime_context_value", "context evidence cannot be a runtime value",
         type.location);
  }
  if (name == "void") {
    if (!allowVoid || type.ownership == ast::OwnershipQualifier::Own ||
        !reference.arguments.empty()) {
      fail("invalid_void_type", "`void` is only an unowned callable return type",
           type.location);
    }
    return;
  }
  if (name == "bool" || isInteger(name) || isFloating(name)) {
    if (!reference.arguments.empty()) {
      fail("type_arity", "scalar types do not take generic arguments", type.location);
    }
    return;
  }
  if (name == "buffer" || name == "view" || name == "event") {
    const std::size_t arity = name == "view" ? 2 : 1;
    if (reference.arguments.size() != arity) {
      fail("type_arity", "wrong generic arity for `" + name + "`", type.location);
    }
    return;
  }
  fail("unknown_value_type", "unknown value type `" + reference.str() + "`",
       type.location);
}

void Validator::requireCapabilities(
    const std::vector<ast::TypeReference> &capabilities,
    const ast::ContextDeclaration *context, SourceLocation location) const {
  for (const auto &capability : capabilities) {
    if (!context || std::none_of(context->grants.begin(), context->grants.end(),
                                 [&](const auto &grant) {
                                   return grant.capability.str() == capability.str();
                                 })) {
      fail("missing_capability", "execution context does not grant `" +
                                    capability.str() + "`", location);
    }
  }
}

void Validator::requireEffects(const std::vector<ast::TypeReference> &effects,
                               SourceLocation location) const {
  for (const auto &effect : effects) {
    if (std::none_of(current_->effects.begin(), current_->effects.end(),
                     [&](const auto &declared) { return declared.str() == effect.str(); })) {
      fail("undeclared_effect", "call requires effect `" + effect.str() +
                                    "` in the enclosing signature", location);
    }
  }
  requireCapabilities(effects, context_, location);
}

void Validator::validateSignature(const Callable &callable) {
  const auto &declaration = *callable.declaration;
  std::unordered_set<std::string> names;
  const ast::ContextDeclaration *context = nullptr;
  for (const auto &parameter : declaration.parameters) {
    if (!names.insert(parameter.name).second) {
      fail("duplicate_callable_parameter", "duplicate parameter `" + parameter.name + "`",
           parameter.location);
    }
    if (!parameter.isContextEvidence) {
      validateType(parameter.type);
      continue;
    }
    if (context || parameter.name != "cx" ||
        parameter.type.ownership != ast::OwnershipQualifier::None ||
        !parameter.type.reference.arguments.empty()) {
      fail("invalid_execution_context", "use one `@cx: context_identifier` parameter",
           parameter.location);
    }
    const auto found = contexts_.find(parameter.type.reference.name.str());
    if (found == contexts_.end()) {
      fail("unknown_execution_context", "execution context must select a declared identifier",
           parameter.location);
    }
    context = found->second;
    if (callable.isKernel && (context->name.components.size() < 2 ||
                              context->name.components.front() != "Gpu")) {
      fail("kernel_execution_context", "a kernel requires a `Gpu::` execution context",
           parameter.location);
    }
  }
  validateType(declaration.resultType, true);
  requireCapabilities(declaration.effects, context, declaration.location);
  for (const auto &clause : declaration.allowsClauses) {
    if (!context || clause.context.str() != context->identifier) {
      fail("invalid_context_requirement", "where clauses must name the selected execution context",
           clause.location);
    }
    requireCapabilities(clause.capabilities, context, clause.location);
  }
  analysis_.contexts.emplace(&declaration, context);
}

CallableAnalysis Validator::run() {
  for (const auto &context : module_.contexts) {
    contexts_.emplace(context.identifier, &context);
  }
  for (const auto &resource : module_.resources) {
    for (const auto &state : resource.states) {
      states_.emplace(resource.name.str() + "::" + state.name, &resource);
    }
    for (const auto &transition : resource.transitions) {
      const std::string name = resource.name.str() + "::" + transition.name;
      if (!transitions_.emplace(name, Transition{&resource, &transition}).second) {
        fail("duplicate_transition", "duplicate transition `" + name + "`", transition.location);
      }
    }
  }
  auto add = [&](const auto &declarations, bool isKernel) {
    for (const auto &declaration : declarations) {
      const std::string name = declaration.name.str();
      if (!callables_.emplace(name, Callable{&declaration, isKernel}).second ||
          transitions_.contains(name)) {
        fail(isKernel ? "duplicate_kernel" : "duplicate_callable",
             "duplicate callable `" + name + "`", declaration.location);
      }
    }
  };
  add(module_.kernels, true);
  add(module_.procedures, false);
  // Validate in source-vector order for deterministic diagnostics. Index all
  // signatures first so forward calls and recursion see the same contracts.
  for (const auto *declarations : {&module_.kernels, &module_.procedures}) {
    for (const auto &declaration : *declarations) {
      validateSignature(callables_.at(declaration.name.str()));
    }
  }
  for (const auto *declarations : {&module_.kernels, &module_.procedures}) {
    for (const auto &declaration : *declarations) {
      current_ = &declaration;
      isKernel_ = callables_.at(declaration.name.str()).isKernel;
      context_ = analysis_.contexts.at(current_);
      contextParameter_ = context_ ? "cx" : "";
      scopes_.clear();
      scopes_.emplace_back();
      for (const auto &parameter : declaration.parameters) {
        if (!parameter.isContextEvidence) {
          scopes_.back().emplace(parameter.name, Local{parameter.type, true});
        }
      }
      const bool returns = block(declaration.body, false);
      if (!isVoid(declaration.resultType) && !returns) {
        fail("missing_return", "procedure `" + declaration.name.str() +
                                   "` must return `" + declaration.resultType.str() + "`",
             declaration.location);
      }
    }
  }
  return std::move(analysis_);
}

Local &Validator::lookup(const ast::Expression &expression) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(expression.name.str());
    if (found != scope->end()) {
      if (!found->second.initialized) {
        fail("uninitialized_variable", "use of uninitialized variable `" + expression.name.str() + "`",
             expression.location);
      }
      if (found->second.consumed) {
        fail("consumed_resource", "use of consumed value `" + expression.name.str() + "`",
             expression.location);
      }
      return found->second;
    }
  }
  if (expression.name.str() == contextParameter_ || contexts_.contains(expression.name.str())) {
    fail("runtime_context_value", "context evidence cannot be a runtime value", expression.location);
  }
  fail("unknown_variable", "unknown variable `" + expression.name.str() + "`", expression.location);
}

const ast::ContextDeclaration *
Validator::contextArgument(const ast::Expression &arg) {
  if (arg.kind != ast::ExpressionKind::ContextReference || !context_ ||
      arg.name.str() != contextParameter_) {
    fail("invalid_context_argument", "pass the current `@cx` as context evidence", arg.location);
  }
  ExpressionInfo info;
  info.context = context_;
  analysis_.expressions[&arg] = std::move(info);
  return context_;
}

ast::ValueType Validator::expression(const ast::Expression &expr) {
  ast::ValueType type;
  switch (expr.kind) {
  case ast::ExpressionKind::Literal:
    type = simpleType(expr.literal.kind == ast::LiteralKind::Boolean ? "bool" :
                      expr.literal.kind == ast::LiteralKind::Integer ? "i32" : "f32",
                      expr.location);
    break;
  case ast::ExpressionKind::Reference: {
    Local &local = lookup(expr);
    type = local.type;
    if (type.ownership == ast::OwnershipQualifier::Own) {
      local.consumed = true;
    }
    break;
  }
  case ast::ExpressionKind::ContextReference:
    fail("runtime_context_value", "context evidence cannot be stored or returned", expr.location);
  case ast::ExpressionKind::Call:
    type = call(expr);
    break;
  }
  analysis_.expressions[&expr].type = type;
  return type;
}

void Validator::match(const ast::Expression &expr, const ast::ValueType &actual,
                      const ast::ValueType &expected) {
  bool matches = actual.str() == expected.str();
  if (expr.kind == ast::ExpressionKind::Literal) {
    const std::string name = expected.reference.name.str();
    const auto kind = expr.literal.kind;
    matches = expected.ownership == ast::OwnershipQualifier::None &&
              expected.reference.arguments.empty() &&
              ((kind == ast::LiteralKind::Boolean && name == "bool") ||
               (kind == ast::LiteralKind::Integer && isInteger(name)) ||
               ((kind == ast::LiteralKind::Floating || kind == ast::LiteralKind::Infinity) &&
                isFloating(name)));
    if (kind == ast::LiteralKind::Integer && !name.empty() && name.front() == 'u' &&
        expr.literal.value.front() == '-') {
      matches = false;
    }
  }
  if (!matches) {
    fail("type_mismatch", "expected `" + expected.str() + "`, found `" + actual.str() + "`",
         expr.location);
  }
  analysis_.expressions.at(&expr).type = expected;
}

bool Validator::block(const ast::Statement &body, bool nested) {
  if (nested) {
    scopes_.emplace_back();
  }
  bool returned = false;
  for (const auto &statement : body.statements) {
    if (returned) {
      fail("unreachable_statement", "statement follows an unconditional return", statement.location);
    }
    switch (statement.kind) {
    case ast::StatementKind::Variable: {
      const auto &variable = statement.variable;
      validateType(variable.type);
      if (scopes_.back().contains(variable.name) || variable.name == contextParameter_) {
        fail(isKernel_ ? "duplicate_kernel_variable" : "duplicate_local_variable",
             "duplicate local `" + variable.name + "`", variable.location);
      }
      // Insert before checking the initializer: self-reference must not fall
      // through to an outer binding with the same name.
      scopes_.back().emplace(variable.name, Local{variable.type, false});
      if (variable.initializer) {
        const auto type = expression(*variable.initializer);
        match(*variable.initializer, type, variable.type);
        scopes_.back().at(variable.name).initialized = true;
      } else if (variable.isConstant) {
        fail("uninitialized_const_variable", "const variable requires an initializer", variable.location);
      }
      break;
    }
    case ast::StatementKind::Call: {
      const auto type = expression(*statement.expression);
      if (type.ownership == ast::OwnershipQualifier::Own) {
        fail("discarded_owned_result", "bind or return an owned call result", statement.location);
      }
      break;
    }
    case ast::StatementKind::Block:
      returned = block(statement, true);
      break;
    case ast::StatementKind::Return:
      if (statement.expression) {
        const auto type = expression(*statement.expression);
        if (isVoid(current_->resultType)) {
          fail("unexpected_return_value", "void callables require a bare return", statement.location);
        }
        match(*statement.expression, type, current_->resultType);
      } else if (!isVoid(current_->resultType)) {
        fail("missing_return_value", "return requires a value", statement.location);
      }
      returned = true;
      break;
    }
  }
  if (nested) {
    scopes_.pop_back();
  }
  return returned;
}

ast::TypeReference Validator::substitute(const ast::TypeReference &type,
                                         const Bindings &bindings) const {
  if (type.arguments.empty()) {
    const auto found = bindings.find(type.name.str());
    if (found != bindings.end()) {
      return found->second;
    }
  }
  auto result = type;
  for (auto &argument : result.arguments) {
    argument = substitute(argument, bindings);
  }
  return result;
}

void Validator::infer(const ast::TypeReference &pattern,
                      const ast::TypeReference &actual,
                      const std::unordered_set<std::string> &generics,
                      Bindings &bindings, SourceLocation location) const {
  const auto name = pattern.name.str();
  if (pattern.arguments.empty() && generics.contains(name)) {
    const auto [found, inserted] = bindings.emplace(name, actual);
    if (!inserted && found->second.str() != actual.str()) {
      fail("type_mismatch", "inconsistent type argument for `" + name + "`", location);
    }
    return;
  }
  if (pattern.name.str() == actual.name.str() &&
      pattern.arguments.size() == actual.arguments.size()) {
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      infer(pattern.arguments[index], actual.arguments[index], generics, bindings, location);
    }
  }
}

ast::ValueType Validator::call(const ast::Expression &expr) {
  const auto transition = transitions_.find(expr.name.str());
  if (transition != transitions_.end()) {
    return transitionCall(expr, transition->second);
  }
  const auto found = callables_.find(expr.name.str());
  if (found == callables_.end()) {
    fail("unknown_operation", "unknown operation `" + expr.name.str() + "`", expr.location);
  }
  if (found->second.isKernel) {
    fail("kernel_call", "kernels cannot be invoked as ordinary procedure calls", expr.location);
  }
  const auto &callee = *found->second.declaration;
  if (expr.arguments.size() != callee.parameters.size()) {
    fail("call_arity", "wrong argument count for `" + expr.name.str() + "`", expr.location);
  }
  const auto *calleeContext = analysis_.contexts.at(&callee);
  if (calleeContext && calleeContext != context_) {
    fail("execution_context_mismatch", "procedure call changes execution context", expr.location);
  }
  requireEffects(callee.effects, expr.location);
  for (std::size_t index = 0; index < expr.arguments.size(); ++index) {
    const auto &arg = expr.arguments[index];
    const auto &parameter = callee.parameters[index];
    if (parameter.isContextEvidence) {
      contextArgument(arg);
    } else {
      const auto type = expression(arg);
      match(arg, type, parameter.type);
    }
  }
  auto &info = analysis_.expressions[&expr];
  info.calleeKind = "procedure";
  info.context = calleeContext;
  info.effects = callee.effects;
  return callee.resultType;
}

ast::ValueType Validator::transitionCall(const ast::Expression &expr,
                                         const Transition &operation) {
  const auto &transition = *operation.declaration;
  const auto &resource = *operation.resource;
  if (expr.arguments.size() != transition.parameters.size()) {
    fail("call_arity", "wrong argument count for `" + expr.name.str() + "`", expr.location);
  }
  Bindings bindings;
  std::unordered_set<std::string> generics;
  for (const auto &parameter : resource.parameters) {
    generics.insert(parameter.name);
  }
  for (const auto &parameter : transition.genericParameters) {
    if (!generics.insert(parameter.name).second) {
      fail("ambiguous_generic_parameter", "transition generic shadows resource generic `" +
                                               parameter.name + "`", parameter.location);
    }
  }
  std::vector<ast::ValueType> actualTypes(expr.arguments.size());
  const ast::TypeReference *selfType = nullptr;
  for (std::size_t index = 0; index < expr.arguments.size(); ++index) {
    const auto &arg = expr.arguments[index];
    const auto &parameter = transition.parameters[index];
    if (parameter.isContextEvidence) {
      if (parameter.name == "self" || parameter.type.ownership == ast::OwnershipQualifier::Own) {
        fail("invalid_context_argument", "context evidence cannot be an owned resource", arg.location);
      }
      const auto *context = contextArgument(arg);
      const auto &pattern = parameter.type.reference;
      if (generics.contains(pattern.name.str()) && pattern.arguments.empty()) {
        bool contextConstraint = false;
        for (const auto *parameters : {&resource.parameters, &transition.genericParameters}) {
          for (const auto &generic : *parameters) {
            if (generic.name == pattern.name.str() && generic.constraint.str() == "Context") {
              contextConstraint = true;
            }
          }
        }
        if (!contextConstraint) {
          fail("invalid_context_argument", "context generic requires a `Context` constraint", arg.location);
        }
        infer(pattern, simpleType(context->identifier, arg.location).reference,
              generics, bindings, arg.location);
      } else if (pattern.str() != context->identifier) {
        fail("execution_context_mismatch", "transition requires a different execution context", arg.location);
      }
    } else {
      actualTypes[index] = expression(arg);
      if (parameter.name == "self") {
        selfType = &actualTypes[index].reference;
        const auto state = states_.find(selfType->name.str());
        if (state == states_.end() || state->second != &resource ||
            selfType->arguments.size() != resource.parameters.size()) {
          fail("type_mismatch", "transition requires a state of `" + resource.name.str() + "`", arg.location);
        }
        for (std::size_t argument = 0; argument < resource.parameters.size(); ++argument) {
          auto pattern = simpleType(resource.parameters[argument].name, arg.location).reference;
          infer(pattern, selfType->arguments[argument], generics, bindings, arg.location);
        }
      }
    }
  }
  if (!selfType) {
    fail("missing_transition_self", "transition requires a runtime self argument", expr.location);
  }
  auto isResourceState = [&](const ast::TypeReference &type) {
    return std::any_of(resource.states.begin(), resource.states.end(),
                       [&](const auto &state) {
                         return state.name == type.name.str();
                       });
  };
  for (std::size_t index = 0; index < transition.parameters.size(); ++index) {
    const auto &parameter = transition.parameters[index];
    if (!parameter.isContextEvidence && parameter.name != "self") {
      auto pattern = parameter.type.reference;
      if (isResourceState(pattern)) {
        pattern.name.components = resource.name.components;
        pattern.name.components.push_back(parameter.type.reference.name.str());
        if (pattern.arguments.empty()) {
          pattern.arguments = selfType->arguments;
        }
      }
      if (expr.arguments[index].kind == ast::ExpressionKind::Literal) {
        pattern = substitute(pattern, bindings);
      }
      infer(pattern, actualTypes[index].reference,
            generics, bindings, expr.arguments[index].location);
    }
  }
  for (const auto *parameters : {&resource.parameters,
                                 &transition.genericParameters}) {
    for (const auto &parameter : *parameters) {
      const auto binding = bindings.find(parameter.name);
      if (binding == bindings.end()) {
        fail("unresolved_type_argument",
             "cannot infer generic argument `" + parameter.name + "`",
             expr.location);
      }
      if (parameter.constraint.str() == "Context" &&
          !contexts_.contains(binding->second.str())) {
        fail("invalid_context_argument",
             "a `Context` generic must bind to declared context evidence",
             expr.location);
      }
    }
  }
  auto qualifyState = [&](const ast::TypeReference &state) {
    auto result = substitute(state, bindings);
    result.name.components = resource.name.components;
    result.name.components.push_back(state.name.str());
    if (state.arguments.empty()) {
      result.arguments = selfType->arguments;
    }
    return result;
  };
  for (std::size_t index = 0; index < transition.parameters.size(); ++index) {
    const auto &parameter = transition.parameters[index];
    if (parameter.isContextEvidence) {
      continue;
    }
    auto expected = parameter.type;
    expected.reference = isResourceState(parameter.type.reference)
                             ? qualifyState(parameter.type.reference)
                             : substitute(parameter.type.reference, bindings);
    validateType(expected);
    match(expr.arguments[index], actualTypes[index], expected);
  }
  std::vector<ast::TypeReference> effects;
  for (const auto &effect : transition.effects) {
    effects.push_back(substitute(effect, bindings));
  }
  requireEffects(effects, expr.location);
  for (const auto &clause : transition.allowsClauses) {
    const auto target = substitute(clause.context, bindings);
    if (!context_ || target.str() != context_->identifier) {
      fail("execution_context_mismatch", "transition where clause requires a different context", expr.location);
    }
    std::vector<ast::TypeReference> capabilities;
    for (const auto &capability : clause.capabilities) {
      capabilities.push_back(substitute(capability, bindings));
    }
    requireCapabilities(capabilities, context_, expr.location);
  }
  ast::ValueType result;
  result.location = expr.location;
  result.ownership = ast::OwnershipQualifier::Own;
  result.reference = qualifyState(transition.resultState);
  validateType(result);
  auto &info = analysis_.expressions[&expr];
  info.calleeKind = "transition";
  info.context = context_;
  info.isAwait = transition.isAwait;
  info.effects = std::move(effects);
  return result;
}

} // namespace

CallableAnalysis validateCallables(const ast::Module &module) {
  return Validator(module).run();
}

} // namespace metreon::sema
