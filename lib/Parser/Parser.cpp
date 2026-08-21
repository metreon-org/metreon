#include "metreon/Parser/Parser.h"

#include "metreon/Basic/Diagnostic.h"

#include <utility>

namespace metreon::parser {

Parser::Parser(std::string_view source) : lexer_(source), current_(lexer_.next()) {}

ast::Module Parser::parseModule() {
  ast::Module module;
  while (current_.kind != lexer::TokenKind::EndOfFile) {
    if (current_.kind != lexer::TokenKind::KeywordContext) {
      fail("parse.expected_context",
           "expected a context declaration, found " +
               std::string(lexer::tokenKindName(current_.kind)));
    }
    module.contexts.push_back(parseContextDeclaration());
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
    declaration.parameters = parseContextParameters();
  }

  expect(lexer::TokenKind::KeywordGrants, "`grants`");
  expect(lexer::TokenKind::LeftBrace, "`{` before the grant list");
  declaration.grants = parseGrantList();
  expect(lexer::TokenKind::RightBrace, "`}` after the grant list");
  expect(lexer::TokenKind::Semicolon, "`;` after the context declaration");
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

std::vector<ast::ContextParameter> Parser::parseContextParameters() {
  std::vector<ast::ContextParameter> parameters;
  if (current_.kind == lexer::TokenKind::Greater) {
    fail("parse.empty_context_parameters",
         "context parameter lists cannot be empty");
  }

  while (true) {
    const lexer::Token name =
        expect(lexer::TokenKind::Identifier, "a context parameter name");
    expect(lexer::TokenKind::Colon, "`:` after the context parameter name");
    parameters.push_back(
        ast::ContextParameter{name.text, parseTypeReference(), name.location});

    if (!consume(lexer::TokenKind::Comma)) {
      break;
    }
    if (current_.kind == lexer::TokenKind::Greater) {
      fail("parse.trailing_context_parameter_comma",
           "context parameter lists do not permit a trailing comma");
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
