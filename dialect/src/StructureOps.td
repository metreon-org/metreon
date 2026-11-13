#ifndef METREON_STRUCTURE_OPS_TD
#define METREON_STRUCTURE_OPS_TD

include "MetreonOpBase.td"
include "mlir/IR/RegionKindInterface.td"
include "mlir/IR/SymbolInterfaces.td"

// These are semantic contracts, not replacements for func/gpu callables.
// deferred_body retains a lossless structured GraphIR operation tree until
// ordinary computation is translated to standard dialects in a later stage.
class Metreon_CallableContract<string mnemonic> : Metreon_Op<mnemonic, [
    Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
      OptionalAttr<StrAttr>:$sym_visibility,
      TypeAttrOf<FunctionType>:$function_type,
      DictArrayAttr:$parameter_attrs, DictArrayAttr:$result_attrs,
      DictArrayAttr:$context_requirements, DictArrayAttr:$effects,
      OptionalAttr<SymbolRefAttr>:$context, StrAttr:$execution_domain,
      DictArrayAttr:$deferred_body, DictArrayAttr:$deferred_cfg);
  let hasVerifier = 1;
}

def Metreon_ProcedureOp : Metreon_CallableContract<"procedure"> {
  let summary = "Preserve procedure authority, effects and ownership contracts";
}
def Metreon_KernelOp : Metreon_CallableContract<"kernel"> {
  let summary = "Preserve GPU kernel execution and ownership contracts";
}
def Metreon_StageOp : Metreon_CallableContract<"stage"> {
  let summary = "Preserve a pipeline stage contract exactly once";
}

class Metreon_DeclarationGroup<string mnemonic> : Metreon_Op<mnemonic, [
    Symbol, SymbolTable, IsolatedFromAbove, GraphRegionNoTerminator,
    DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
      OptionalAttr<StrAttr>:$sym_visibility, DictionaryAttr:$semantic_properties,
      DictArrayAttr:$records, ArrayAttr:$entry_states);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
}
def Metreon_ProtocolOp : Metreon_DeclarationGroup<"protocol"> {
  let summary = "Declare protocol roles and message state machines";
}
def Metreon_PipelineOp : Metreon_DeclarationGroup<"pipeline"> {
  let summary = "Declare pipeline parameters and semantic members";
}
def Metreon_ProtocolStateOp : Metreon_DeclarationGroup<"protocol_state"> {
  let summary = "Declare a protocol state and its messages";
}

class Metreon_CompositionDeclaration<string mnemonic> : Metreon_Op<mnemonic, [
    Symbol, DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
      OptionalAttr<StrAttr>:$sym_visibility, DictionaryAttr:$semantic_properties,
      DictArrayAttr:$records);
  let hasVerifier = 1;
}
def Metreon_MessageDeclOp : Metreon_CompositionDeclaration<"message_decl">;
def Metreon_PipelineStateOp : Metreon_CompositionDeclaration<"pipeline_state">;
def Metreon_EdgeOp : Metreon_CompositionDeclaration<"edge">;
def Metreon_TopologyOp : Metreon_CompositionDeclaration<"topology">;
def Metreon_ScheduleOp : Metreon_CompositionDeclaration<"schedule">;

#endif // METREON_STRUCTURE_OPS_TD
