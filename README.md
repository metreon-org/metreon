# metreon
Metreon is a statically typed Domain Specific Language (DSL) that aims to verify execution contexts, effects, ownership, resource lifecycles, and asynchronous protocol topology in low-level GPU pipelines

## Run the frontend examples

```sh
cmake -S . -B build
cmake --build build --parallel
./build/metreonc --emit=graphir examples/contexts.mtr
./build/metreonc --emit=graphir examples/resources.mtr
./build/metreonc --emit=graphir examples/accumulators.mtr
./build/metreonc --emit=graphir examples/kernel_variables.mtr
```

The current compiler parses and validates `.mtr` context declarations and
resource state machines, then emits textual GraphIR with separate context and
resource graphs. It does not yet generate or execute runtime or GPU code.

The frontend also accepts a deliberately minimal kernel body for C++-style
local declarations and literal initialization:

```metreon
kernel literal_initializers() {
  const index feature_dim = 64;
  f32 running_max = -infinity;
  bool enabled = true;
  f32 scratch;
}
```

Each local is emitted as a scoped `graphir.variable_decl` in a separate
`graphir.kernel` section, with type, mutability, automatic storage, initialized
state, and structured literal metadata. Initializers currently support integer,
floating-point, boolean, and signed `infinity` literals. Kernel parameters,
return types, assignments, expression initializers, control flow, and lowering
to MLIR are not yet supported.

Resource GraphIR ends with a template section that maps each transition context
parameter to every locally declared context. Each mapping records a non-fatal
`valid` or `invalid` result by matching the exact `where ... allows {...}`
capabilities, including generic arguments, against that context's grant nodes.
Promotion of invalid results to compilation errors is intentionally deferred to
a later IR optimization pass.

Every context declaration ends with a module-unique identifier after its grant
block, for example `} thread1;`. Multiple declarations may share the same
context type, such as `Gpu::Thread`, while their identifiers, grants, `#ctxN`
metadata, and resource-template validation remain distinct.

Context angle brackets accept either generic parameter declarations, such as
`Transport::TxQueue<N: Nic, Q: Queue>`, or concrete type arguments, such as
`Gpu::PersistentWorker<D, Uniform<Block>>`. Nested arguments including
`Uniform<Block>` and `Uniform<Cluster>` are preserved in context metadata.
Parameter declarations and concrete arguments cannot be mixed in one list.

Resource states may opt into persistent accumulator storage after their state
declaration with `accumulator State(scope);`. Supported scopes are `thread`,
`warp`, `block`, `device`, `cluster`, and `host::pinned`. GraphIR records the
selected scope in the corresponding `graphir.resource_state` metadata.

The first declared state is the resource entry state. GraphIR emission rejects
any state that cannot be reached from it through resource transitions. An
`await transition` must consume a source state containing an
`own event<...>` field and must provide matching `@cx: C` evidence for a
`C: Context` parameter with `where C allows {gpu_await}`. The transition effect
list is not part of this await-evidence check yet.

## Context evidence

GraphIR represents a context as immutable compiler metadata, not as a graph node
or runtime value. Every context-owned node references metadata containing a
compiler-minted opaque key. The key is nominally unique to its module and has no
source-level constructor or runtime representation; GraphIR rejects attempts to
store, transmute, cast, send, or capture it. This is a compiler provenance and
type-system invariant, not a secret embedded in generated code. The printed key
is only a reproducible, module-local diagnostic symbol; it is not accepted as a
user-provided bearer token.
