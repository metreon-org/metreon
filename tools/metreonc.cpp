#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"
#ifdef METREON_ENABLE_MLIR
#include "metreon/MLIR/Import.h"
#include "metreon/MLIR/HostLowering.h"
#include "metreon/MLIR/GPULowering.h"
#include "metreon/MLIR/DeviceExecution.h"
#include "metreon/CodeGen/AOT.h"
#include "mlir/IR/MLIRContext.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Artifact { std::string format, extension, text; };

void printUsage(std::ostream &output) {
  output << "usage: metreonc <input.mtr> [-o <executable>] [--target cuda|amdgpu] [--arch <chip>]\n"
            "       metreonc --emit=graphir|mlir|llvm|gpu|nvvm|ptx|rocdl|amdgpu|hsaco [options] <input.mtr>\n"
            "Default output is a native executable named after the input stem.\n"
            "Compilation never executes the program or initializes a GPU.\n"
            "GraphIR emission runs its default optimization passes unless --no-optimization is set.\n"
            "Writes <input-stem>.graphir or .mlir (stdin for -) to\n"
            "  tmp/graphir/, tmp/mlir/, tmp/llvm/, or tmp/gpu/ relative to the working directory.\n"
            "MLIR requires METREON_ENABLE_MLIR=ON; --emit=mlir preserves deferred bodies.\n"
            "--emit=llvm converts checked host scalar GraphIR CFGs to LLVM-dialect MLIR.\n"
            "--emit=gpu lowers checked GPU scalar control flow to affine/scf MLIR.\n"
            "--context-only selects live device bodies and skips optional verification/optimization.\n"
            "--emit=nvvm and --emit=ptx imply --context-only and preserve intermediate artifacts.\n"
            "Executable emission uses the context-only profile and writes only the linked binary.\n"
            "GPU code defaults to sm_80 (CUDA) or gfx900 (AMDGPU); --arch selects the AoT chip.\n"
            "--emit=rocdl, --emit=amdgpu (assembly) and --emit=hsaco select the AMDGPU target.\n";
}

std::string readSource(const std::string &path) {
  if (path == "-") {
    return std::string(std::istreambuf_iterator<char>(std::cin),
                       std::istreambuf_iterator<char>());
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open input file `" + path + "`");
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

#ifdef METREON_ENABLE_MLIR
std::string printMLIR(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream output(text);
  module.print(output, mlir::OpPrintingFlags().enableDebugInfo());
  output << '\n';
  return text;
}

std::optional<std::vector<Artifact>> emitDevice(
    const metreon::graphir::Module &graph, mlir::MLIRContext &context,
    const std::string &format, const std::string &architecture, bool amd) {
  auto imported = metreon::importDeviceExecution(graph, context);
  if (mlir::failed(imported)) return std::nullopt;
  std::vector<Artifact> artifacts{{"graphir", ".graphir", graph.print()},
                                 {"mlir", ".mlir", printMLIR(**imported)}};
  if (format == "mlir") return artifacts;
  auto manifest = format == "ptx" ? metreon::deviceLaunchManifest(**imported) : llvm::json::Object{};
  if (mlir::failed(metreon::lowerMetreonDeviceToGPU(**imported))) return std::nullopt;
  artifacts.push_back({"gpu", ".mlir", printMLIR(**imported)});
  if (format == "gpu") return artifacts;
  if (amd) {
    auto rocdl = metreon::lowerDeviceToROCDL(**imported);
    if (mlir::failed(rocdl)) return std::nullopt;
    artifacts.push_back({"rocdl", ".mlir", printMLIR(**rocdl)});
    if (format == "rocdl") return artifacts;
    auto assembly = metreon::emitDeviceAMDGPU(**rocdl, architecture);
    if (mlir::failed(assembly)) return std::nullopt;
    artifacts.push_back({"amdgpu", ".s", std::move(*assembly)});
    if (format == "amdgpu") return artifacts;
    auto object = metreon::emitDeviceAMDGPU(**rocdl, architecture, true);
    if (mlir::failed(object)) return std::nullopt;
    artifacts.push_back({"hsaco", ".hsaco", metreon::linkAMDGPUCodeObject(*object)});
    return artifacts;
  }
  auto nvvm = metreon::lowerDeviceToNVVM(**imported);
  if (mlir::failed(nvvm)) return std::nullopt;
  artifacts.push_back({"nvvm", ".mlir", printMLIR(**nvvm)});
  if (format == "nvvm") return artifacts;
  auto ptx = metreon::emitDevicePTX(**nvvm, architecture.empty() ? "sm_80" : architecture);
  if (mlir::failed(ptx)) return std::nullopt;
  artifacts.push_back({"ptx", ".abi.json", llvm::formatv("{0:2}\n", llvm::json::Value(llvm::json::Object(manifest))).str()});
  artifacts.push_back({"ptx", ".ptx", std::move(*ptx)});
  return artifacts;
}
#endif

} // namespace

int main(int argc, char **argv) {
  std::string inputPath;
  bool optimize = true;
  std::string emitFormat = "exe";
  bool contextOnly = false;
  std::string outputPath, architecture;
  std::string gpuTarget = "cuda";
  bool explicitTarget = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout);
      return 0;
    }
    if (argument == "--emit=exe" || argument == "--emit=graphir" || argument == "--emit=mlir" || argument == "--emit=llvm" || argument == "--emit=gpu" || argument == "--emit=nvvm" || argument == "--emit=ptx" || argument == "--emit=rocdl" || argument == "--emit=amdgpu" || argument == "--emit=hsaco") {
      emitFormat = argument.substr(7);
      continue;
    }
    if (argument == "--emit") {
      if (index + 1 >= argc || (std::string(argv[index + 1]) != "exe" && std::string(argv[index + 1]) != "graphir" &&
                               std::string(argv[index + 1]) != "mlir" && std::string(argv[index + 1]) != "llvm" &&
                               std::string(argv[index + 1]) != "gpu" && std::string(argv[index + 1]) != "nvvm" &&
                               std::string(argv[index + 1]) != "ptx" && std::string(argv[index + 1]) != "rocdl" &&
                               std::string(argv[index + 1]) != "amdgpu" && std::string(argv[index + 1]) != "hsaco")) {
        std::cerr << "metreonc: --emit requires exe, graphir, mlir, llvm, gpu, nvvm, ptx, rocdl, amdgpu, or hsaco\n";
        printUsage(std::cerr);
        return 1;
      }
      emitFormat = argv[++index];
      continue;
    }
    if (argument == "--no-optimization") {
      optimize = false;
      continue;
    }
    if (argument == "--context-only") { contextOnly = true; continue; }
    if (argument.starts_with("--target=")) {
      gpuTarget = argument.substr(9); explicitTarget = true; continue;
    }
    if (argument == "-o" || argument == "--output" || argument == "--arch" || argument == "--target") {
      if (index + 1 == argc) {
        std::cerr << "metreonc: " << argument << " requires a value\n";
        return 1;
      }
      if (argument == "--target") { gpuTarget = argv[++index]; explicitTarget = true; }
      else (argument == "--arch" ? architecture : outputPath) = argv[++index];
      continue;
    }
    if (!argument.empty() && argument.front() == '-' && argument != "-") {
      std::cerr << "metreonc: unknown option `" << argument << "`\n";
      printUsage(std::cerr);
      return 1;
    }
    if (!inputPath.empty()) {
      std::cerr << "metreonc: expected one input file\n";
      printUsage(std::cerr);
      return 1;
    }
    inputPath = argument;
  }

  if (inputPath.empty()) {
    std::cerr << "metreonc: no input file\n";
    printUsage(std::cerr);
    return 1;
  }

  const std::string sourceName = inputPath == "-" ? "<stdin>" : inputPath;
  const bool amdFormat = emitFormat == "rocdl" || emitFormat == "amdgpu" || emitFormat == "hsaco";
  if (amdFormat && !explicitTarget) gpuTarget = "amdgpu";
  if ((gpuTarget != "cuda" && gpuTarget != "amdgpu") || (amdFormat && gpuTarget != "amdgpu") ||
      ((emitFormat == "ptx" || emitFormat == "nvvm") && gpuTarget != "cuda")) {
    std::cerr << "metreonc: GPU target must be cuda or amdgpu and match the requested output dialect\n";
    return 1;
  }
  if (architecture.empty()) architecture = gpuTarget == "amdgpu" ? "gfx900" : "sm_80";
  if (emitFormat == "exe" || emitFormat == "ptx" || emitFormat == "nvvm" || amdFormat) contextOnly = true;
  if (contextOnly) optimize = false;
  if (contextOnly && emitFormat == "llvm") {
    std::cerr << "metreonc: --context-only currently supports GPU execution, not host LLVM emission\n";
    return 1;
  }
#ifndef METREON_ENABLE_MLIR
  if (emitFormat != "graphir") {
    std::cerr << "metreonc: MLIR emission requires a build with METREON_ENABLE_MLIR=ON\n";
    return 1;
  }
#endif
  try {
    const std::string source = readSource(inputPath);
    metreon::parser::Parser parser(source);
    const metreon::ast::Module module = parser.parseModule();
    metreon::graphir::Module graph =
        metreon::graphir::lowerModule(module, sourceName, contextOnly);
    if (optimize) metreon::graphir::passes::runDefaultPipeline(graph);
#ifdef METREON_ENABLE_MLIR
    if (emitFormat == "exe") {
      const auto destination = outputPath.empty() ? (inputPath == "-" ? "stdin" : std::filesystem::path(inputPath).stem().string()) : outputPath;
      metreon::emitExecutable(graph, {destination, architecture, gpuTarget == "amdgpu" ? metreon::GPUBackend::AMDGPU : metreon::GPUBackend::CUDA});
      std::cout << "Executable written to " << destination << '\n';
      return 0;
    }
#endif
    if (!outputPath.empty()) throw std::runtime_error("-o currently applies to executable output");
    // Serialize completely before opening an output so validation failures do
    // not truncate an existing artifact. Resolve output relative to the caller's
    // working directory, independently of the input or executable location.
    std::string outputText;
    std::vector<Artifact> intermediates;
    if (emitFormat == "graphir") outputText = graph.print();
#ifdef METREON_ENABLE_MLIR
    else {
      mlir::MLIRContext context;
      if (contextOnly) {
        auto emitted = emitDevice(graph, context, emitFormat, architecture, gpuTarget == "amdgpu");
        if (!emitted) return 1;
        intermediates = std::move(*emitted);
        outputText = std::move(intermediates.back().text);
        intermediates.pop_back();
      } else {
        auto imported = emitFormat == "llvm" ? metreon::lowerHostToLLVM(graph, context)
                        : emitFormat == "gpu" ? metreon::lowerGPUControlFlow(graph, context)
                                             : metreon::importGraphIR(graph, context);
        if (mlir::failed(imported)) return 1;
        outputText = printMLIR(**imported);
      }
    }
#endif
    const std::string outputLabel = emitFormat == "rocdl" ? "ROCDL MLIR" : emitFormat == "amdgpu" ? "AMDGPU assembly" : emitFormat == "hsaco" ? "HSACO" : emitFormat == "ptx" ? "PTX" : emitFormat == "nvvm" ? "NVVM MLIR" : emitFormat == "llvm" ? "LLVM MLIR" : emitFormat == "gpu" ? "GPU MLIR"
                                     : emitFormat == "mlir" ? "MLIR" : "GraphIR";
    const std::filesystem::path outputDirectory =
        std::filesystem::path("tmp") / emitFormat;
    std::filesystem::path outputName =
        inputPath == "-" ? "stdin.graphir"
                         : std::filesystem::path(inputPath).filename();
    outputName.replace_extension(emitFormat == "hsaco" ? ".hsaco" : emitFormat == "amdgpu" ? ".s" : emitFormat == "ptx" ? ".ptx" : emitFormat == "graphir" ? ".graphir" : ".mlir");
    for (const auto &artifact : intermediates) {
      const auto directory = std::filesystem::path("tmp") / artifact.format;
      auto name = outputName;
      name.replace_extension(artifact.extension);
      std::filesystem::create_directories(directory);
      std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
      file << artifact.text;
      file.close();
      if (!file) throw std::runtime_error("cannot write intermediate artifact " + (directory / name).generic_string());
    }
    std::filesystem::create_directories(outputDirectory);
    const auto outputPath = outputDirectory / outputName;
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open " + outputLabel + " output `" +
                               outputPath.generic_string() + "`");
    }
    output << outputText;
    output.close();
    if (!output) {
      throw std::runtime_error("cannot write " + outputLabel + " output `" +
                               outputPath.generic_string() + "`");
    }
    std::cout << outputLabel << " written to " << outputPath.generic_string() << '\n';
    return 0;
  } catch (const metreon::DiagnosticError &error) {
    std::cerr << metreon::formatDiagnostic(sourceName, error.diagnostic())
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "metreonc: " << error.what() << '\n';
  }
  return 1;
}
