#pragma once

#include "metreon/MLIR/Import.h"

namespace metreon {

// Consume the current in-memory GraphIR, repeat semantic/CFG import checks,
// and lower the supported host scalar subset through standard MLIR conversion.
// No AST, source parser, or printed GraphIR is an executable-lowering input.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
lowerHostToLLVM(const graphir::Module &source, ::mlir::MLIRContext &context);

} // namespace metreon
