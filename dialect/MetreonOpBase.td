#ifndef METREON_OP_BASE_TD
#define METREON_OP_BASE_TD

include "MetreonDialect.td"
include "mlir/IR/OpBase.td"

class Metreon_Op<string mnemonic, list<Trait> traits = []>
    : Op<Metreon_Dialect, mnemonic, traits>;

// The initial operations introduce only names. ODS defaults give them zero
// operands, results, regions, and successors. No attributes, custom assembly
// formats, traits, or semantic verifiers are specified yet. For now, use generic
// MLIR syntax, e.g. "metreon.context"() : () -> ().

#endif // METREON_OP_BASE_TD
