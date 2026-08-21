#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace metreon {

struct Diagnostic {
  std::string code;
  std::string message;
  SourceLocation location;
};

class DiagnosticError final : public std::runtime_error {
public:
  explicit DiagnosticError(Diagnostic diagnostic)
      : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {}

  const Diagnostic &diagnostic() const noexcept { return diagnostic_; }

private:
  Diagnostic diagnostic_;
};

inline std::string formatDiagnostic(const std::string &sourceName,
                                    const Diagnostic &diagnostic) {
  return sourceName + ":" + std::to_string(diagnostic.location.line) + ":" +
         std::to_string(diagnostic.location.column) + ": error[" +
         diagnostic.code + "]: " + diagnostic.message;
}

} // namespace metreon
