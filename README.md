# metreon
Metreon is a statically typed Domain Specific Language (DSL) that aims to verify execution contexts, effects, ownership, resource lifecycles, and asynchronous protocol topology in low-level GPU pipelines

## Run the frontend examples

```sh
cmake -S . -B build
cmake --build build --parallel
./build/metreonc --emit=graphir examples/contexts.mtr
./build/metreonc --emit=graphir examples/resources.mtr
```

The current compiler parses and validates `.mtr` context declarations and
resource state machines, then emits textual GraphIR with separate context and
resource graphs. It does not yet generate or execute runtime or GPU code.

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

## Context evidence

GraphIR represents a context as immutable compiler metadata, not as a graph node
or runtime value. Every context-owned node references metadata containing a
compiler-minted opaque key. The key is nominally unique to its module and has no
source-level constructor or runtime representation; GraphIR rejects attempts to
store, transmute, cast, send, or capture it. This is a compiler provenance and
type-system invariant, not a secret embedded in generated code. The printed key
is only a reproducible, module-local diagnostic symbol; it is not accepted as a
user-provided bearer token.
