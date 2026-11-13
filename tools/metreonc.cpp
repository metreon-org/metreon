#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"
#ifdef METREON_ENABLE_MLIR
#include "metreon/MLIR/Import.h"
#include "metreon/MLIR/HostLowering.h"
#include "metreon/MLIR/GPULowering.h"
#include "mlir/IR/MLIRContext.h"
#include "llvm/Support/raw_ostream.h"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void printUsage(std::ostream &output) {
  output << "usage: metreonc [--emit=graphir|mlir|llvm|gpu] [--no-optimization] <input.mtr>\n"
            "       metreonc [--emit graphir|mlir|llvm|gpu] [--no-optimization] -\n"
            "GraphIR optimization passes run by default; --no-optimization skips them.\n"
            "Writes <input-stem>.graphir or .mlir (stdin for -) to\n"
            "  tmp/graphir/, tmp/mlir/, tmp/llvm/, or tmp/gpu/ relative to the working directory.\n"
            "MLIR requires METREON_ENABLE_MLIR=ON; --emit=mlir preserves deferred bodies.\n"
            "--emit=llvm converts checked host scalar GraphIR CFGs to LLVM-dialect MLIR.\n"
            "--emit=gpu lowers checked GPU scalar control flow to affine/scf MLIR.\n";
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

} // namespace

int main(int argc, char **argv) {
  std::string inputPath;
  bool optimize = true;
  std::string emitFormat = "graphir";

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout);
      return 0;
    }
    if (argument == "--emit=graphir" || argument == "--emit=mlir" || argument == "--emit=llvm" || argument == "--emit=gpu") {
      emitFormat = argument.substr(7);
      continue;
    }
    if (argument == "--emit") {
      if (index + 1 >= argc || (std::string(argv[index + 1]) != "graphir" &&
                               std::string(argv[index + 1]) != "mlir" && std::string(argv[index + 1]) != "llvm" &&
                               std::string(argv[index + 1]) != "gpu")) {
        std::cerr << "metreonc: --emit requires the format `graphir`, `mlir`, `llvm`, or `gpu`\n";
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
        metreon::graphir::lowerModule(module, sourceName);
    if (optimize) metreon::graphir::passes::runDefaultPipeline(graph);
    // Serialize completely before opening an output so validation failures do
    // not truncate an existing artifact. Resolve output relative to the caller's
    // working directory, independently of the input or executable location.
    std::string outputText;
    if (emitFormat == "graphir") outputText = graph.print();
#ifdef METREON_ENABLE_MLIR
    else {
      mlir::MLIRContext context;
      auto imported = emitFormat == "llvm" ? metreon::lowerHostToLLVM(graph, context)
                      : emitFormat == "gpu" ? metreon::lowerGPUControlFlow(graph, context)
                                           : metreon::importGraphIR(graph, context);
      if (mlir::failed(imported)) return 1;
      llvm::raw_string_ostream output(outputText);
      (*imported)->print(output, mlir::OpPrintingFlags().enableDebugInfo());
      output << '\n';
    }
#endif
    const std::string outputLabel = emitFormat == "llvm" ? "LLVM MLIR" : emitFormat == "gpu" ? "GPU MLIR"
                                     : emitFormat == "mlir" ? "MLIR" : "GraphIR";
    const std::filesystem::path outputDirectory =
        std::filesystem::path("tmp") / emitFormat;
    std::filesystem::path outputName =
        inputPath == "-" ? "stdin.graphir"
                         : std::filesystem::path(inputPath).filename();
    outputName.replace_extension(emitFormat == "graphir" ? ".graphir" : ".mlir");
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
