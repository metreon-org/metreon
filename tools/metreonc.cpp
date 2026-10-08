#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void printUsage(std::ostream &output) {
  output << "usage: metreonc [--emit=graphir] <input.mtr>\n"
            "       metreonc [--emit=graphir] -\n"
            "Writes <input-stem>.graphir (stdin.graphir for -) to\n"
            "  tmp/graphir/ relative to the current working directory.\n";
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

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout);
      return 0;
    }
    if (argument == "--emit=graphir") {
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
  try {
    const std::string source = readSource(inputPath);
    metreon::parser::Parser parser(source);
    const metreon::ast::Module module = parser.parseModule();
    const metreon::graphir::Module graph =
        metreon::graphir::lowerModule(module, sourceName);
    // Serialize completely before opening an output so validation failures do
    // not truncate an existing artifact. Resolve output relative to the caller's
    // working directory, independently of the input or executable location.
    const std::string graphir = graph.print();
    const std::filesystem::path outputDirectory =
        std::filesystem::path("tmp") / "graphir";
    std::filesystem::path outputName =
        inputPath == "-" ? "stdin.graphir"
                         : std::filesystem::path(inputPath).filename();
    outputName.replace_extension(".graphir");
    std::filesystem::create_directories(outputDirectory);
    const auto outputPath = outputDirectory / outputName;
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open GraphIR output `" +
                               outputPath.generic_string() + "`");
    }
    output << graphir;
    output.close();
    if (!output) {
      throw std::runtime_error("cannot write GraphIR output `" +
                               outputPath.generic_string() + "`");
    }
    std::cout << "GraphIR written to " << outputPath.generic_string() << '\n';
    return 0;
  } catch (const metreon::DiagnosticError &error) {
    std::cerr << metreon::formatDiagnostic(sourceName, error.diagnostic())
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "metreonc: " << error.what() << '\n';
  }
  return 1;
}
