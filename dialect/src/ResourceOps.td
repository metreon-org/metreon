#ifndef METREON_RESOURCE_OPS_TD
#define METREON_RESOURCE_OPS_TD

include "MetreonOpBase.td"
include "mlir/IR/RegionKindInterface.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def Metreon_ResourceLifecycleAttr : StringBasedAttr<And<[
  StrAttr.predicate,
  CPred<"::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"unspecified\" || "
        "::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"must_complete\" || "
        "::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"terminal\" || "
        "::llvm::cast<::mlir::StringAttr>($_self).getValue() == \"droppable\"">
]>, "resource lifecycle ('unspecified', 'must_complete', 'terminal' or 'droppable')">;

def Metreon_ResourceOp : Metreon_Op<"resource", [
  Symbol, SymbolTable, IsolatedFromAbove, GraphRegionNoTerminator,
  DeclareOpInterfaceMethods<SymbolUserOpInterface>
]> {
  let summary = "Declare a nominal resource and its state machine";
  let description = [{
    Imports an ordinary GraphIR ResourceGraph as a symbol table containing
    `metreon.state` and `metreon.transition_decl` symbols. Protocol/pipeline
    membership containers are not ordinary resource declarations.

    `generic_parameters` is an ordered array of parameter dictionaries with
    names and structured constraints. `entry_states` contains references to
    direct state members, relative to this resource's symbol table. The body
    is one declaration block with no arguments or terminator. Collapsed
    GraphIR state containers must be expanded by the importer into their
    original logical states and internal transitions before constructing it.

    The declared verifier hooks require parameter checks, declaration-only
    contents, and an argument-free body. Symbol-use verification must require
    distinct entry references that resolve to states owned by this resource.
    Structural C++ checks accompany the importer.

    This operation produces no runtime value. Physical lowering specializes
    ordered state payloads, converts every resource use and callable signature
    consistently, then erases the declaration once no symbolic uses remain.
  }];

  let arguments = (ins
    SymbolNameAttr:$sym_name,
    OptionalAttr<StrAttr>:$sym_visibility,
    DictArrayAttr:$generic_parameters,
    FlatSymbolRefArrayAttr:$entry_states
  );
  let regions = (region SizedRegion<1>:$body);

  let hasVerifier = 1;
}

def Metreon_StateOp : Metreon_Op<"state", [
  Symbol, HasParent<"ResourceOp">,
  DeclareOpInterfaceMethods<SymbolUserOpInterface>
]> {
  let summary = "Declare an ordered resource-state payload and lifecycle";
  let description = [{
    Imports a logical ResourceStateNode as a resource-local symbol. Each entry
    in `fields` is a dictionary containing `name`, an actual MLIR `type`
    attribute, an `ownership` mode, and structured `source_type` information
    retaining facts such as source integer signedness. Field order determines
    the specialized payload layout and must not be sorted as a set.

    `accumulator_scope`, when present, preserves the structured scope of a
    GraphIR accumulator. `lifecycle` records an explicit completion/drop policy.
    The default `unspecified` preserves declarations whose lifecycle is not
    defined yet; it does not grant permission to drop the resource. A graph
    sink alone is not evidence of terminal or droppable status.

    The declared verifier hook requires field-name, type and ownership checks,
    recursive exclusion of context evidence from payloads, and scope/lifecycle
    validation. Structural C++ checks accompany the importer; full lifecycle
    analysis remains a separate pass.
    This declaration has no runtime operands, results, or region. Lowering
    uses its ordered payload and lifecycle contract; it emits no instruction
    merely because the declaration exists.
  }];

  let arguments = (ins
    SymbolNameAttr:$sym_name,
    OptionalAttr<StrAttr>:$sym_visibility,
    DictArrayAttr:$fields,
    OptionalAttr<DictionaryAttr>:$accumulator_scope,
    DefaultValuedAttr<Metreon_ResourceLifecycleAttr, "\"unspecified\"">:$lifecycle
  );

  let hasVerifier = 1;
}

def Metreon_TransitionDeclOp : Metreon_Op<"transition_decl", [
  Symbol, IsolatedFromAbove, HasParent<"ResourceOp">,
  DeclareOpInterfaceMethods<SymbolUserOpInterface>
]> {
  let summary = "Declare a resource transition contract and implementation";
  let description = [{
    Imports a ResourceTransitionEdge as a declaration distinct from an
    executable `metreon.transition`. `source_state` and `target_state` refer
    to sibling state symbols in the enclosing resource.

    `function_type` contains only runtime parameters and one successor resource
    result. Runtime parameters retain source order, with context evidence
    excluded. `self_index` identifies the consumed source resource in that
    signature; self need not be the first source parameter. `parameter_attrs`
    and `result_attrs` contain ordered ownership and structured `source_type`
    metadata with one dictionary per runtime parameter/result.

    `generic_parameters`, `context_requirements`, and `effects` are structured
    dictionary arrays. Context requirements refer to context symbols or generic
    context parameters, never runtime inputs. Effects form a canonical set.
    `is_await` retains the source modifier. Optional `completion` metadata
    supplies a resolved completion kind/domain and failure contract; the await
    modifier alone does not select a wait implementation.

    The body region is empty for an unimplemented declaration or one using a
    versioned `implementation` binding. A checked implementation body, when
    supplied, is an isolated SSACFG region with explicit terminators and entry
    arguments matching the runtime signature. A binding and a body are mutually
    exclusive. Import may retain a signature-only declaration; executable
    conversion must reject it until a verified implementation is available.

    The declared verifier hooks require signature, self-index/consume-mode,
    successor-type, metadata-length/structure, and implementation/body checks.
    Symbol-use verification must resolve state and context contracts and check
    resource ownership. Structural signature and symbol checks accompany the
    importer; whole-callable ownership analysis remains separate.
    Lowering invokes the verified transition implementation
    and constructs only the declared successor payload; a signature is never
    permission to reinterpret states or discard completion work.
  }];

  let arguments = (ins
    SymbolNameAttr:$sym_name,
    OptionalAttr<StrAttr>:$sym_visibility,
    FlatSymbolRefAttr:$source_state,
    FlatSymbolRefAttr:$target_state,
    TypeAttrOf<FunctionType>:$function_type,
    ConfinedAttr<I64Attr, [IntNonNegative]>:$self_index,
    DictArrayAttr:$parameter_attrs,
    DictArrayAttr:$result_attrs,
    DictArrayAttr:$generic_parameters,
    DictArrayAttr:$context_requirements,
    DictArrayAttr:$effects,
    BoolAttr:$is_await,
    OptionalAttr<DictionaryAttr>:$completion,
    OptionalAttr<DictionaryAttr>:$implementation
  );
  let regions = (region AnyRegion:$body);

  let hasVerifier = 1;
  let hasRegionVerifier = 1;
}

def Metreon_TransitionOp : Metreon_Op<"transition", [
  DeclareOpInterfaceMethods<SymbolUserOpInterface>,
  MemoryEffects<[MemRead, MemWrite, MemAlloc, MemFree]>
]> {
  let summary = "Consume a resource state and invoke its declared transition";
  let description = [{
    Imports a GraphIR call with `callee_kind=transition`. `callee` identifies a
    `metreon.transition_decl`, for example @CopySlot::@complete. `arguments`
    contains runtime operands in declaration order, including the consumed
    self operand at the declaration's self index. `successor` is the single
    instantiated target resource state, which is intrinsically owned.

    `type_arguments` preserves ordered generic substitutions as type or
    structured parameter attributes, including compile-time context references.
    Optional `context` is the effective ambient context symbol after resolving
    direct, hoisted-region, and callable context metadata. It is not an SSA
    operand. Effects, ownership modes, and completion behavior come from the
    resolved declaration, not untrusted copies at the call site.

    The declared verifier hooks require substitution and local resource-type
    checks. Symbol-use verification must resolve the declaration and context,
    instantiate the signature, and check operand/result compatibility. Whole-callable
    ownership/effect/async passes must additionally prove legal consumption,
    capabilities, and completion. These C++ checks accompany later chunks.

    Conservative read/write/allocate/free effects on the default MLIR resource
    prevent treating an unknown transition as memory-effect-free. This can be
    refined to a shared abstract protocol resource and concrete storage effects
    once alias/provenance analysis exists. The operation is not speculatable.

    Executable lowering requires a checked body or implementation binding and
    converts the complete resource ABI consistently. Representation-preserving
    erasure needs an explicit pure implementation contract; matching payload
    types alone are insufficient.
  }];

  let arguments = (ins
    SymbolRefAttr:$callee,
    Variadic<AnyType>:$arguments,
    ArrayAttr:$type_arguments,
    OptionalAttr<SymbolRefAttr>:$context
  );
  let results = (outs AnyType:$successor);

  let hasVerifier = 1;
}

#endif // METREON_RESOURCE_OPS_TD
