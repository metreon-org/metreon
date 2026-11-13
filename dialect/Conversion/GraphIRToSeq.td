#ifndef METREON_GRAPHIR_TO_SEQ_TD
#define METREON_GRAPHIR_TO_SEQ_TD

include "MetreonHardwareProfiles.td"

// Architecture records exported as JSON. These are deliberately not DRR
// patterns: logical typestate declarations alone cannot construct RTL storage.
class GraphIRToSeqRule<MetreonContextProfile profile, string subject,
    list<string> operations, list<string> obligations> {
  MetreonContextProfile context_profile = profile;
  string source_ir = "post-optimization graphir::Module";
  string source_object = "ResourceStateNode payload under a physical binding";
  string physical_subject = subject;
  list<string> target_operations = operations;
  list<string> preconditions = obligations;
  bit requires_physical_binding = 1;
  bit implemented = 0;
  assert profile.seq_eligible,
      "an external accelerator context cannot be lowered to seq";
  assert !gt(!size(operations), 0), "a seq rule needs target operations";
  assert !gt(!size(obligations), 0), "a seq rule needs semantic obligations";
}

class MetreonSeqFifoProfile<int depth, int readLatency> {
  int capacity = depth;
  int read_latency = readLatency;
  int clock_domains = 1;
  string clock_type = "!seq.clock";
  string reset = "synchronous active-high; empty FIFO and zero reservations";
  string conservation = "free + reserved + queued = capacity";
  string write_enable = "valid && ready && matching_permit_consumed";
  string read_enable = "output_slot_available && !read_pending && !fifo_empty";
  string consumer_transfer = "output_valid && consumer_ready";
  string queued = "fifo occupancy + pending read + valid output holding slot";
  string stalled_payload = "payload and sidebands remain stable until accepted";
  assert !gt(depth, 0), "FIFO capacity must be positive";
  assert !or(!eq(readLatency, 0), !eq(readLatency, 1)),
      "the first seq profile supports FIFO read latency zero or one";
}

def Metreon_OutputFifoProfile : MetreonSeqFifoProfile<16, 1>;

def Metreon_SeqContextErasure {
  string source_declaration = "graphir::ContextMetadata and grant Nodes";
  bit emits_runtime_context = 0;
  list<string> obligations = [
    "resolve exact nominal context and substituted grant applications",
    "bind the logical clock domain to an explicit hw.module clock port",
    "materialize all authorized runtime operations before erasing evidence",
    "reject dangling context references and implicit clock-domain crossings"
  ];
}

def Metreon_SeqPayloadRegister : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "instantiated physical payload register", ["seq.compreg"], [
      "payload width and layout are resolved and supported",
      "clock is an explicit !seq.clock value in the selected domain",
      "reset-agnostic semantics are sufficient for this data register",
      "reset and resetValue are both present or both absent",
      "uninitialized payload cannot be observed before its valid condition"
    ]>;

def Metreon_SeqEnabledRegister : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "instantiated enabled physical payload register", ["seq.compreg.ce"], [
      "all payload-register obligations hold",
      "clockEnable is an explicit i1, distinct from the physical clock",
      "disabled cycles retain the old value; no implicit clock gating"
    ]>;

def Metreon_SeqControlRegister : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "realized protocol state, credit counter, or valid-bit storage",
    ["seq.firreg", "comb.mux"], [
      "runtime state is required by the selected implementation",
      "use explicit synchronous active-high reset with a constant reset value",
      "input, result and reset value have identical resolved hardware types",
      "one next-state driver and one clock domain exist per register",
      "symbolic resource states do not independently allocate registers"
    ]> {
  let source_object = "ResourceGraph and selected CompositionDefinition schedule";
}

def Metreon_SeqOutputFifo : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "realized OutputStream queue", ["seq.fifo"], [
      "bind depth and read latency from Metreon_OutputFifoProfile",
      "preserve payload and Tlast sideband together in the selected layout",
      "implement ready/valid with explicit full, empty and read-latency behavior",
      "physical FIFO and reservation accounting share the declared clock/reset",
      "read-latency one requires a valid/payload holding register and pending read",
      "hold output stable while stalled; internal prefetch does not free credit",
      "reject unsupported target FIFO configurations instead of changing them"
    ]> {
  let source_object = "CompositionDefinition edge or bound ResourceStateNode field";
}

def Metreon_SeqReserve : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "bound StreamWriter::reserve implementation",
    ["seq.firreg", "comb.icmp", "comb.add", "comb.sub"], [
      "require fifo_reserve for the exact port and clock_wait for its clock",
      "issue one affine port-bound permit only when free credit is positive",
      "account for reservations not yet written into the physical FIFO",
      "preserve free + reserved + queued = capacity on simultaneous actions",
      "waiting is a stalled stage, not host blocking or a new clock"
    ]> {
  let source_object = "ResourceTransitionEdge and resolved CallableOperation call";
}

def Metreon_SeqEnqueue : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "bound StreamWriter::send implementation", ["seq.fifo", "seq.firreg"], [
      "require stream_enqueue for the exact port",
      "consume one matching permit and owned beat at the accepted transfer",
      "convert reserved credit into queued occupancy exactly once",
      "release credit only on accepted consumer transfer, not internal prefetch",
      "prevent underflow, overflow and reordering"
    ]> {
  let source_object = "ResourceTransitionEdge and resolved CallableOperation call";
}

def Metreon_SeqClockWait : GraphIRToSeqRule<Metreon_FpgaDataflowStage,
    "scheduled stage wait", ["seq.firreg", "comb.mux"], [
      "require clock_wait for the same logical and physical clock domain",
      "derive explicit stage advance/hold conditions from the selected schedule",
      "an await modifier alone does not define a number of cycles or completion",
      "reject host waits, TPU completion requests and clock-domain crossings"
    ]> {
  let source_object = "await ResourceTransitionEdge and selected stage schedule";
}

def Metreon_GraphIRToSeqBoundary {
  string source_ir = "post-optimization graphir::Module";
  list<string> target_dialects = ["hw", "comb", "seq"];
  bit requires_metreon_mlir_import = 0;
  bit parses_mtr_source = 0;
  bit implemented = 0;
  list<string> obligations = [
    "consume the GraphIR object; never parse .mtr or textual GraphIR here",
    "reconstruct collapsed state members and internal transitions once",
    "recover effective context through hoisted ContextRegion operations",
    "rebuild callable CFG with buildControlFlow after GraphIR optimization",
    "resolve storage, clocks, reset, layout and schedule before constructing CIRCT"
  ];
}

def Metreon_TpuBindingBoundary {
  MetreonContextProfile context_profile = Metreon_TpuTensorCore;
  bit seq_eligible = 0;
  list<string> target_operations = [];
  list<string> obligations = [
    "bind TPU DMA, completion, tensor compute and semaphore semantics explicitly",
    "preserve hbm, vmem and smem identity and ordered DMA direction",
    "retain source/destination ownership until matching LocalDma completion",
    "validate bf16 support, shapes, layouts, scope and failure outcomes",
    "do not synthesize a physical TPU or fabricate seq operations from a grant"
  ];
}

#endif // METREON_GRAPHIR_TO_SEQ_TD
