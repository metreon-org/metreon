#ifndef METREON_CONTEXT_OPS_TD
#define METREON_CONTEXT_OPS_TD

include "MetreonOpBase.td"
include "mlir/IR/SymbolInterfaces.td"

def Metreon_ContextResolutionAttr : StringBasedAttr<And<[
  StrAttr.predicate,
  CPred<"::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"declared\" || "
        "::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"external\"">
]>, "context resolution ('declared' or 'external')">;

def Metreon_ContextOp : Metreon_Op<"context", [
  Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>
]> {
  let summary = "Declare nominal, compile-time context evidence";
  let description = [{
    Imports one GraphIR ContextMetadata identity. Context variables and their
    constraints become ordered `generic_parameters`; grant nodes and argument
    edges become recursive entries in `grants`. Context-transition edges such
    as `defer<C>` remain grant metadata, not executable resource transitions.

    `nominal_type` is a structured type application with a qualified `name`
    and an `arguments` array. Each generic parameter is a dictionary containing
    its `name` and structured `constraint`. Each grant is a dictionary with a
    capability `name` and recursive `arguments`; context targets use symbol
    references and generic targets use parameter-reference dictionaries.
    These builtin containers provide the schema boundary until dedicated
    Metreon attributes are introduced; canonical type strings are not the
    representation of nested applications.

    The symbol is the module-local identity. `identifier`, when present,
    preserves the source alias; equal aliases, nominal types, or grant sets
    do not authorize merging distinct context symbols. Generic arity is
    derived from `generic_parameters`, not stored redundantly. External
    declarations preserve unresolved evidence but cannot prove capabilities.

    This declaration has no operands, results, or regions. Context evidence
    must never become a runtime value, block argument, or stored field.
    Lowering may erase it only after all contextual users have been legalized
    and no references remain.

    The declared verifier hooks require recursive attribute checks, parameter
    identity/constraints, and sorted, duplicate-free grants. Symbol-use
    verification must resolve concrete grant targets without conflating contexts
    with the same nominal type. Structural C++ checks accompany the importer;
    broader effect/lifecycle analysis remains separate from declaration import.
  }];

  let arguments = (ins
    SymbolNameAttr:$sym_name,
    OptionalAttr<StrAttr>:$sym_visibility,
    DictionaryAttr:$nominal_type,
    DictArrayAttr:$generic_parameters,
    DictArrayAttr:$grants,
    Metreon_ContextResolutionAttr:$resolution,
    OptionalAttr<StrAttr>:$identifier
  );

  let hasVerifier = 1;
}

#endif // METREON_CONTEXT_OPS_TD
