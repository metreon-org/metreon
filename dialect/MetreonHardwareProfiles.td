#ifndef METREON_HARDWARE_PROFILES_TD
#define METREON_HARDWARE_PROFILES_TD

// Build-time semantic contracts, not operations or runtime context values.
// Profiles describe supported grant applications; authority still comes from
// each compiler-minted GraphIR context identity and its declared grant nodes.
class MetreonGrantProfile<string name, list<string> argumentKinds,
                          string contract> {
  string mnemonic = name;
  list<string> argument_kinds = argumentKinds;
  int arity = !size(argumentKinds);
  string semantics = contract;
  assert !ne(name, ""), "a grant must have a name";
}

def Metreon_FifoReserve : MetreonGrantProfile<"fifo_reserve", ["port"],
    "Reserve one slot on the named port; produce one affine port-bound permit.">;
def Metreon_StreamEnqueue : MetreonGrantProfile<"stream_enqueue", ["port"],
    "Consume a permit and owned payload for the same port; preserve message order.">;
def Metreon_ClockWait : MetreonGrantProfile<"clock_wait", ["clock_domain"],
    "Permit stage suspension until a legal clocked step; not a host sleep or CDC.">;

def Metreon_DmaCopy : MetreonGrantProfile<"dma_copy",
    ["source_address_space", "destination_address_space"],
    "Issue an ordered-space DMA and retain its storage until matching completion.">;
def Metreon_AwaitCompletion : MetreonGrantProfile<"await_completion",
    ["completion_kind"],
    "Resolve a matching owned completion request without duplicating wait rights.">;
def Metreon_TensorCompute : MetreonGrantProfile<"tensor_compute", ["element_type"],
    "Authorize target-supported tensor computation for the declared element type.">;
def Metreon_SemaphoreSignal : MetreonGrantProfile<"semaphore_signal", ["scope"],
    "Signal the named accelerator scope with an explicitly bound memory order.">;

class MetreonContextProfile<string name, list<string> argumentKinds,
    list<MetreonGrantProfile> grants, bit seqEligible> {
  string context_name = name;
  list<string> argument_kinds = argumentKinds;
  int arity = !size(argumentKinds);
  list<MetreonGrantProfile> supported_grants = grants;
  bit seq_eligible = seqEligible;
  bit runtime_materializable = 0;
  string identity = "nominal declaration identity; never structural grant equality";
  assert !ne(name, ""), "a context profile must have a name";
  assert !gt(!size(grants), 0), "a profile must describe at least one grant";
}

def Metreon_FpgaDataflowStage : MetreonContextProfile<
    "Fpga::DataflowStage", ["clock_domain"],
    [Metreon_FifoReserve, Metreon_StreamEnqueue, Metreon_ClockWait], 1> {
  string example_identifier = "writer_ctx";
  list<string> example_arguments = ["fpga::clk0"];
  list<string> example_grants = ["fifo_reserve<port::Output>",
      "stream_enqueue<port::Output>", "clock_wait<fpga::clk0>"];
  string lowering_domain = "synthesized single-clock fabric";
}

def Metreon_TpuTensorCore : MetreonContextProfile<"Tpu::TensorCore", [],
    [Metreon_DmaCopy, Metreon_AwaitCompletion, Metreon_TensorCompute,
     Metreon_SemaphoreSignal], 0> {
  string example_identifier = "tpu_core";
  list<string> example_arguments = [];
  list<string> example_grants = ["dma_copy<tpu::hbm, tpu::vmem>",
      "await_completion<tpu::LocalDma>", "tensor_compute<bf16>",
      "semaphore_signal<tpu::core>"];
  list<string> address_spaces = ["tpu::hbm", "tpu::vmem", "tpu::smem"];
  string lowering_domain = "external accelerator implementation binding";
}

#endif // METREON_HARDWARE_PROFILES_TD
