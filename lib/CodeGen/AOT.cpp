#include "metreon/CodeGen/AOT.h"
#include "metreon/CodeGen/Toolchain.h"
#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/MLIR/HostExecution.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace metreon {
namespace {
namespace fs = std::filesystem;
void write(const fs::path &path, llvm::StringRef text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  if (!file) throw std::runtime_error("cannot write compiler output " + path.string());
}
std::string read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read compiler output " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct WorkDirectory {
  fs::path path;
  explicit WorkDirectory(const fs::path &parent) {
    llvm::SmallString<256> created;
    if (auto error = llvm::sys::fs::createUniqueDirectory((parent / ".metreon-link").string(), created))
      throw std::runtime_error("cannot create linker workspace: " + error.message());
    path = created.str().str();
  }
  ~WorkDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void execute(const std::string &tool, std::vector<std::string> arguments, const fs::path &work) {
  if (tool.empty() || tool.ends_with("-NOTFOUND")) throw std::runtime_error("required AoT tool is unavailable");
  auto program = llvm::sys::findProgramByName(tool);
  if (!program) throw std::runtime_error("cannot find AoT tool " + tool);
  arguments.insert(arguments.begin(), *program);
  std::vector<llvm::StringRef> args(arguments.begin(), arguments.end());
  const auto errorPath = (work / "tool.stderr").string();
  const std::optional<llvm::StringRef> redirects[] = {std::nullopt, std::nullopt, errorPath};
  std::string error;
  const auto status = llvm::sys::ExecuteAndWait(*program, args, std::nullopt, redirects, 0, 0, &error);
  const auto diagnostics = read(errorPath);
  if (status != 0) throw std::runtime_error("AoT tool " + tool + " failed (" + std::to_string(status) + "): " + error + diagnostics);
  if (!diagnostics.empty()) llvm::errs() << diagnostics;
}

// Define the platform ABI of source host intrinsics in the emitted LLVM module.
// These functions become native instructions in the output object. The compiler
// itself neither links CUDA/HIP nor executes any of these calls.
class HostABIEmitter {
  llvm::Module &module;
  llvm::LLVMContext &context;
  llvm::IRBuilder<> builder;
  llvm::Type *i32, *i64, *pointer, *voidType;
  llvm::Function *checkFunction = nullptr;
  llvm::Function *initialize = nullptr;
  llvm::GlobalVariable *deviceModule = nullptr;
  llvm::ArrayRef<uint8_t> image;
  GPUBackend backend;

  llvm::StringRef driverName(llvm::StringRef name) {
    if (backend != GPUBackend::AMDGPU) return name;
    return llvm::StringSwitch<llvm::StringRef>(name)
        .Case("cuInit", "hipInit").Case("cuDeviceGet", "hipDeviceGet")
        .Case("cuModuleLoadData", "hipModuleLoadData").Case("cuModuleUnload", "hipModuleUnload")
        .Case("cuModuleGetFunction", "hipModuleGetFunction").Case("cuLaunchKernel", "hipModuleLaunchKernel")
        .Case("cuMemAlloc_v2", "hipMalloc").Case("cuMemFree_v2", "hipFree")
        .Case("cuMemcpyHtoD_v2", "hipMemcpyHtoD").Case("cuMemcpyDtoH_v2", "hipMemcpyDtoH")
        .Case("cuCtxSynchronize", "hipDeviceSynchronize").Default(name);
  }

  llvm::ConstantInt *integer(uint64_t value, unsigned bits = 64) {
    return llvm::ConstantInt::get(llvm::IntegerType::get(context, bits), value);
  }
  llvm::Value *null() { return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(pointer)); }
  llvm::FunctionCallee external(llvm::StringRef name, llvm::Type *result, llvm::ArrayRef<llvm::Type *> arguments, bool variadic = false) {
    name = driverName(name);
    auto *type = llvm::FunctionType::get(result, arguments, variadic);
    if (auto *existing = module.getFunction(name); existing &&
        (!existing->isDeclaration() || existing->getFunctionType() != type))
      throw std::runtime_error("source symbol conflicts with platform ABI: " + name.str());
    return module.getOrInsertFunction(name, type);
  }
  void check(llvm::Value *status, llvm::StringRef action) {
    builder.CreateCall(checkFunction, {status, builder.CreateGlobalStringPtr(action)});
  }
  void require(llvm::Value *condition, llvm::StringRef action) {
    check(builder.CreateSelect(condition, integer(0, 32), integer(1, 32)), action);
  }
  llvm::Value *call(llvm::StringRef name, llvm::ArrayRef<llvm::Type *> types, llvm::ArrayRef<llvm::Value *> args) {
    llvm::SmallVector<llvm::Type *> signature(types);
    llvm::SmallVector<llvm::Value *> operands(args);
    if (backend == GPUBackend::AMDGPU) {
      // HIP's device addresses are void*, whereas the CUDA driver API uses
      // CUdeviceptr (uint64_t). Keep the emitted declarations ABI-correct.
      const int address = name == "cuMemFree_v2" || name == "cuMemcpyHtoD_v2" ? 0 : name == "cuMemcpyDtoH_v2" ? 1 : -1;
      if (address >= 0) { signature[address] = pointer; operands[address] = builder.CreateIntToPtr(operands[address], pointer); }
    }
    auto *status = builder.CreateCall(external(name, i32, signature), operands);
    check(status, driverName(name));
    return status;
  }
  void emitCheck() {
    checkFunction = llvm::Function::Create(llvm::FunctionType::get(voidType, {i32, pointer}, false),
        llvm::GlobalValue::InternalLinkage, "__metreon_gpu_check", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", checkFunction);
    auto *failed = llvm::BasicBlock::Create(context, "failed", checkFunction);
    auto *done = llvm::BasicBlock::Create(context, "done", checkFunction);
    builder.SetInsertPoint(entry);
    builder.CreateCondBr(builder.CreateICmpEQ(checkFunction->getArg(0), integer(0, 32)), done, failed);
    builder.SetInsertPoint(failed);
    builder.CreateCall(external("printf", i32, {pointer}, true),
        {builder.CreateGlobalStringPtr("GPU operation %s failed (%d)\n"), checkFunction->getArg(1), checkFunction->getArg(0)});
    builder.CreateCall(external("exit", voidType, {i32}), {integer(1, 32)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
  }
  void emitInitialization() {
    emitCheck();
    auto *initialized = new llvm::GlobalVariable(module, builder.getInt1Ty(), false,
        llvm::GlobalValue::InternalLinkage, builder.getFalse(), "__metreon_gpu_initialized");
    deviceModule = new llvm::GlobalVariable(module, pointer, false,
        llvm::GlobalValue::InternalLinkage, llvm::cast<llvm::Constant>(null()), "__metreon_gpu_module");
    auto *device = new llvm::GlobalVariable(module, i32, false,
        llvm::GlobalValue::InternalLinkage, integer(0, 32), "__metreon_gpu_device");
    initialize = llvm::Function::Create(llvm::FunctionType::get(voidType, false),
        llvm::GlobalValue::InternalLinkage, "__metreon_gpu_initialize", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", initialize);
    auto *setup = llvm::BasicBlock::Create(context, "setup", initialize);
    auto *done = llvm::BasicBlock::Create(context, "done", initialize);
    builder.SetInsertPoint(entry);
    builder.CreateCondBr(builder.CreateLoad(builder.getInt1Ty(), initialized), done, setup);
    builder.SetInsertPoint(setup);
    call("cuInit", {i32}, {integer(0, 32)});
    if (backend == GPUBackend::AMDGPU) {
      call("hipSetDevice", {i32}, {integer(0, 32)});
    } else {
      call("cuDeviceGet", {pointer, i32}, {device, integer(0, 32)});
      auto *handle = builder.CreateAlloca(pointer);
      call("cuDevicePrimaryCtxRetain", {pointer, i32}, {handle, builder.CreateLoad(i32, device)});
      call("cuCtxSetCurrent", {pointer}, {builder.CreateLoad(pointer, handle)});
    }
    if (!image.empty()) {
      auto *data = llvm::ConstantDataArray::get(context, image);
      auto *binary = new llvm::GlobalVariable(module, data->getType(), true,
          llvm::GlobalValue::PrivateLinkage, data, "__metreon_device_image");
      binary->setAlignment(llvm::Align(16));
      binary->setSection(backend == GPUBackend::AMDGPU ? ".metreon.amdgpu" : ".metreon.cuda");
      call("cuModuleLoadData", {pointer, pointer}, {deviceModule, binary});
    }
    auto *cleanup = llvm::Function::Create(llvm::FunctionType::get(voidType, false),
        llvm::GlobalValue::InternalLinkage, "__metreon_gpu_cleanup", module);
    {
      llvm::IRBuilderBase::InsertPointGuard guard(builder);
      builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", cleanup));
      if (!image.empty()) builder.CreateCall(external("cuModuleUnload", i32, {pointer}), {builder.CreateLoad(pointer, deviceModule)});
      if (backend == GPUBackend::CUDA)
        builder.CreateCall(external("cuDevicePrimaryCtxRelease_v2", i32, {i32}), {builder.CreateLoad(i32, device)});
      builder.CreateRetVoid();
    }
    check(builder.CreateCall(external("atexit", i32, {pointer}), {cleanup}), "register GPU cleanup");
    builder.CreateStore(builder.getTrue(), initialized);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
  }
  llvm::Value *deviceAddress(llvm::Function &function) {
    return builder.CreateAdd(builder.CreatePtrToInt(function.getArg(1), i64),
        builder.CreateMul(function.getArg(2), integer(4)));
  }

public:
  HostABIEmitter(llvm::Module &module, llvm::ArrayRef<uint8_t> image, GPUBackend backend)
      : module(module), context(module.getContext()), builder(context), i32(builder.getInt32Ty()),
        i64(builder.getInt64Ty()), pointer(builder.getPtrTy()), voidType(builder.getVoidTy()), image(image), backend(backend) {}
  void emit(llvm::ArrayRef<HostABI> functions) {
    if (llvm::any_of(functions, [](const auto &abi) { return abi.kind.starts_with("gpu::"); })) emitInitialization();
    for (const auto &abi : functions) {
      auto *function = module.getFunction(abi.symbol);
      if (!function || !function->isDeclaration()) throw std::runtime_error("missing generated host ABI declaration");
      function->setLinkage(llvm::GlobalValue::InternalLinkage);
      builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
      if (abi.kind == "host::print_f32") {
        builder.CreateCall(external("printf", i32, {pointer}, true),
            {builder.CreateGlobalStringPtr("%g\n"), builder.CreateFPExt(function->getArg(0), builder.getDoubleTy())});
      } else {
        builder.CreateCall(initialize);
        if (abi.kind == "gpu::alloc") {
          auto *count = function->getArg(0);
          require(builder.CreateAnd(builder.CreateICmpSGE(count, integer(0)),
              builder.CreateICmpULE(count, integer(INT64_MAX / 4))), "invalid GPU allocation length");
          auto *allocation = builder.CreateAlloca(i64);
          auto *bytes = builder.CreateSelect(builder.CreateICmpEQ(count, integer(0)), integer(4), builder.CreateMul(count, integer(4)));
          call("cuMemAlloc_v2", {pointer, i64}, {allocation, bytes});
          auto *type = llvm::cast<llvm::StructType>(function->getReturnType());
          auto *address = builder.CreateIntToPtr(builder.CreateLoad(i64, allocation), type->getElementType(0));
          llvm::Value *result = llvm::UndefValue::get(type);
          result = builder.CreateInsertValue(result, address, {0});
          result = builder.CreateInsertValue(result, address, {1});
          result = builder.CreateInsertValue(result, integer(0), {2});
          result = builder.CreateInsertValue(result, count, {3, 0});
          result = builder.CreateInsertValue(result, integer(1), {4, 0});
          builder.CreateRet(result);
          continue;
        }
        if (abi.kind == "gpu::free") {
          call("cuMemFree_v2", {i64}, {builder.CreatePtrToInt(function->getArg(0), i64)});
        } else if (abi.kind == "gpu::upload" || abi.kind == "gpu::download") {
          require(builder.CreateICmpEQ(function->getArg(3), function->getArg(8)), "GPU copy span length mismatch");
          auto *host = builder.CreateGEP(builder.getFloatTy(), function->getArg(6), function->getArg(7));
          auto *bytes = builder.CreateMul(function->getArg(8), integer(4));
          if (abi.kind == "gpu::upload") call("cuMemcpyHtoD_v2", {i64, pointer, i64}, {deviceAddress(*function), host, bytes});
          else call("cuMemcpyDtoH_v2", {pointer, i64, i64}, {host, deviceAddress(*function), bytes});
        } else if (abi.kind == "gpu::wait") {
          call("cuCtxSynchronize", {}, {});
        } else if (abi.kind == "gpu::launch") {
          if (image.empty()) throw std::runtime_error("kernel launch has no compiled device image");
          auto *handle = builder.CreateAlloca(pointer);
          call("cuModuleGetFunction", {pointer, pointer, pointer},
              {handle, builder.CreateLoad(pointer, deviceModule), builder.CreateGlobalStringPtr(abi.kernel)});
          for (unsigned axis = 0; axis < 2; ++axis)
            require(builder.CreateAnd(builder.CreateICmpUGT(function->getArg(axis), integer(0)),
                builder.CreateICmpULE(function->getArg(axis), integer(UINT32_MAX))), "invalid GPU launch dimensions");
          if (backend == GPUBackend::AMDGPU)
            require(builder.CreateICmpULT(builder.CreateMul(function->getArg(0), function->getArg(1)), integer(uint64_t{1} << 32)),
                "HIP grid dimension exceeds the supported work-item range");
          const auto count = function->arg_size() - 2;
          auto *parameters = builder.CreateAlloca(pointer, integer(count ? count : 1));
          for (size_t index = 0; index < count; ++index) {
            auto *argument = function->getArg(index + 2);
            auto *slot = builder.CreateAlloca(argument->getType());
            builder.CreateStore(argument, slot);
            builder.CreateStore(slot, builder.CreateGEP(pointer, parameters, integer(index)));
          }
          call("cuLaunchKernel", {pointer, i32, i32, i32, i32, i32, i32, i32, pointer, pointer, pointer},
              {builder.CreateLoad(pointer, handle), builder.CreateTrunc(function->getArg(0), i32), integer(1, 32), integer(1, 32),
               builder.CreateTrunc(function->getArg(1), i32), integer(1, 32), integer(1, 32), integer(0, 32), null(), parameters, null()});
        } else throw std::runtime_error("unsupported host platform ABI: " + abi.kind);
      }
      builder.CreateRetVoid();
    }
  }
};
} // namespace

std::string linkAMDGPUCodeObject(const std::string &object) {
  WorkDirectory work(fs::temp_directory_path());
  write(work.path / "device.o", object);
  execute(toolchain::amdLinker, {"-shared", "--no-undefined", "-o", (work.path / "device.hsaco").string(),
      (work.path / "device.o").string()}, work.path);
  return read(work.path / "device.hsaco");
}

void emitExecutable(const graphir::Module &source, const AOTOptions &options) {
  const fs::path destination = fs::absolute(options.output);
  if (!fs::is_directory(destination.parent_path())) throw std::runtime_error("executable output directory does not exist");
  if (fs::exists(destination) && !fs::is_regular_file(destination)) throw std::runtime_error("executable output is not a regular file");
  if (fs::is_regular_file(source.sourceName()) && fs::exists(destination) && fs::equivalent(destination, source.sourceName()))
    throw std::runtime_error("executable output must not overwrite the source file");
  mlir::MLIRContext context;
  auto host = importHostExecution(source, context);
  if (mlir::failed(host)) throw std::runtime_error("host executable lowering failed");
  auto main = mlir::dyn_cast_or_null<mlir::func::FuncOp>(mlir::SymbolTable::lookupSymbolIn(**host, "main"));
  if (!main || main.getNumArguments() || main.getFunctionType().getResults().size() != 1 || !main.getResultTypes()[0].isInteger(32))
    throw std::runtime_error("executable requires a host procedure main() -> i32 with no runtime parameters");
  WorkDirectory work(destination.parent_path());
  std::string deviceImage;
  if (!source.kernelGraphs().empty()) {
    auto device = importDeviceExecution(source, context);
    if (mlir::failed(device) || mlir::failed(lowerMetreonDeviceToGPU(**device))) throw std::runtime_error("device executable lowering failed");
    if (options.backend == GPUBackend::AMDGPU) {
      auto rocdl = lowerDeviceToROCDL(**device);
      if (mlir::failed(rocdl)) throw std::runtime_error("ROCDL lowering failed");
      auto object = emitDeviceAMDGPU(**rocdl, options.architecture, true);
      if (mlir::failed(object)) throw std::runtime_error("AMDGPU object emission failed");
      deviceImage = linkAMDGPUCodeObject(*object);
    } else {
      auto nvvm = lowerDeviceToNVVM(**device);
      if (mlir::failed(nvvm)) throw std::runtime_error("NVVM lowering failed");
      auto ptx = emitDevicePTX(**nvvm, options.architecture);
      if (mlir::failed(ptx)) throw std::runtime_error("PTX emission failed");
      write(work.path / "device.ptx", *ptx);
      execute(toolchain::ptxas, {"-arch=" + options.architecture, "-O0", "-o", (work.path / "device.cubin").string(), (work.path / "device.ptx").string()}, work.path);
      deviceImage = read(work.path / "device.cubin");
    }
  }
  auto abi = lowerHostExecution(**host);
  if (mlir::failed(abi)) throw std::runtime_error("host LLVM lowering failed");
  const bool needsGPU = llvm::any_of(*abi, [](const auto &item) { return item.kind.starts_with("gpu::"); });
  const char *gpuLibrary = options.backend == GPUBackend::AMDGPU ? toolchain::hipLibrary : toolchain::cudaDriver;
  if (needsGPU && (std::string(gpuLibrary).empty() || !fs::is_regular_file(gpuLibrary)))
    throw std::runtime_error("linking a GPU executable requires the selected CUDA/HIP library at build configuration time");
  mlir::registerBuiltinDialectTranslation(context);
  mlir::registerLLVMDialectTranslation(context);
  llvm::LLVMContext nativeContext;
  auto module = mlir::translateModuleToLLVMIR(**host, nativeContext, source.sourceName());
  if (!module) throw std::runtime_error("host LLVM translation failed");
  HostABIEmitter(*module, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(deviceImage.data()), deviceImage.size()), options.backend).emit(*abi);
  if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter()) throw std::runtime_error("native LLVM target unavailable");
  std::string error;
  const auto triple = llvm::sys::getDefaultTargetTriple();
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) throw std::runtime_error(error);
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(triple, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_, std::nullopt, llvm::CodeGenOptLevel::None));
  if (!machine) throw std::runtime_error("cannot create native LLVM target machine");
  module->setTargetTriple(triple);
  module->setDataLayout(machine->createDataLayout());
  if (module->getDataLayout().getPointerSizeInBits() != 64) throw std::runtime_error("executable profile currently requires a 64-bit host");
  if (llvm::verifyModule(*module, &llvm::errs())) throw std::runtime_error("invalid generated host LLVM IR");
  const auto object = (work.path / "host.o").string();
  std::error_code outputError;
  llvm::raw_fd_ostream output(object, outputError);
  if (outputError) throw std::runtime_error(outputError.message());
  llvm::legacy::PassManager emission;
  if (machine->addPassesToEmitFile(emission, output, nullptr, llvm::CodeGenFileType::ObjectFile)) throw std::runtime_error("native object emission unavailable");
  emission.run(*module);
  output.close();
  if (output.has_error()) throw std::runtime_error("native object write failed");
  const auto executable = work.path / "program";
  std::vector<std::string> link{object, "-o", executable.string()};
  if (needsGPU) {
    link.push_back(gpuLibrary);
    if (options.backend == GPUBackend::AMDGPU) link.push_back("-Wl,-rpath," + fs::path(gpuLibrary).parent_path().string());
  }
  execute(toolchain::hostLinker, std::move(link), work.path);
  // The last successful executable survives any frontend, assembler or linker
  // failure. The temporary and destination share a filesystem for atomic rename.
  fs::rename(executable, destination);
}
} // namespace metreon
