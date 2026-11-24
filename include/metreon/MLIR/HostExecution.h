#pragma once

#include "metreon/MLIR/Import.h"
#include <vector>

namespace metreon {
struct HostABI {
  std::string symbol;
  std::string kind;
  std::string kernel;
};
::mlir::FailureOr<::mlir::OwningOpRef<::mlir::ModuleOp>>
importHostExecution(const graphir::Module &source, ::mlir::MLIRContext &context);
::mlir::LogicalResult verifyHostContexts(::mlir::ModuleOp module);
::mlir::FailureOr<std::vector<HostABI>> lowerHostExecution(::mlir::ModuleOp module);
} // namespace metreon
