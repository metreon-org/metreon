#ifndef METREON_TYPES_TD
#define METREON_TYPES_TD

include "MetreonDialect.td"
include "mlir/IR/AttrTypeBase.td"

def Metreon_ResourceType : TypeDef<Metreon_Dialect, "Resource"> {
  let mnemonic = "resource";
  let summary = "An intrinsically owned nominal resource state";
  let parameters = (ins "::mlir::SymbolRefAttr":$state,
                        "::mlir::ArrayAttr":$arguments);
  let assemblyFormat = "`<` $state `,` $arguments `>`";
}

def Metreon_OwnedType : TypeDef<Metreon_Dialect, "Owned"> {
  let mnemonic = "owned";
  let summary = "Ownership of a non-resource payload";
  let parameters = (ins "::mlir::Type":$valueType);
  let assemblyFormat = "`<` $valueType `>`";
}

def Metreon_UnresolvedType : TypeDef<Metreon_Dialect, "Unresolved"> {
  let mnemonic = "unresolved";
  let summary = "Structured source type awaiting layout or target conversion";
  let description = [{
    Inspection-stage representation for storage, events, generic parameters,
    endpoints and other types with no selected physical layout. It is not an
    executable ABI and must be resolved before target lowering. Scalar types
    use builtin MLIR types directly. Context evidence is never a runtime type.
  }];
  let parameters = (ins "::mlir::DictionaryAttr":$sourceType);
  let assemblyFormat = "`<` $sourceType `>`";
}

#endif // METREON_TYPES_TD
