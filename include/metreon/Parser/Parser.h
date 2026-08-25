#pragma once

#include "metreon/AST/Context.h"
#include "metreon/Lexer/Lexer.h"

#include <string>
#include <string_view>

namespace metreon::parser {

class Parser {
public:
  explicit Parser(std::string_view source);

  ast::Module parseModule();

private:
  ast::ContextDeclaration parseContextDeclaration();
  ast::ResourceDeclaration parseResourceDeclaration();
  ast::ResourceStateDeclaration parseResourceStateDeclaration();
  ast::ResourceTransitionDeclaration parseResourceTransitionDeclaration();
  ast::QualifiedName parseQualifiedName();
  ast::TypeReference parseTypeReference();
  ast::ValueType parseValueType();
  std::vector<ast::ContextParameter> parseGenericParameters();
  std::vector<ast::GrantDeclaration> parseGrantList();
  std::vector<ast::ResourceField> parseResourceFields();
  std::vector<ast::TransitionParameter> parseTransitionParameters();
  std::vector<ast::TypeReference> parseEffectSet();

  bool consume(lexer::TokenKind kind);
  lexer::Token expect(lexer::TokenKind kind, const std::string &expectation);
  [[noreturn]] void fail(const std::string &code,
                         const std::string &message) const;
  void advance();

  lexer::Lexer lexer_;
  lexer::Token current_;
};

} // namespace metreon::parser
