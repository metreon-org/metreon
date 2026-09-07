#pragma once

#include "metreon/AST/Context.h"

namespace metreon::sema {

void validateContexts(const ast::Module &module);
void validateResources(const ast::Module &module);
// Compatibility entry point; validates the shared kernel/procedure contracts.
void validateKernels(const ast::Module &module);

} // namespace metreon::sema
