#pragma once

#include "metreon/MLIR/Import.h"

namespace metreon {
// Lower GPU scalar control flow from freshly checked in-memory GraphIR.
// Metreon contracts and non-device bodies remain semantic declarations; device
// bodies use primary MLIR dialects inside gpu.module. No device launch or binary
// generation is performed by this stage.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
lowerGPUControlFlow(const graphir::Module &source, ::mlir::MLIRContext &context);
} // namespace metreon
