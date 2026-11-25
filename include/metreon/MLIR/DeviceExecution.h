#pragma once

#include "metreon/MLIR/Import.h"
#include "llvm/Support/JSON.h"

namespace metreon {
// Experimental device profile: current GraphIR -> live Metreon contracts and
// context-bearing intrinsics -> primary GPU dialects -> NVVM/PTX or ROCDL/AMDGPU. Only
// nominal context/grant checks and compiler/ABI well-formedness are performed.
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
importDeviceExecution(const graphir::Module &source, ::mlir::MLIRContext &context);
::mlir::LogicalResult verifyDeviceContexts(::mlir::ModuleOp module);
::mlir::LogicalResult lowerMetreonDeviceToGPU(::mlir::ModuleOp module);
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>> lowerDeviceToNVVM(::mlir::ModuleOp module);
::mlir::FailureOr<std::string> emitDevicePTX(::mlir::ModuleOp nvvm, ::llvm::StringRef chip);
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>> lowerDeviceToROCDL(::mlir::ModuleOp module);
// AMDGPU assembly and relocatable ELF object, both produced from ROCDL. The
// object is subsequently linked into an HSA code object for HIP module loading.
::mlir::FailureOr<std::string> emitDeviceAMDGPU(::mlir::ModuleOp rocdl, ::llvm::StringRef chip,
                                             bool object = false);
::llvm::json::Object deviceLaunchManifest(::mlir::ModuleOp semantic);
} // namespace metreon
