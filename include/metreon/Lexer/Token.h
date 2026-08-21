#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <string>

namespace metreon::lexer {

enum class TokenKind {
  EndOfFile,
  Identifier,
  KeywordContext,
  KeywordGrants,
  ColonColon,
  Colon,
  Less,
  Greater,
  Comma,
  LeftBrace,
  RightBrace,
  Semicolon,
};

struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  std::string text;
  SourceLocation location;
};

const char *tokenKindName(TokenKind kind);

} // namespace metreon::lexer
