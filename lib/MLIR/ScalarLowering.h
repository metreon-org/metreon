#pragma once

#include "metreon/MLIR/Import.h"
#include "llvm/ADT/ArrayRef.h"

namespace metreon::detail {
// Shared scalar realization of the existing GraphIR CFG. Callers must freshly
// validate the exact GraphIR with importGraphIR and check target eligibility
// before invoking this internal helper. Context evidence never becomes SSA.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>> lowerScalarCFG(
    const graphir::Module &source,
    ::llvm::ArrayRef<const graphir::CallableGraph *> graphs,
    ::mlir::MLIRContext &context, unsigned indexBits);
} // namespace metreon::detail
