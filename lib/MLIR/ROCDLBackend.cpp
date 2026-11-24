#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/CodeGen/Toolchain.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/GPUToROCDL/GPUToROCDLPass.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/ROCDLDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/ROCDL/ROCDLToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

using namespace mlir;
namespace metreon {
FailureOr<OwningOpRef<ModuleOp>> lowerDeviceToROCDL(ModuleOp module) {
  auto *context = module.getContext();
  context->getOrLoadDialect<LLVM::LLVMDialect>();
  context->getOrLoadDialect<ROCDL::ROCDLDialect>();
  LowerToLLVMOptions options(context);
  // AMD allocas live in private address space 5. Obtain the target layout
  // before converting memref slots rather than inheriting a host layout.
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetMC();
  std::string error;
  auto *amd = llvm::TargetRegistry::lookupTarget("amdgcn-amd-amdhsa", error);
  if (!amd) { module.emitError(error); return failure(); }
  auto layout = std::unique_ptr<llvm::TargetMachine>(amd->createTargetMachine(
      "amdgcn-amd-amdhsa", "gfx900", "", llvm::TargetOptions{}, llvm::Reloc::PIC_));
  if (!layout) { module.emitError("AMDGPU target layout unavailable"); return failure(); }
  options.dataLayout = layout->createDataLayout();
  // GraphIR's scalar slots are invocation-local. MemRefToLLVM uses the memref
  // address space for allocas, so mark those slots private explicitly; the
  // target data layout alone does not change a memref's default memory space.
  module.walk([&](memref::AllocaOp local) {
    auto type = local.getType();
    local.getResult().setType(MemRefType::get(type.getShape(), type.getElementType(), type.getLayout(),
        IntegerAttr::get(IntegerType::get(context, 64), 5)));
  });
  options.overrideIndexBitwidth(64);
  LLVMTypeConverter converter(context, options);
  RewritePatternSet patterns(context);
  populateGpuToROCDLConversionPatterns(converter, patterns, gpu::amd::Runtime::HIP);
  populateFuncToLLVMConversionPatterns(converter, patterns);
  arith::populateArithToLLVMConversionPatterns(converter, patterns);
  cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
  populateFinalizeMemRefToLLVMConversionPatterns(converter, patterns);
  ConversionTarget target(*context);
  configureGpuToROCDLConversionLegality(target);
  target.addLegalDialect<LLVM::LLVMDialect, ROCDL::ROCDLDialect>();
  target.addLegalOp<ModuleOp, gpu::GPUModuleOp, gpu::ModuleEndOp>();
  if (failed(applyFullConversion(module, target, std::move(patterns))) || failed(verify(module))) return failure();
  // Hardware IDs and dimensions are unsigned 32-bit values. Preserve their
  // full range when realizing Metreon's 64-bit index arithmetic.
  SmallVector<LLVM::SExtOp> indexExtensions;
  module.walk([&](LLVM::SExtOp op) {
    if (isa_and_nonnull<ROCDL::BlockIdXOp, ROCDL::BlockIdYOp, ROCDL::BlockIdZOp,
        ROCDL::ThreadIdXOp, ROCDL::ThreadIdYOp, ROCDL::ThreadIdZOp,
        ROCDL::BlockDimXOp, ROCDL::BlockDimYOp, ROCDL::BlockDimZOp,
        ROCDL::GridDimXOp, ROCDL::GridDimYOp, ROCDL::GridDimZOp>(op.getArg().getDefiningOp())) indexExtensions.push_back(op);
  });
  for (auto op : indexExtensions) {
    OpBuilder builder(op);
    auto replacement = builder.create<LLVM::ZExtOp>(op.getLoc(), op.getType(), op.getArg());
    op.replaceAllUsesWith(replacement.getResult());
    op.erase();
  }
  auto output = OwningOpRef<ModuleOp>(ModuleOp::create(module.getLoc()));
  unsigned modules = 0;
  for (auto device : module.getOps<gpu::GPUModuleOp>()) {
    ++modules;
    for (auto &op : device.getBody()->without_terminator()) output->getBody()->push_back(op.clone());
  }
  if (modules != 1) { module.emitError("ROCDL emission requires exactly one device module"); return failure(); }
  output->walk([&](LLVM::LLVMFuncOp function) {
    function->removeAttr("gpu.kernel");
    if (function->hasAttr("rocdl.kernel"))
      function->setAttr("rocdl.flat_work_group_size", StringAttr::get(context, "1,1024"));
  });
  (*output)->setAttr("llvm.target_triple", StringAttr::get(context, "amdgcn-amd-amdhsa"));
  if (failed(verify(*output))) return failure();
  return output;
}

FailureOr<std::string> emitDeviceAMDGPU(ModuleOp rocdl, llvm::StringRef chip, bool object) {
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUAsmPrinter();
  std::string error;
  const std::string triple = "amdgcn-amd-amdhsa";
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) { rocdl.emitError(error); return failure(); }
  auto supported = std::unique_ptr<llvm::MCSubtargetInfo>(target->createMCSubtargetInfo(triple, "gfx900", ""));
  if (!supported || !chip.starts_with("gfx") || !supported->isCPUStringValid(chip)) {
    rocdl.emitError() << "unsupported AMDGPU architecture: " << chip;
    return failure();
  }
  auto machine = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, chip, "", llvm::TargetOptions{}, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::None));
  if (!machine) { rocdl.emitError("cannot create AMDGPU target machine"); return failure(); }
  registerBuiltinDialectTranslation(*rocdl.getContext());
  registerLLVMDialectTranslation(*rocdl.getContext());
  registerROCDLDialectTranslation(*rocdl.getContext());
  llvm::LLVMContext context;
  auto ir = translateModuleToLLVMIR(rocdl, context);
  if (!ir) return failure();
  ir->setTargetTriple(triple);
  ir->setDataLayout(machine->createDataLayout());
  ir->addModuleFlag(llvm::Module::Override, "amdgpu_code_object_version", 500);
  // Standard ROCDL workgroup-size operations translate to OCKL calls. Link the
  // actual device library and its ABI constant; do not leave unresolved device
  // calls or synthesize a different indexing contract in the source frontend.
  for (const char *path : {toolchain::ockl, toolchain::abi500}) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) { rocdl.emitError() << "AMDGPU emission requires ROCm device bitcode: " << path; return failure(); }
    auto library = llvm::parseBitcodeFile((*buffer)->getMemBufferRef(), context);
    if (!library) { rocdl.emitError() << llvm::toString(library.takeError()); return failure(); }
    (*library)->setDataLayout(ir->getDataLayout());
    if (llvm::Linker::linkModules(*ir, std::move(*library), llvm::Linker::Flags::LinkOnlyNeeded)) {
      rocdl.emitError("failed to link ROCm device bitcode"); return failure();
    }
  }
  if (llvm::verifyModule(*ir, &llvm::errs())) { rocdl.emitError("invalid translated AMDGPU LLVM IR"); return failure(); }
  llvm::SmallVector<char> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::legacy::PassManager emission;
  if (machine->addPassesToEmitFile(emission, stream, nullptr,
          object ? llvm::CodeGenFileType::ObjectFile : llvm::CodeGenFileType::AssemblyFile)) {
    rocdl.emitError("AMDGPU code emission unavailable"); return failure();
  }
  emission.run(*ir);
  return std::string(bytes.begin(), bytes.end());
}
} // namespace metreon
