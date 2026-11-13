# GraphIR to CIRCT seq contract

`GraphIRToSeq.td` is a declarative architecture schema, exported as
`GraphIRToSeq.json` by the existing optional MLIR/TableGen build. It defines
obligations for a future conversion, not executable rewrite patterns or a
registered pass. No CIRCT library or new backend is linked into `metreonc`.

The hardware path is:

```text
.mtr -> existing AST/frontend -> GraphIR -> default GraphIR passes
     -> typed hardware realization of graphir::Module -> CIRCT hw/comb/seq
```

The conversion boundary consumes the post-optimization **GraphIR object**.
It does not parse `.mtr` or printed GraphIR and does not require the separate
GraphIR-to-Metreon-MLIR importer. A future `lowerGraphIRToSeq` library will
allocate CIRCT modules/ports only after resolving GraphIR types, layouts,
implementation bindings, clocks, reset, and the selected hardware schedule.

## Context and grant profiles

`../MetreonHardwareProfiles.td` records `Fpga::DataflowStage<Clock>` and
`Tpu::TensorCore`, preserving these example contracts:

| Context | Grant | Ordered arguments |
|---|---|---|
| FPGA stage | `fifo_reserve` | Port |
| FPGA stage | `stream_enqueue` | Port |
| FPGA stage | `clock_wait` | Clock domain |
| TPU core | `dma_copy` | Source space, destination space |
| TPU core | `await_completion` | Completion kind |
| TPU core | `tensor_compute` | Element type |
| TPU core | `semaphore_signal` | Scope |

These catalogs describe recognized contracts, not automatically granted
authority or a global restriction on user-defined capabilities. Exact grants
still come from each compiler-minted `ContextMetadata` and its grant nodes.
Generic substitution precedes exact membership checks. Separate nominal
contexts remain distinct even when their names and grants match.

The tracked context example contains only the relevant declarations. The
exploratory files under `tmp/` remain ignored and are not build inputs. Their
type aliases, linear-resource syntax, completion types and full workloads are
not implemented by adding these profiles. The existing generic frontend can
already preserve the context applications and enforce their grant requirements
on procedures. GPU-only kernel rules are unchanged.

## Realization rules

| GraphIR input | Planned CIRCT realization | Required distinction |
|---|---|---|
| Context metadata and grant nodes | Verify then erase evidence; bind the declared clock to a module clock port | A context never becomes a clock SSA value or runtime capability. |
| Instantiated resource payload fields | `seq.compreg` or `seq.compreg.ce` when reset-agnostic data-register semantics fit | A resource state declaration by itself allocates no register. |
| Realized protocol/control state | Explicit-reset `seq.firreg` and `comb` next-state logic | Storage is introduced by a checked implementation and schedule. |
| Bound stream payload/edge | `seq.fifo` plus valid/payload holding registers | Preserve ordering, capacity, sidebands, reset, and read latency. |
| `ResourceTransitionEdge` plus resolved transition call | Checked reservation/enqueue circuitry | Transition signatures and await modifiers alone are not implementations. |
| Await transition plus selected schedule | Explicit stage advance/hold control | A clock wait is not a host sleep, TPU completion, or CDC adapter. |

Normalize collapsed `ResourceStateNode.members` and `internalTransitions` once,
recover logical edge endpoints, propagate hoisted `ContextRegion` identity, and
rebuild callable CFG after optimization. Composition records describe storage
and selected stage invocations; they do not duplicate execution. Preserve
source locations and reject unresolved or unsupported realizations.

The first FIFO profile has one clock, depth 16 and read latency 1. Its output
holding stage must track pending reads and hold data/sidebands stable under
backpressure. Queued occupancy includes physical FIFO entries, pending reads
and held output. Restore credit on an accepted consumer transfer, not internal
prefetch. Maintain `free + reserved + queued = capacity`, including simultaneous
events. An affine permit belongs to one port and is consumed exactly once.
Reset empties the FIFO/output state, clears pending reads/reservations and sets
free credit to capacity. Reject overflow, underflow and implicit clock crossings.

TPU HBM/VMEM DMA, LocalDma completion, BF16 computation and core semaphore
operations require a separate explicit accelerator binding. Their declarations
do not synthesize a TPU circuit. The schema rejects using that profile in a
`GraphIRToSeqRule`; no TPU-to-seq mapping is claimed.

The target operation contracts were checked against the upstream
[Seq definitions](https://github.com/llvm/circt/blob/main/include/circt/Dialect/Seq/SeqOps.td)
and [Seq rationale](https://circt.llvm.org/docs/Dialects/Seq/RationaleSeq/).
Pin CIRCT with its matching LLVM revision before implementing conversion or
claiming target verification, simulation or RTL generation. Generating these
manifests uses `llvm-tblgen` from the LLVM installation selected by MLIR.

```sh
cmake --build build-mlir --target metreon_mlir
ctest --test-dir build-mlir -R 'accelerator_contexts|hardware_schemas' --output-on-failure
```
