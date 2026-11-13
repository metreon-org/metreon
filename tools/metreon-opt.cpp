#include "metreon/MLIR/Dialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  registry.insert<metreon::MetreonDialect>();
  registry.insert<mlir::LLVM::LLVMDialect>();
  registry.insert<mlir::affine::AffineDialect, mlir::arith::ArithDialect,
      mlir::func::FuncDialect, mlir::gpu::GPUDialect, mlir::scf::SCFDialect, mlir::ub::UBDialect>();
  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "Metreon semantic MLIR inspector\n", registry));
}
