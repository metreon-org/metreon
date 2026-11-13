#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

metreon::graphir::Module lower(const std::string &source) {
  metreon::parser::Parser parser(source);
  const auto ast = parser.parseModule();
  return metreon::graphir::lowerModule(ast, "accelerator-contexts.mtr");
}

void rejects(const std::string &source, const char *code) {
  try {
    static_cast<void>(lower(source));
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == code, "unexpected accelerator diagnostic");
    return;
  }
  throw std::runtime_error("accepted an invalid accelerator context use");
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected the contexts fixture path");
    std::ifstream input(argv[1]);
    check(static_cast<bool>(input), "cannot open the contexts fixture");
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    const auto graph = lower(source + R"(
procedure fpga_probe(@cx: writer_ctx) -> void
  !{fifo_reserve<port::Output>, stream_enqueue<port::Output>, clock_wait<fpga::clk0>} {
  return;
}
procedure tpu_probe(@cx: tpu_core) -> void
  !{dma_copy<tpu::hbm, tpu::vmem>, await_completion<tpu::LocalDma>,
    tensor_compute<bf16>, semaphore_signal<tpu::core>} {
  return;
}
)");

    metreon::graphir::ContextMetadataRef fpga, tpu;
    for (const auto &context : graph.contexts()) {
      if (context->identifier() == "writer_ctx") fpga = context;
      if (context->identifier() == "tpu_core") tpu = context;
    }
    check(fpga && fpga->name() == "Fpga::DataflowStage<fpga::clk0>" &&
              fpga->genericArity() == 1, "lost FPGA clock-domain metadata");
    check(tpu && tpu->name() == "Tpu::TensorCore" && tpu->genericArity() == 0,
          "lost TPU context metadata");
    check(!fpga->isRuntimeMaterializable() && !tpu->isRuntimeMaterializable(),
          "accelerator evidence became a runtime value");
    std::set<std::string> fpgaGrants, tpuGrants;
    for (const auto &node : graph.nodes()) {
      if (node.kind != metreon::graphir::NodeKind::Grant || !node.context) continue;
      if (node.context->hasSameIdentity(*fpga)) fpgaGrants.insert(node.name);
      if (node.context->hasSameIdentity(*tpu)) tpuGrants.insert(node.name);
    }
    check(fpgaGrants == std::set<std::string>{"fifo_reserve<port::Output>",
              "stream_enqueue<port::Output>", "clock_wait<fpga::clk0>"},
          "FPGA grants lost port/clock arguments");
    check(tpuGrants == std::set<std::string>{"dma_copy<tpu::hbm, tpu::vmem>",
              "await_completion<tpu::LocalDma>", "tensor_compute<bf16>",
              "semaphore_signal<tpu::core>"}, "TPU grants lost scoped arguments");

    rejects(source + R"(
procedure bad(@cx: tpu_core) -> void !{dma_copy<tpu::vmem, tpu::hbm>} { return; }
)", "sema.missing_capability");
    rejects(source + R"(
procedure bad(@cx: writer_ctx) -> void !{fifo_reserve<port::Input>} { return; }
)", "sema.missing_capability");
    rejects(source + R"(
procedure bad(@cx: writer_ctx) -> void !{tensor_compute<bf16>} { return; }
)", "sema.missing_capability");
    rejects(source + R"(
context Fpga::DataflowStage<fpga::clk0> grants {
  fifo_reserve<port::Output>, stream_enqueue<port::Output>, clock_wait<fpga::clk0>
} other_writer;
procedure helper(@cx: writer_ctx) -> void { return; }
procedure bad(@cx: other_writer) -> void { helper(@cx); return; }
)", "sema.execution_context_mismatch");
    std::cout << "Accelerator context, grant and identity checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
