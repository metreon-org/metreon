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

## GraphIR passes

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

