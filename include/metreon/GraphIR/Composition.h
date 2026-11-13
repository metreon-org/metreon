#pragma once

#include "metreon/AST/Context.h"
#include "metreon/GraphIR/Graph.h"

namespace metreon::graphir {
void lowerCompositions(const ast::Module &source, Module &module);
std::string printCompositions(const Module &module);
} // namespace metreon::graphir
