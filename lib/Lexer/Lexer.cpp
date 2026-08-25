#include "metreon/Lexer/Lexer.h"

#include "metreon/Basic/Diagnostic.h"

#include <cctype>
#include <string>

namespace metreon::lexer {

namespace {

bool isIdentifierStart(char value) {
  const auto character = static_cast<unsigned char>(value);
  return std::isalpha(character) != 0 || value == '_';
}

bool isIdentifierContinue(char value) {
  const auto character = static_cast<unsigned char>(value);
  return std::isalnum(character) != 0 || value == '_';
}

} // namespace

const char *tokenKindName(TokenKind kind) {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "end of file";
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::KeywordContext:
    return "`context`";
  case TokenKind::KeywordGrants:
    return "`grants`";
  case TokenKind::KeywordResource:
    return "`resource`";
  case TokenKind::KeywordState:
    return "`state`";
  case TokenKind::KeywordTransition:
    return "`transition`";
  case TokenKind::KeywordAwait:
    return "`await`";
  case TokenKind::KeywordWhere:
    return "`where`";
  case TokenKind::KeywordAllows:
    return "`allows`";
  case TokenKind::KeywordOwn:
    return "`own`";
  case TokenKind::ColonColon:
    return "`::`";
  case TokenKind::Colon:
    return "`:`";
  case TokenKind::Less:
    return "`<`";
  case TokenKind::Greater:
    return "`>`";
  case TokenKind::Comma:
    return "`,`";
  case TokenKind::LeftBrace:
    return "`{`";
  case TokenKind::RightBrace:
    return "`}`";
  case TokenKind::LeftParen:
    return "`(`";
  case TokenKind::RightParen:
    return "`)`";
  case TokenKind::Semicolon:
    return "`;`";
  case TokenKind::At:
    return "`@`";
  case TokenKind::Bang:
    return "`!`";
  case TokenKind::Arrow:
    return "`->`";
  }
  return "unknown token";
}

bool Lexer::atEnd() const noexcept { return offset_ >= source_.size(); }

char Lexer::current() const noexcept { return atEnd() ? '\0' : source_[offset_]; }

char Lexer::peek(std::size_t distance) const noexcept {
  const std::size_t index = offset_ + distance;
  return index >= source_.size() ? '\0' : source_[index];
}

char Lexer::advance() noexcept {
  const char value = current();
  if (atEnd()) {
    return value;
  }

  ++offset_;
  if (value == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return value;
}

void Lexer::skipTrivia() {
  while (!atEnd()) {
    const auto character = static_cast<unsigned char>(current());
    if (std::isspace(character) != 0) {
      advance();
      continue;
    }

    if (current() == '/' && peek() == '/') {
      advance();
      advance();
      while (!atEnd() && current() != '\n') {
        advance();
      }
      continue;
    }

    if (current() == '/' && peek() == '*') {
      const SourceLocation start{offset_, line_, column_};
      advance();
      advance();
      std::size_t depth = 1;
      while (depth != 0) {
        if (atEnd()) {
          throw DiagnosticError({"lex.unterminated_block_comment",
                                 "unterminated block comment", start});
        }
        if (current() == '/' && peek() == '*') {
          advance();
          advance();
          ++depth;
          continue;
        }
        if (current() == '*' && peek() == '/') {
          advance();
          advance();
          --depth;
          continue;
        }
        advance();
      }
      continue;
    }

    break;
  }
}

Token Lexer::lexIdentifier() {
  const SourceLocation start{offset_, line_, column_};
  const std::size_t begin = offset_;
  advance();
  while (isIdentifierContinue(current())) {
    advance();
  }

  std::string text(source_.substr(begin, offset_ - begin));
  TokenKind kind = TokenKind::Identifier;
  if (text == "context") {
    kind = TokenKind::KeywordContext;
  } else if (text == "grants") {
    kind = TokenKind::KeywordGrants;
  } else if (text == "resource") {
    kind = TokenKind::KeywordResource;
  } else if (text == "state") {
    kind = TokenKind::KeywordState;
  } else if (text == "transition") {
    kind = TokenKind::KeywordTransition;
  } else if (text == "await") {
    kind = TokenKind::KeywordAwait;
  } else if (text == "where") {
    kind = TokenKind::KeywordWhere;
  } else if (text == "allows") {
    kind = TokenKind::KeywordAllows;
  } else if (text == "own") {
    kind = TokenKind::KeywordOwn;
  }
  return Token{kind, std::move(text), start};
}

Token Lexer::punctuation(TokenKind kind, std::size_t length) {
  const SourceLocation start{offset_, line_, column_};
  const std::size_t begin = offset_;
  for (std::size_t index = 0; index < length; ++index) {
    advance();
  }
  return Token{kind, std::string(source_.substr(begin, length)), start};
}

Token Lexer::next() {
  skipTrivia();
  if (atEnd()) {
    return Token{TokenKind::EndOfFile, "", {offset_, line_, column_}};
  }

  if (isIdentifierStart(current())) {
    return lexIdentifier();
  }

  switch (current()) {
  case ':':
    if (peek() == ':') {
      return punctuation(TokenKind::ColonColon, 2);
    }
    return punctuation(TokenKind::Colon);
  case '<':
    return punctuation(TokenKind::Less);
  case '>':
    return punctuation(TokenKind::Greater);
  case ',':
    return punctuation(TokenKind::Comma);
  case '{':
    return punctuation(TokenKind::LeftBrace);
  case '}':
    return punctuation(TokenKind::RightBrace);
  case '(':
    return punctuation(TokenKind::LeftParen);
  case ')':
    return punctuation(TokenKind::RightParen);
  case ';':
    return punctuation(TokenKind::Semicolon);
  case '@':
    return punctuation(TokenKind::At);
  case '!':
    return punctuation(TokenKind::Bang);
  case '-':
    if (peek() == '>') {
      return punctuation(TokenKind::Arrow, 2);
    }
    break;
  default:
    break;
  }

  const SourceLocation location{offset_, line_, column_};
  throw DiagnosticError({"lex.unexpected_character",
                         "unexpected character `" + std::string(1, current()) +
                             "`",
                         location});
}

} // namespace metreon::lexer
