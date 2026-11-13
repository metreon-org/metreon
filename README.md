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

The `--emit=mlir` importer preserves the operation trees and CFG edges as deferred
body metadata. It does not create Metreon arithmetic, loop, or branch operations.
Executable host scalar conversion is available separately through `--emit=llvm`,
as described below. GPU execution and hardware clock timing remain outside that path.

## Lower host scalar control flow to LLVM

An MLIR-enabled build accepts `--emit=llvm` (or `--emit llvm`) and writes
LLVM-dialect MLIR to `tmp/llvm/<input-stem>.mlir`. Stdin uses `stdin.mlir`.
GraphIR remains the default output format, and semantic `--emit=mlir` retains its
existing behavior.

```sh
./build-mlir/metreonc --emit=graphir tests/Inputs/HostControlFlow.mtr
./build-mlir/metreonc --emit=llvm tests/Inputs/HostControlFlow.mtr
cat tmp/graphir/HostControlFlow.graphir
cat tmp/llvm/HostControlFlow.mlir
./build-mlir/metreon-opt tmp/llvm/HostControlFlow.mlir -o /tmp/host.checked.mlir
```

The lowering consumes the current **in-memory GraphIR**, following frontend
validation and the selected GraphIR optimization pipeline. It repeats the
existing semantic-import checks on that exact object: nominal context identity,
module ownership, declarations and call contracts, CFG value availability,
lexical scopes, and definite initialization. It then checks host eligibility and
the supported type/operation subset. A stale verification marker cannot authorize
conversion, and an import/lowering failure does not replace the previous artifact.

Conversion follows this route:

```text
.mtr -> validated GraphIR -> default GraphIR passes
     -> fresh semantic/CFG checks and host eligibility checks
     -> func + arith + cf + rank-0 memref locals
     -> one shared LLVM type converter and full dialect conversion
     -> verified builtin.module containing only llvm operations
```

The existing GraphIR CFG supplies every branch, conditional edge, loop backedge,
break/continue destination, and return. The backend does not reconstruct control
flow from source text or parse a GraphIR dump. Mutable scalar slots allocate once
per function invocation; their initialization stores remain at their original
GraphIR execution points. These scalar slots do not infer a layout for a source
buffer or view.

The supported subset comprises host/context-independent procedures, direct calls
between those procedures, unowned integer/Boolean/floating scalars, assignment,
arithmetic, comparisons, short-circuit logic, conditionals, and integer-range
loops. `index` uses the native host pointer width; the module records the native
target triple. This is a native-host profile, not a cross-compilation interface.
Signedness is checked from GraphIR source types before equal-width integer types
are converted. Integer literals must fit their destination types; floating
constants round to the destination precision with round-to-nearest, ties-to-even,
preserving signed zero and explicit infinity. Arithmetic otherwise follows
standard MLIR/LLVM scalar semantics.

Execution-context parameters and loop evidence have no runtime ABI arguments;
they are erased only after the applicable checks. Resource ownership, transition
implementations, host runtime adapters, and unrefined view/buffer layouts remain
unsupported for executable conversion. Container loops therefore require a future
explicit extent/layout/access contract. Modules containing kernels or pipeline
execution are rejected by this output mode. No GPU or FPGA/ASIC execution
conversion is added.

The tracked [host fixture](tests/Inputs/HostControlFlow.mtr) exercises all four loop
forms, both `until` stop values, branches, breaks, continues, and a checked host
procedure call. Its LLVM `main` has no context argument and returns 42. Tests
check structural validity, GraphIR preservation, rejected unsafe/unsupported
inputs, and previous-artifact preservation. When matching `mlir-translate` and
`lli` tools are available, CTest also translates the output to LLVM IR and executes
host cases in both GraphIR optimization modes. This output mode emits MLIR; it
does not itself link a standalone executable.

## Lower GPU scalar control flow to Affine and SCF

An MLIR-enabled build accepts `--emit=gpu` (or `--emit gpu`), producing
`tmp/gpu/<input-stem>.mlir`. Stdin uses `tmp/gpu/stdin.mlir`.

```sh
./build-mlir/metreonc --emit=graphir tests/Inputs/GPUControlFlow.mtr
./build-mlir/metreonc --emit=gpu tests/Inputs/GPUControlFlow.mtr
cat tmp/gpu/GPUControlFlow.mlir
./build-mlir/metreon-opt tmp/gpu/GPUControlFlow.mlir -o /tmp/gpu.checked.mlir
```

The input is the current in-memory GraphIR after the selected GraphIR passes.
The existing semantic importer checks that exact graph again, including nominal
execution contexts, module ownership, call contracts, CFG dominance/scopes and
definite initialization. GPU eligibility checks require `Gpu::` execution
contexts and grant-free `Gpu::Loop` evidence. The same scalar CFG realization
used by the host backend produces `func`/`arith`/`cf` and private rank-0 local
slots. MLIR's memory-to-SSA and CFG-to-SCF transformations then preserve the
checked CFG's control flow as structured operations. There is no direct source
or printed-GraphIR input to executable lowering.

SCF normalization applies upstream scalar/branch/signature patterns while
restricting dead-code removal to regionless operations. It preserves empty and
conditionally nonterminating loops; a generic effect-based canonicalizer could
otherwise remove a result-free loop. Later backend pass pipelines must retain
this control-flow requirement as well.

The output preserves Metreon semantic contracts and adds a `gpu.module`.
Source kernels become `gpu.func ... kernel`; GPU-context procedures and their
reachable context-independent helpers become `func.func` device functions in
that module. Each lowered contract has a `metreon.device_body` symbol reference.
Other bodies, including host procedures, retain their deferred semantic form.
Context and loop evidence are checked metadata and never runtime GPU arguments.

Dialect selection is conservative:

| Control flow | Emission |
| --- | --- |
| Exact signed `<`/unit-step counted recurrence with an invariant upper bound and no observable final IV | `scf.for`, including loop-carried scalar values and structured continues |
| Counted loop with legal affine bounds and `index` or signed integer induction up to 32 bits | `affine.for`; integer IVs are widened to the 64-bit index profile and cast back for source arithmetic |
| Signed/equality comparisons of proven nonnegative, valid affine indices | `affine.if`; the nonnegative proof prevents overflow in constraint subtraction |
| General predicates, short circuiting and other branches | `scf.if` (or an equivalent canonicalized scalar select) |
| Data-dependent loops, `until`, unbounded loops, early exits, or unmatched/unsigned recurrences | `scf.while` with the CFG's exit and continuation semantics |

Affine dimension/symbol legality is checked in the final device region. In
MLIR 18, `gpu.func` has no `AffineScope` trait, so a kernel argument is not
automatically an affine symbol: a dynamic kernel bound can remain `scf.for`
while a constant-bound loop becomes `affine.for`. Nested affine IVs are used
only where their bounds satisfy the same legality checks. Bounds evaluated once
by GraphIR remain evaluated once; a mutable `while` bound is not frozen into a
counted loop. Unsigned ranges preserve unsigned comparisons in SCF, and integer
limits are not widened or truncated silently. `index` uses a 64-bit GPU scalar
profile at this stage; other index widths need a future target configuration.

This stage supports unowned Boolean/integer/floating scalars and direct device
procedure calls. It rejects resource transitions without implementations,
ownership-bearing values, unrefined buffers/views and container iteration
without extent/layout/access contracts. No new synchronization, memory transfer,
GPU launch, parallel work mapping, NVVM/SPIR-V conversion, device binary or
FPGA/ASIC lowering is provided. Serial source loops stay serial per work item.
Failures leave an existing output artifact intact.

Tests cover dialect selection and MLIR round-tripping, invalid/mutated GraphIR,
context evidence erasure, unchanged host bodies, both GraphIR optimization modes
and CLI failure behavior. With matching `mlir-opt`, `mlir-translate` and `lli`,
CTest also extracts the scalar device procedure bodies into a CPU-only test
module, lowers Affine/SCF through the standard conversions and checks their
results. These are serial control-flow regressions, not GPU hardware tests.

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

