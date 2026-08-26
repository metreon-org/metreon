#pragma once

#include "metreon/Lexer/Token.h"

#include <string_view>

namespace metreon::lexer {

class Lexer {
public:
  explicit Lexer(std::string_view source) : source_(source) {}

  Token next();

private:
  bool atEnd() const noexcept;
  char current() const noexcept;
  char peek(std::size_t distance = 1) const noexcept;
  char advance() noexcept;
  void skipTrivia();
  Token lexIdentifier();
  Token punctuation(TokenKind kind, std::size_t length = 1);

  std::string_view source_;
  std::size_t offset_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

} // namespace metreon::lexer
