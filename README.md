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
./build/metreonc --emit=graphir examples/procedures.mtr
./build/metreonc --emit=graphir examples/protocols_and_pipelines.mtr
cat tmp/graphir/procedures.graphir
```

The compiler saves GraphIR to `tmp/graphir/` relative to its current working
directory and prints only the saved path. For example, running from `compiler/`
produces `compiler/tmp/graphir/procedures.graphir`, while invoking
`compiler/build/metreonc` from the parent directory produces
`tmp/graphir/procedures.graphir` in that parent directory. The output location
does not depend on the source file or executable location. When running inside
this checkout, the existing `/tmp/` rule in `.gitignore` excludes the generated
artifacts; other working directories use their own ignore rules.

The default invocation and `--emit=graphir` both save a file. Reading from stdin
with `-` produces `stdin.graphir`. Recompiling the same input stem replaces its
previous output; inputs in different directories with the same stem share that
output filename. Source validation finishes before opening the output file, so
invalid programs do not replace a previous artifact. Directory or file-write
failures produce an error and a nonzero exit status.

GraphIR optimization runs by default, including when `--emit` is omitted.
`--emit graphir` is equivalent to `--emit=graphir`. To emit the original lowered
graph and skip all four passes, including fusion verification, use:

```sh
./build/metreonc --emit graphir --no-optimization examples/optimizations.mtr
```

Without `--no-optimization`, that example removes an unreachable protocol state,
collapses the resource chain, and groups common context evidence in callable
bodies and the fused worker. Both modes still perform frontend validation, and
pass failures preserve any previous output file.

## Inspect Metreon MLIR

Enable MLIR with an LLVM/MLIR installation whose versions match:

```sh
cmake -S . -B build-mlir -DMETREON_ENABLE_MLIR=ON \
  -DMLIR_DIR=/path/to/llvm/lib/cmake/mlir \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build-mlir --parallel
./build-mlir/metreonc --emit=mlir examples/procedures.mtr
cat tmp/mlir/procedures.mlir
./build-mlir/metreon-opt tmp/mlir/procedures.mlir -o /tmp/procedures.checked.mlir
```

The implementation is checked against LLVM/MLIR 18.1.3. `--emit mlir` also works;
stdin produces `tmp/mlir/stdin.mlir`. Output follows the same working-directory
and validation-before-replacement rules as GraphIR. GraphIR remains the default,
and builds with MLIR disabled diagnose an explicit MLIR emission request.

This route is `.mtr -> validated GraphIR -> default GraphIR passes -> registered
Metreon semantic MLIR`. `--no-optimization` skips GraphIR passes; import verification
still runs. Contexts/grants, resources and typestates, transition declarations,
callable authority/effect/ownership contracts, protocols, and pipeline topology
and schedules use `metreon` operations. Contexts have symbols and attributes,
never runtime SSA values. Collapsed resource members and internal transitions are
reconstructed; hoisted context evidence is resolved for each deferred call.

This is a structural import stage. Ordinary expressions, local bindings, calls,
returns, and CFGs remain in explicit `deferred_body`/`deferred_cfg` attributes
for later direct conversion to standard MLIR dialects. They do not become custom
`metreon.literal`, `metreon.variable`, loop, or branch operations. Builtin scalar
types are reused; unresolved storage/layout/generic types remain structured
`!metreon.unresolved` types. Resource states are intrinsically owned
`!metreon.resource` types; other owned payloads use `!metreon.owned`.

The module records `metreon.import_stage = "semantic_structure"` and
`metreon.ordinary_lowering = "deferred"`. Verification checks the registered
schemas, symbol references, context identity, transition signatures, and imported
value/scope consistency. It does not establish complete lifecycle/async ownership
verification or executable host/GPU code. Transition signatures remain
declarations without fabricated implementations. No FPGA/CIRCT backend is added.

## Loops and conditionals

The frontend accepts `if`/`else if`/`else`, `loop`, `for`, `while`, `until`,
`break`, and `continue`. See [loops_and_conditionals.mtr](examples/loops_and_conditionals.mtr)
for complete host and GPU examples:

```metreon
context Host::Loop repeat_ctx;

procedure count() -> i32 {
    i32 value = 0;
    until<@cx: repeat_ctx> (value == 4 is true) {
        value = value + 1;
    }
    if (value == 4) {
        return value;
    } else {
        return 0;
    }
}
```

Loop contexts have no grants and cannot serve as callable execution authority.
Use `Host::Loop` for host callables and `Gpu::Loop` for GPU callables; a
context-independent procedure defaults to the host domain for this check.
The existing grants syntax with an empty set is also accepted for loop contexts.

`until` checks its condition before each iteration and stops when it equals the
explicit `is true` or `is false` value. `while` also checks before each iteration.
Plain `loop (@cx: repeat_ctx)` repeats until an exit. `break` exits the nearest
loop; `continue` rechecks its predicate, or executes a `for` step before testing
the next element. A nested `return` always exits the callable.

`for<@cx: repeat_ctx> (i32 item) in 0..4` visits a half-open integer range;
both bounds are evaluated once. Equal or descending bounds produce no iterations.
Container iteration supports `view<Element, AddressSpace>` with a matching,
read-only iteration variable. View extent/layout and iterator realization remain
deferred to later primary-dialect lowering.

Expressions support parentheses, unary `!`/`+`/`-`, arithmetic `+ - * / %`,
comparisons `== != < <= > >=`, and short-circuit `&&`/`||` with conventional
precedence. Assignment supports mutable, unowned scalar locals and parameters.
Conditions require `bool`. Initialization is checked on every continuing path;
owned values cannot be consumed again through a loop backedge.

```sh
./build/metreonc --emit=graphir examples/loops_and_conditionals.mtr
cat tmp/graphir/loops_and_conditionals.graphir
```

Control-flow bodies are organized within the existing `graphir.cfg` section:
basic blocks contain the computations, `graphir.cfg.cond_br` records both edges,
and ordinary branches record joins, backedges, breaks, and continues. Loop block
metadata identifies its role and nominal loop context. Mutable locals use explicit
local reads/writes, so a predicate observes the current value on every iteration.
There is no separate loop or conditional section. Existing straight-line dumps
retain their earlier representation for compatibility.

The optional MLIR importer preserves the new operation trees and CFG edges as
deferred body metadata. It does not create Metreon arithmetic, loop, or branch
operations. This frontend/GraphIR support does not establish native/GPU execution
or hardware clock timing.

## GraphIR pass APIs

The independent pass APIs are in `metreon/GraphIR/Passes.h`, under
`metreon::graphir::passes`. `lowerModule` produces unoptimized GraphIR for library
callers; `runDefaultPipeline(module)` applies the same pipeline as the CLI:

1. `FusionLegalityPass` checks nominal context compatibility, topology and
   stage-call dependencies, and dependency order within each `fuse` list. It
   rejects existing cycles and cycles introduced by contracting fused workers,
   including a fusion of `A` and `C` around an unfused `B` in `A -> B -> C`.
2. `DeadTransitionEliminationPass` removes unreachable resource states and edges
   and unreachable protocol states/messages. Resource roots include the first
   declared state, as required by existing source validation, and additional
   typed state references. Protocol roots include channel initial states and
   typed endpoint states across all pipelines. Graphs without known entries are
   retained. Context-template checks and definition/member references are
   updated with removals. Source validation continues to reject unreachable
   resource declarations; the pass also supports graphs changed after lowering.
3. `StateChainCollapsePass` collapses resource links with a single successor and
   a single predecessor into one node. Cycles and independently referenced entry
   states remain boundaries. `graphir.state_member` records retain each original
   state's ID, fields, accumulator metadata, and source location. Ordered
   `graphir.internal_transition` records retain all transition contracts,
   including waits, effects, parameters, and ownership types. Collapsing a graph
   never authorizes skipping those transitions or entering at the wrong member.
4. `ContextHoistingPass` groups consecutive uses of the same nominal evidence in
   `graphir.context_region` operations. Scalar setup between calls keeps its
   execution order; lexical blocks, returns, other contexts, and context-free
   calls are boundaries. These regions supply inherited evidence and introduce
   neither runtime values nor lexical ownership scopes. Fused workers also
   record common-context stage groups by reference, without invoking stages
   again. Existing call order, value IDs, effects, and CFGs are preserved.

Passes may also be run individually with `PassName{}.run(module)`. Fusion checks
cover the current structural GraphIR model; GPU execution, target resource
limits, barrier participation, and general runtime scheduling remain future work.

The current compiler parses and validates `.mtr` context declarations and
resource state machines, kernels, and procedures, then emits textual GraphIR
with context metadata, resource graphs, and ordered callable bodies. Procedures
support typed parameters and return types, resource-transition calls, nested
blocks, and execution-context/effect requirements. It does not yet generate or execute runtime or GPU code.

## Protocols and pipelines

The frontend also accepts the concrete protocol/pipeline syntax in
`examples/protocols_and_pipelines.mtr`: protocol roles, message payloads and
successor states, final states, pipeline input/output parameters, scalar state
properties, bounded channel edges, stages, topology connections, and schedules
with fused workers and warp placements. Stage bodies use the same declarations,
calls, nested blocks, returns, context checks, and effects as procedures. Stage
names are local to their pipeline; calls to other local stages resolve within
that pipeline, including forward calls.

GraphIR emits `graphir.protocol` and `graphir.pipeline` sections. States, edges,
stages, topology, and schedules have ordinary definitions with qualified names.
Each owning `graphir.resource_graph` contains `graphir.definition_ref` nodes;
the protocol/pipeline sections refer to those nodes using `graphir.member_ref`.
Messages, connections, and fusion records retain their resolved definition
references, and stage control flow appears in `graphir.cfg.stage`.

The compiler checks names, roles, states, payload and parameter types, state
initializers, positive channel capacities and block counts, topology direction
and type compatibility, and schedule membership/placement. Invalid source is
rejected before replacing an existing output file.

This is structural frontend support for concrete declarations. Tensor shapes,
const generics, async channel operations, automatic scheduling, target hardware
validation, GPU lowering, and runtime execution are not implemented by this
example. `TACHYON_MASTER_PLAN.md` remains a broader language-design document.

