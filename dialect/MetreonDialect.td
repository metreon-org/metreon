#ifndef METREON_DIALECT_TD
#define METREON_DIALECT_TD

include "mlir/IR/DialectBase.td"

def Metreon_Dialect : Dialect {
  let name = "metreon";
  let cppNamespace = "::metreon";
  let summary = "Execution contexts and resource lifecycles for Metreon";
  let description = [{
    The Metreon dialect represents execution contexts, resources, states, and
    transitions. This initial definition provides the dialect namespace; the
    operation schemas and semantics will be defined separately.
  }];
}

#endif // METREON_DIALECT_TD
