#include "metreon/GraphIR/Lowering.h"
#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/MLIR/Dialect.h"
#include "metreon/Parser/Parser.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
std::string print(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream output(text);
  module.print(output);
  return text;
}
metreon::graphir::Module graph(const std::string &source) {
  metreon::parser::Parser parser(source);
  return metreon::graphir::lowerModule(parser.parseModule(), "amd-test.mtr", true);
}
mlir::OwningOpRef<mlir::ModuleOp> lower(const metreon::graphir::Module &source, mlir::MLIRContext &context) {
  const auto before = source.print();
  auto semantic = metreon::importDeviceExecution(source, context);
  check(mlir::succeeded(semantic), "Metreon import failed");
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(print(**semantic), &context);
  check(static_cast<bool>(parsed), "Metreon round-trip failed");
  check(mlir::succeeded(metreon::lowerMetreonDeviceToGPU(*parsed)), "context-checked primary conversion failed");
  auto lowered = metreon::lowerDeviceToROCDL(*parsed);
  check(mlir::succeeded(lowered), "ROCDL conversion failed");
  check(source.print() == before, "AMD lowering mutated GraphIR");
  (**lowered).walk([](mlir::Operation *op) {
    const auto name = op->getName().getDialectNamespace();
    check(name == "builtin" || name == "llvm" || name == "rocdl", "unconverted operation in ROCDL module");
  });
  auto result = mlir::parseSourceString<mlir::ModuleOp>(print(**lowered), &context);
  check(static_cast<bool>(result), "ROCDL round-trip failed");
  return result;
}
void emit(mlir::ModuleOp module, const std::string &chip) {
  auto assembly = metreon::emitDeviceAMDGPU(module, chip);
  check(mlir::succeeded(assembly), "AMD assembly emission failed");
  check(assembly->find("amdgcn-amd-amdhsa--" + chip) != std::string::npos &&
      assembly->find(".amdhsa_code_object_version 5") != std::string::npos &&
      assembly->find(".amdhsa_kernel vector_add") != std::string::npos &&
      assembly->find("v_add_f32") != std::string::npos, "missing AMD target, kernel or source addition");
  auto object = metreon::emitDeviceAMDGPU(module, chip, true);
  check(mlir::succeeded(object) && object->size() > 64, "AMD object emission failed");
  check(object->substr(0, 4) == std::string("\x7f" "ELF", 4) &&
      llvm::support::endian::read16le(object->data() + 18) == llvm::ELF::EM_AMDGPU &&
      static_cast<unsigned char>((*object)[7]) == llvm::ELF::ELFOSABI_AMDGPU_HSA &&
      static_cast<unsigned char>((*object)[8]) == llvm::ELF::ELFABIVERSION_AMDGPU_HSA_V5,
      "wrong ELF machine or HSA ABI");
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2 || argc == 3, "expected vector-add source and optional codegen mode");
    std::ifstream file(argv[1]);
    check(static_cast<bool>(file), "cannot read source");
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto original = graph(source);
    mlir::MLIRContext context;
    auto rocdl = lower(original, context);
    const auto text = print(*rocdl);
    check(text.find("rocdl.workitem.id.x") != std::string::npos && text.find("rocdl.workgroup.dim.x") != std::string::npos &&
        text.find("llvm.fadd") != std::string::npos && text.find("llvm.zext") != std::string::npos,
        "missing device operations or unsigned hardware-index widening");
    if (argc == 3) { emit(*rocdl, "gfx1030"); emit(*rocdl, "gfx1100"); }
    {
      std::string diagnostic;
      mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
        llvm::raw_string_ostream out(diagnostic); value.print(out); return mlir::success();
      });
      check(mlir::failed(metreon::emitDeviceAMDGPU(*rocdl, "gfx9999")), "invalid architecture accepted");
      check(diagnostic.find("unsupported AMDGPU") != std::string::npos, "wrong architecture diagnostic");
      auto bad = metreon::importDeviceExecution(original, context);
      check(mlir::succeeded(bad), "test Metreon import failed");
      (**bad).walk([&](metreon::DeviceIntrinsicOp op) { op.setContextAttr(mlir::FlatSymbolRefAttr::get(&context, "host")); });
      check(mlir::failed(metreon::lowerMetreonDeviceToGPU(**bad)), "AMD path erased mismatched nominal contexts");
    }
    // Mutable GPU slots must lower using the AMD private address space; a
    // helper call and backedge exercise more than a straight-line kernel.
    std::string loops = "context Gpu::Loop repeat;\nprocedure sum(@cx: gpu_thread; a: f32, b: f32) -> f32 { return a + b; }\n" + source;
    const std::string store = "gpu::store(@cx; out, i, left + right);";
    const auto at = loops.find(store);
    check(at != std::string::npos, "missing store in fixture");
    loops.replace(at, store.size(), "f32 total = 0.0; for<@cx: repeat> (i32 pass) in 0..2 { if (pass == 0) { continue; } total = sum(@cx; left, right); } gpu::store(@cx; out, i, total);");
    auto loopModule = lower(graph(loops), context);
    check(print(*loopModule).find("llvm.alloca") != std::string::npos && print(*loopModule).find("ptr<5>") != std::string::npos,
        "AMD private local slots are missing");
    if (argc == 3) emit(*loopModule, "gfx1030");
    std::cout << "ROCDL lowering checks passed; no GPU program executed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
