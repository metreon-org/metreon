#include "metreon/Parser/Parser.h"

#include "metreon/Basic/Diagnostic.h"

#include <utility>

namespace metreon::parser {

Parser::Parser(std::string_view source) : lexer_(source), current_(lexer_.next()) {}

ast::Module Parser::parseModule() {
  ast::Module module;
  while (current_.kind != lexer::TokenKind::EndOfFile) {
    if (current_.kind == lexer::TokenKind::KeywordContext) {
      module.contexts.push_back(parseContextDeclaration());
      continue;
    }
    if (current_.kind == lexer::TokenKind::KeywordResource) {
      module.resources.push_back(parseResourceDeclaration());
      continue;
    }
    fail("parse.expected_context",
         "expected a context or resource declaration, found " +
             std::string(lexer::tokenKindName(current_.kind)));
  }
  return module;
}

ast::ContextDeclaration Parser::parseContextDeclaration() {
  const lexer::Token contextToken =
      expect(lexer::TokenKind::KeywordContext, "`context`");
  ast::ContextDeclaration declaration;
  declaration.location = contextToken.location;
  declaration.name = parseQualifiedName();

  if (consume(lexer::TokenKind::Less)) {
    declaration.parameters = parseGenericParameters();
  }

  expect(lexer::TokenKind::KeywordGrants, "`grants`");
  expect(lexer::TokenKind::LeftBrace, "`{` before the grant list");
  declaration.grants = parseGrantList();
  expect(lexer::TokenKind::RightBrace, "`}` after the grant list");
  if (current_.kind != lexer::TokenKind::Identifier) {
    fail("parse.expected_context_identifier",
         "expected a unique context identifier after the grant list");
  }
  const lexer::Token identifier = expect(
      lexer::TokenKind::Identifier,
      "a unique context identifier after the grant list");
  declaration.identifier = identifier.text;
  declaration.identifierLocation = identifier.location;
  expect(lexer::TokenKind::Semicolon, "`;` after the context declaration");
  return declaration;
}

ast::ResourceDeclaration Parser::parseResourceDeclaration() {
  const lexer::Token resourceToken =
      expect(lexer::TokenKind::KeywordResource, "`resource`");
  ast::ResourceDeclaration declaration;
  declaration.location = resourceToken.location;
  declaration.name = parseQualifiedName();

  if (consume(lexer::TokenKind::Less)) {
    declaration.parameters = parseGenericParameters();
  }

  expect(lexer::TokenKind::LeftBrace, "`{` before the resource body");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile) {
      fail("parse.unterminated_resource",
           "expected `}` before the end of the resource declaration");
    }
    if (current_.kind == lexer::TokenKind::KeywordState) {
      declaration.states.push_back(parseResourceStateDeclaration());
      continue;
    }
    if (current_.kind == lexer::TokenKind::KeywordTransition ||
        current_.kind == lexer::TokenKind::KeywordAwait) {
      declaration.transitions.push_back(parseResourceTransitionDeclaration());
      continue;
    }
    fail("parse.expected_resource_member",
         "expected a state or transition declaration, found " +
             std::string(lexer::tokenKindName(current_.kind)));
  }
  expect(lexer::TokenKind::RightBrace, "`}` after the resource body");
  consume(lexer::TokenKind::Semicolon);
  return declaration;
}

ast::ResourceStateDeclaration Parser::parseResourceStateDeclaration() {
  const lexer::Token stateToken =
      expect(lexer::TokenKind::KeywordState, "`state`");
  const lexer::Token name =
      expect(lexer::TokenKind::Identifier, "a resource state name");

  ast::ResourceStateDeclaration declaration;
  declaration.name = name.text;
  declaration.location = stateToken.location;
  expect(lexer::TokenKind::LeftParen, "`(` after the resource state name");
  declaration.fields = parseResourceFields();
  expect(lexer::TokenKind::RightParen, "`)` after the resource state fields");
  expect(lexer::TokenKind::Semicolon, "`;` after the resource state");
  return declaration;
}

ast::ResourceTransitionDeclaration
Parser::parseResourceTransitionDeclaration() {
  const SourceLocation declarationLocation = current_.location;
  const bool isAwait = consume(lexer::TokenKind::KeywordAwait);
  expect(lexer::TokenKind::KeywordTransition,
         isAwait ? "`transition` after `await`" : "`transition`");
  const lexer::Token name =
      expect(lexer::TokenKind::Identifier, "a transition name");

  ast::ResourceTransitionDeclaration declaration;
  declaration.name = name.text;
  declaration.isAwait = isAwait;
  declaration.location = declarationLocation;
  if (consume(lexer::TokenKind::Less)) {
    declaration.genericParameters = parseGenericParameters();
  }

  expect(lexer::TokenKind::LeftParen, "`(` after the transition name");
  declaration.parameters = parseTransitionParameters();
  expect(lexer::TokenKind::RightParen, "`)` after transition parameters");
  expect(lexer::TokenKind::Arrow, "`->` before the target state");
  declaration.resultState = parseTypeReference();

  if (consume(lexer::TokenKind::Bang)) {
    declaration.effects = parseEffectSet();
  }

  while (consume(lexer::TokenKind::KeywordWhere)) {
    ast::AllowsClause clause;
    clause.location = current_.location;
    clause.context = parseTypeReference();
    expect(lexer::TokenKind::KeywordAllows, "`allows` in a where clause");
    clause.capabilities = parseEffectSet();
    declaration.allowsClauses.push_back(std::move(clause));
  }

  expect(lexer::TokenKind::Semicolon, "`;` after the transition");
  return declaration;
}

ast::QualifiedName Parser::parseQualifiedName() {
  const lexer::Token first =
      expect(lexer::TokenKind::Identifier, "an identifier");
  ast::QualifiedName name;
  name.location = first.location;
  name.components.push_back(first.text);

  while (consume(lexer::TokenKind::ColonColon)) {
    name.components.push_back(
        expect(lexer::TokenKind::Identifier, "an identifier after `::`").text);
  }
  return name;
}

ast::TypeReference Parser::parseTypeReference() {
  ast::TypeReference reference;
  reference.name = parseQualifiedName();
  reference.location = reference.name.location;

  if (!consume(lexer::TokenKind::Less)) {
    return reference;
  }

  if (current_.kind == lexer::TokenKind::Greater) {
    fail("parse.empty_type_arguments", "type argument lists cannot be empty");
  }

  while (true) {
    reference.arguments.push_back(parseTypeReference());
    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
    if (current_.kind == lexer::TokenKind::Greater) {
      fail("parse.trailing_type_argument_comma",
           "type argument lists do not permit a trailing comma");
    }
  }
  expect(lexer::TokenKind::Greater, "`>` after type arguments");
  return reference;
}

ast::ValueType Parser::parseValueType() {
  ast::ValueType type;
  type.location = current_.location;
  if (consume(lexer::TokenKind::KeywordOwn)) {
    type.ownership = ast::OwnershipQualifier::Own;
  }
  type.reference = parseTypeReference();
  return type;
}

std::vector<ast::ContextParameter> Parser::parseGenericParameters() {
  std::vector<ast::ContextParameter> parameters;
  if (current_.kind == lexer::TokenKind::Greater) {
    fail("parse.empty_generic_parameters",
         "generic parameter lists cannot be empty");
  }

  while (true) {
    const lexer::Token name =
        expect(lexer::TokenKind::Identifier, "a generic parameter name");
    expect(lexer::TokenKind::Colon, "`:` after the generic parameter name");
    parameters.push_back(
        ast::ContextParameter{name.text, parseTypeReference(), name.location});

    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
    if (current_.kind == lexer::TokenKind::Greater) {
      fail("parse.trailing_generic_parameter_comma",
           "generic parameter lists do not permit a trailing comma");
    }
  }

  expect(lexer::TokenKind::Greater, "`>` after context parameters");
  return parameters;
}

std::vector<ast::GrantDeclaration> Parser::parseGrantList() {
  std::vector<ast::GrantDeclaration> grants;
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile) {
      fail("parse.unterminated_grant_list",
           "expected `}` before the end of file");
    }

    ast::TypeReference capability = parseTypeReference();
    const SourceLocation location = capability.location;
    grants.push_back(
        ast::GrantDeclaration{std::move(capability), location});

    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
  }
  return grants;
}

std::vector<ast::ResourceField> Parser::parseResourceFields() {
  std::vector<ast::ResourceField> fields;
  while (current_.kind != lexer::TokenKind::RightParen) {
    const lexer::Token name =
        expect(lexer::TokenKind::Identifier, "a resource field name");
    expect(lexer::TokenKind::Colon, "`:` after the resource field name");
    fields.push_back(
        ast::ResourceField{name.text, parseValueType(), name.location});

    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
  }
  return fields;
}

std::vector<ast::TransitionParameter> Parser::parseTransitionParameters() {
  std::vector<ast::TransitionParameter> parameters;
  while (current_.kind != lexer::TokenKind::RightParen) {
    const bool isContextEvidence = consume(lexer::TokenKind::At);
    const lexer::Token name =
        expect(lexer::TokenKind::Identifier, "a transition parameter name");
    expect(lexer::TokenKind::Colon,
           "`:` after the transition parameter name");
    parameters.push_back(ast::TransitionParameter{
        name.text, parseValueType(), isContextEvidence, name.location});

    if (!consume(lexer::TokenKind::Comma) &&
        !consume(lexer::TokenKind::Semicolon)) {
      break;
    }
  }
  return parameters;
}

std::vector<ast::TypeReference> Parser::parseEffectSet() {
  expect(lexer::TokenKind::LeftBrace, "`{` before the capability set");
  std::vector<ast::TypeReference> capabilities;
  while (current_.kind != lexer::TokenKind::RightBrace) {
    capabilities.push_back(parseTypeReference());
    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
  }
  expect(lexer::TokenKind::RightBrace, "`}` after the capability set");
  return capabilities;
}

bool Parser::consume(lexer::TokenKind kind) {
  if (current_.kind != kind) {
    return false;
  }
  advance();
  return true;
}

lexer::Token Parser::expect(lexer::TokenKind kind,
                            const std::string &expectation) {
  if (current_.kind != kind) {
    fail("parse.unexpected_token",
         "expected " + expectation + ", found " +
             std::string(lexer::tokenKindName(current_.kind)));
  }
  lexer::Token token = current_;
  advance();
  return token;
}

[[noreturn]] void Parser::fail(const std::string &code,
                               const std::string &message) const {
  throw DiagnosticError({code, message, current_.location});
}

void Parser::advance() { current_ = lexer_.next(); }

} // namespace metreon::parser
