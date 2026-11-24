#pragma once

#include "metreon/GraphIR/Graph.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Support/LogicalResult.h"

namespace metreon {
// Import just the current module's nominal context declarations. This is an
// internal building block for the explicitly scoped context-only device path.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
importGraphIRContexts(const graphir::Module &source, ::mlir::MLIRContext &context);

// Structural semantic import only. Ordinary bodies remain explicitly deferred;
// no target lowering or resource implementation is implied by successful import.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
importGraphIR(const graphir::Module &source, ::mlir::MLIRContext &context);

} // namespace metreon
