#ifndef METREON_OP_BASE_TD
#define METREON_OP_BASE_TD

include "MetreonDialect.td"
include "mlir/IR/OpBase.td"

class Metreon_Op<string mnemonic, list<Trait> traits = []>
    : Op<Metreon_Dialect, mnemonic, traits>;

// Operations use generic MLIR assembly and explicit semantic attributes.

#endif // METREON_OP_BASE_TD
