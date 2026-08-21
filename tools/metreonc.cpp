#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void printUsage(std::ostream &output) {
  output << "usage: metreonc [--emit=graphir] <input.mtr>\n"
            "       metreonc [--emit=graphir] -\n";
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
        metreon::graphir::lowerContexts(module, sourceName);
    std::cout << graph.print();
    return 0;
  } catch (const metreon::DiagnosticError &error) {
    std::cerr << metreon::formatDiagnostic(sourceName, error.diagnostic())
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "metreonc: " << error.what() << '\n';
  }
  return 1;
}
