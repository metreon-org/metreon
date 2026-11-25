#include "metreon/MLIR/DeviceExecution.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/LLVMCommon/LoweringOptions.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

using namespace mlir;
namespace metreon {
FailureOr<OwningOpRef<ModuleOp>> lowerDeviceToNVVM(ModuleOp module) {
  auto *context = module.getContext();
  context->getOrLoadDialect<LLVM::LLVMDialect>();
  context->getOrLoadDialect<NVVM::NVVMDialect>();
  LowerToLLVMOptions options(context);
  options.overrideIndexBitwidth(64);
  LLVMTypeConverter converter(context, options);
  RewritePatternSet patterns(context);
  populateGpuToNVVMConversionPatterns(converter, patterns);
  populateFuncToLLVMConversionPatterns(converter, patterns);
  arith::populateArithToLLVMConversionPatterns(converter, patterns);
  cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
  populateFinalizeMemRefToLLVMConversionPatterns(converter, patterns);
  ConversionTarget target(*context);
  configureGpuToNVVMConversionLegality(target);
  target.addLegalDialect<LLVM::LLVMDialect, NVVM::NVVMDialect>();
  target.addLegalOp<ModuleOp, gpu::GPUModuleOp, gpu::ModuleEndOp>();
  if (failed(applyFullConversion(module, target, std::move(patterns))) || failed(verify(module))) return failure();
  auto output = OwningOpRef<ModuleOp>(ModuleOp::create(module.getLoc()));
  unsigned modules = 0;
  for (auto device : module.getOps<gpu::GPUModuleOp>()) {
    ++modules;
    for (auto &op : device.getBody()->without_terminator()) output->getBody()->push_back(op.clone());
  }
  if (modules != 1) { module.emitError("NVVM execution requires exactly one device module"); return failure(); }
  output->walk([](LLVM::LLVMFuncOp function) { function->removeAttr("gpu.kernel"); });
  OpBuilder builder(context);
  (*output)->setAttr("llvm.target_triple", builder.getStringAttr("nvptx64-nvidia-cuda"));
  if (failed(verify(*output))) return failure();
  return output;
}

FailureOr<std::string> emitDevicePTX(ModuleOp nvvm, llvm::StringRef chip) {
  if (chip.size() < 5 || !chip.starts_with("sm_") ||
      !llvm::all_of(chip.drop_front(3), [](char c) { return c >= '0' && c <= '9'; })) {
    nvvm.emitError("CUDA architecture must be sm_<compute capability>, for example sm_89");
    return failure();
  }
  LLVMInitializeNVPTXTargetInfo();
  LLVMInitializeNVPTXTarget();
  LLVMInitializeNVPTXTargetMC();
  LLVMInitializeNVPTXAsmPrinter();
  std::string error;
  const std::string triple = "nvptx64-nvidia-cuda";
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) { nvvm.emitError(error); return failure(); }
  auto supported = std::unique_ptr<llvm::MCSubtargetInfo>(target->createMCSubtargetInfo(triple, "", ""));
  if (!supported || !supported->isCPUStringValid(chip)) {
    nvvm.emitError() << "CUDA architecture is unsupported by this LLVM build: " << chip;
    return failure();
  }
  llvm::TargetOptions options;
  auto machine = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, chip, "+ptx80", options, std::nullopt, std::nullopt, llvm::CodeGenOptLevel::None));
  if (!machine) { nvvm.emitError("cannot create the NVPTX target machine"); return failure(); }
  registerBuiltinDialectTranslation(*nvvm.getContext());
  registerLLVMDialectTranslation(*nvvm.getContext());
  registerNVVMDialectTranslation(*nvvm.getContext());
  llvm::LLVMContext context;
  auto ir = translateModuleToLLVMIR(nvvm, context);
  if (!ir) return failure();
  ir->setTargetTriple(triple);
  ir->setDataLayout(machine->createDataLayout());
  if (llvm::verifyModule(*ir, &llvm::errs())) { nvvm.emitError("invalid translated LLVM IR"); return failure(); }
  llvm::SmallVector<char> bytes;
  llvm::raw_svector_ostream assembly(bytes);
  llvm::legacy::PassManager emission;
  if (machine->addPassesToEmitFile(emission, assembly, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
    nvvm.emitError("NVPTX assembly emission is unavailable"); return failure();
  }
  emission.run(*ir);
  return std::string(bytes.begin(), bytes.end());
}
} // namespace metreon
