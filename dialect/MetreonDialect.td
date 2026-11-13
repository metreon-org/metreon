#ifndef METREON_DIALECT_TD
#define METREON_DIALECT_TD

include "mlir/IR/DialectBase.td"

def Metreon_Dialect : Dialect {
  let name = "metreon";
  let cppNamespace = "::metreon";
  let summary = "Execution contexts and resource lifecycles for Metreon";
  let description = [{
    The Metreon dialect represents execution contexts, resources, states, and
    transitions. Ordinary computation belongs to standard MLIR dialects;
    structural import can retain its body explicitly for later conversion.
  }];
  let useDefaultTypePrinterParser = 1;
}

#endif // METREON_DIALECT_TD
