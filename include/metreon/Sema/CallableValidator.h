#pragma once

#include "metreon/AST/Context.h"

#include <string>
#include <unordered_map>

namespace metreon::sema {

// Entries refer to the immutable AST supplied to validateCallables. Lowering
// consumes these resolutions rather than resolving calls a second time.
struct ExpressionInfo {
  ast::ValueType type;
  const ast::ContextDeclaration *context = nullptr;
  std::string calleeKind;
  bool isAwait = false;
  std::vector<ast::TypeReference> effects;
};

struct CallableAnalysis {
  std::unordered_map<const ast::CallableDeclaration *,
                     const ast::ContextDeclaration *> contexts;
  std::unordered_map<const ast::Expression *, ExpressionInfo> expressions;
};

CallableAnalysis validateCallables(const ast::Module &module);

} // namespace metreon::sema
