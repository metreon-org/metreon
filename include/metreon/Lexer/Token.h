#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <string>

namespace metreon::lexer {

enum class TokenKind {
  EndOfFile,
  Identifier,
  IntegerLiteral,
  FloatingLiteral,
  BooleanLiteral,
  KeywordContext,
  KeywordGrants,
  KeywordResource,
  KeywordKernel,
  KeywordProcedure,
  KeywordReturn,
  KeywordConst,
  KeywordState,
  KeywordAccumulator,
  KeywordTransition,
  KeywordAwait,
  KeywordWhere,
  KeywordAllows,
  KeywordOwn,
  ColonColon,
  Colon,
  Less,
  Greater,
  Comma,
  LeftBrace,
  RightBrace,
  LeftParen,
  RightParen,
  Semicolon,
  At,
  Bang,
  Equal,
  Minus,
  Arrow,
};

struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  std::string text;
  SourceLocation location;
};

const char *tokenKindName(TokenKind kind);

} // namespace metreon::lexer
