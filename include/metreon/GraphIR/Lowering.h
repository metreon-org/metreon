#pragma once

#include "metreon/AST/Context.h"
#include "metreon/GraphIR/Graph.h"

#include <string>

namespace metreon::graphir {

Module lowerModule(const ast::Module &sourceModule, std::string sourceName);
Module lowerContexts(const ast::Module &sourceModule, std::string sourceName);

} // namespace metreon::graphir
