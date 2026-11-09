#pragma once

#include "metreon/Parser/Parser.h"

namespace metreon::parser {

// Reads canonical GraphIR attribute fragments, never source modules or printed
// GraphIR. Temporary AST values reuse the parser's recursive grammar; no AST
// pointer survives into the imported representation.
class CanonicalSyntax {
  static void end(Parser &parser) {
    parser.expect(lexer::TokenKind::EndOfFile, "end of canonical GraphIR attribute");
  }
public:
  static ast::ValueType valueType(const std::string &text) {
    Parser parser(text);
    auto value = parser.parseValueType();
    end(parser);
    return value;
  }
  static ast::TypeReference type(const std::string &text) {
    Parser parser(text);
    auto value = parser.parseTypeReference();
    end(parser);
    return value;
  }
  static std::vector<ast::ContextParameter> generics(const std::string &text) {
    if (text.empty()) return {};
    const std::string input = text + ">";
    Parser parser(input);
    auto values = parser.parseGenericParameters();
    end(parser);
    return values;
  }
  static std::vector<ast::TransitionParameter> parameters(const std::string &text) {
    const std::string input = text + ")";
    Parser parser(input);
    auto values = parser.parseTransitionParameters();
    parser.expect(lexer::TokenKind::RightParen, "end of parameter list");
    end(parser);
    return values;
  }
  static std::vector<ast::TypeReference> effects(const std::string &text) {
    const std::string input = "{" + text + "}";
    Parser parser(input);
    auto values = parser.parseEffectSet();
    end(parser);
    return values;
  }
  static std::vector<ast::AllowsClause> requirements(const std::string &text) {
    Parser parser(text);
    std::vector<ast::AllowsClause> values;
    while (parser.current_.kind != lexer::TokenKind::EndOfFile) {
      ast::AllowsClause clause;
      clause.context = parser.parseTypeReference();
      parser.expect(lexer::TokenKind::KeywordAllows, "allows in context requirement");
      clause.capabilities = parser.parseEffectSet();
      values.push_back(std::move(clause));
      if (!parser.consume(lexer::TokenKind::Semicolon)) break;
    }
    end(parser);
    return values;
  }
};
} // namespace metreon::parser
