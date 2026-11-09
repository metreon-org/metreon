#include "metreon/MLIR/Dialect.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  registry.insert<metreon::MetreonDialect>();
  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "Metreon semantic MLIR inspector\n", registry));
}
