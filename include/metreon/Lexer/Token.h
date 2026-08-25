#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <string>

namespace metreon::lexer {

enum class TokenKind {
  EndOfFile,
  Identifier,
  KeywordContext,
  KeywordGrants,
  KeywordResource,
  KeywordState,
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
  Arrow,
};

struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  std::string text;
  SourceLocation location;
};

const char *tokenKindName(TokenKind kind);

} // namespace metreon::lexer
