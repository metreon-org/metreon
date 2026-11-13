#include "metreon/MLIR/Import.h"
#include "metreon/MLIR/Dialect.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
metreon::graphir::Module lower(const std::string &text, const std::string &name = "test.mtr") {
  metreon::parser::Parser parser(text);
  return metreon::graphir::lowerModule(parser.parseModule(), name);
}
std::string print(mlir::ModuleOp module) {
  std::string text;
  llvm::raw_string_ostream out(text);
  module.print(out, mlir::OpPrintingFlags().enableDebugInfo());
  return text;
}
std::string inspect(metreon::graphir::Module &graph, mlir::MLIRContext &context) {
  const auto before = graph.print();
  auto imported = metreon::importGraphIR(graph, context);
  require(mlir::succeeded(imported), "GraphIR import failed");
  require(mlir::succeeded(mlir::verify(**imported)), "invalid imported module");
  const auto text = print(**imported);
  auto parsed = mlir::parseSourceString<mlir::ModuleOp>(text, &context);
  require(static_cast<bool>(parsed), "imported MLIR did not round-trip through the registered dialect");
  require(mlir::succeeded(mlir::verify(*parsed)), "round-trip verification failed");
  require(before == graph.print(), "import mutated GraphIR");
  require(text.find("metreon.literal") == std::string::npos &&
          text.find("metreon.variable") == std::string::npos &&
          text.find("metreon.return") == std::string::npos &&
          text.find("arith.constant") == std::string::npos,
          "ordinary computation was lowered in the semantic-only stage");
  require(text.find("metreon.ordinary_lowering = \"deferred\"") != std::string::npos,
          "missing explicit deferred-lowering marker");
  return text;
}
void expectFailure(metreon::graphir::Module &graph, mlir::MLIRContext &context,
                   const std::string &expected) {
  std::string messages;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &diagnostic) {
    llvm::raw_string_ostream out(messages);
    diagnostic.print(out);
    return mlir::success();
  });
  require(mlir::failed(metreon::importGraphIR(graph, context)), "invalid GraphIR was accepted");
  require(messages.find(expected) != std::string::npos, "wrong diagnostic: " + messages);
}

const char *contextProgram = R"mtr(
context Host::Process grants {work} first;
context Host::Process grants {work} second;
procedure worker(@cx: first) -> void !{work} where first allows {work} { return; }
procedure caller(@cx: first) -> void !{work} where first allows {work} {
  worker(@cx);
  worker(@cx);
  return;
}
)mtr";

void checkCore(mlir::MLIRContext &context) {
  auto graph = lower(contextProgram);
  metreon::graphir::passes::runDefaultPipeline(graph);
  const auto text = inspect(graph, context);
  require(text.find("sym_name = \"first\"") != std::string::npos &&
          text.find("sym_name = \"second\"") != std::string::npos,
          "nominal contexts were merged");
  require(text.find("kind = \"context_region\"") != std::string::npos,
          "optimized ambient context was lost");
  require(text.find("context = @first") != std::string::npos, "effective context reference missing");
  auto again = lower(contextProgram);
  metreon::graphir::passes::runDefaultPipeline(again);
  require(text == inspect(again, context), "import depends on process-local context identities");

  auto wrong = lower(contextProgram);
  auto body = wrong.procedureGraphs()[1].body();
  body[0].context = wrong.contexts()[1];
  wrong.procedureGraphs()[1].setBody(body);
  expectFailure(wrong, context, "different nominal execution context");

  auto foreign = lower(contextProgram);
  body = foreign.procedureGraphs()[1].body();
  body[0].context = graph.contexts()[0];
  foreign.procedureGraphs()[1].setBody(body);
  expectFailure(foreign, context, "context");

  auto wrongResult = lower(contextProgram);
  body = wrongResult.procedureGraphs()[1].body();
  body[0].attributes["type"] = "i32";
  wrongResult.procedureGraphs()[1].setBody(body);
  expectFailure(wrongResult, context, "call result type mismatch");

  auto wrongArguments = lower(contextProgram);
  body = wrongArguments.procedureGraphs()[1].body();
  body[0].operands.push_back(999);
  wrongArguments.procedureGraphs()[1].setBody(body);
  expectFailure(wrongArguments, context, "undefined");

  auto uninitialized = lower("procedure value() -> i32 { i32 x = 4; return x; }");
  body = uninitialized.procedureGraphs()[0].body();
  body[0].attributes["initialized"] = "false";
  body[0].attributes.erase("initializer.kind");
  body[0].attributes.erase("initializer.value");
  uninitialized.procedureGraphs()[0].setBody(body);
  expectFailure(uninitialized, context, "uninitialized");

  metreon::graphir::Module contextPayload("context-payload.mtr");
  auto &resource = contextPayload.addResourceGraph("ContextBox",
      {{"generic_arity", "1"}, {"generic_parameters", "C: Context"}}, {});
  auto state = resource.addState("S", {{"field_count", "1"},
      {"field.0.name", "evidence"}, {"field.0.type", "C"}}, {});
  resource.addEntryState(state);
  expectFailure(contextPayload, context, "context evidence cannot be a runtime type");
}

void checkTransitions(mlir::MLIRContext &context) {
  auto graph = lower(R"mtr(
resource R {
  state A(value: i32);
  state B(value: i32, ticket: own event<Test>);
  state C(value: i32);
  transition first<X: Context>(left: i32, @cx: X; self: own A, right: i32)
    -> B !{work} where X allows {work};
  await transition second<X: Context>(@cx: X; self: own B)
    -> C !{gpu_await} where X allows {gpu_await};
}
)mtr");
  metreon::graphir::passes::runDefaultPipeline(graph);
  auto imported = metreon::importGraphIR(graph, context);
  require(mlir::succeeded(imported), "collapsed resource import failed");
  unsigned states = 0, transitions = 0;
  (**imported).walk([&](metreon::StateOp) { ++states; });
  (**imported).walk([&](metreon::TransitionDeclOp op) {
    ++transitions;
    if (op.getSymName() == "first") {
      require(op.getSelfIndex() == 1, "self order changed after filtering context evidence");
      require(op.getFunctionType().getNumInputs() == 3, "context became a runtime input");
    }
    if (op.getSymName() == "second") require(op.getIsAwait(), "await contract was lost");
  });
  require(states == 3 && transitions == 2, "collapsed members or internal transitions disappeared");
  // Nested symbol references inside custom type storage must participate in
  // MLIR's recursive attribute/type replacement machinery.
  auto oldRef = mlir::SymbolRefAttr::get(&context, "R", {
      mlir::FlatSymbolRefAttr::get(&context, "A")});
  auto newRef = mlir::SymbolRefAttr::get(&context, "R", {
      mlir::FlatSymbolRefAttr::get(&context, "Start")});
  mlir::AttrTypeReplacer replacer;
  replacer.addReplacement([&](mlir::SymbolRefAttr ref) -> std::optional<mlir::Attribute> {
    if (ref == oldRef) return newRef;
    if (ref == mlir::FlatSymbolRefAttr::get(&context, "A"))
      return mlir::FlatSymbolRefAttr::get(&context, "Start");
    return std::nullopt;
  });
  replacer.recursivelyReplaceElementsIn((**imported).getOperation(), true, false, true);
  (**imported).walk([&](metreon::StateOp op) {
    if (op.getSymName() == "A") mlir::SymbolTable::setSymbolName(op, "Start");
  });
  require(mlir::succeeded(mlir::verify(**imported)), "resource symbol rename left stale type references");
  bool invalidSignatureRejected = false;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &) { return mlir::success(); });
  (**imported).walk([&](metreon::TransitionDeclOp op) {
    if (op.getSymName() == "first") op.setSelfIndex(99);
  });
  invalidSignatureRejected = mlir::failed(mlir::verify(**imported));
  require(invalidSignatureRejected, "malformed standalone MLIR signature was accepted");
  inspect(graph, context);
  graph.resourceGraphs()[0].states()[0].members[0].attributes["field_count"] = "2";
  expectFailure(graph, context, "field count");
}
} // namespace

int main(int argc, char **argv) {
  try {
    mlir::MLIRContext context;
    context.getOrLoadDialect<metreon::MetreonDialect>();
    checkCore(context);
    checkTransitions(context);
    for (int i = 1; i < argc; ++i) {
      std::ifstream input(argv[i]);
      require(static_cast<bool>(input), "cannot read fixture");
      const std::string text((std::istreambuf_iterator<char>(input)), {});
      for (bool optimize : {false, true}) {
        auto graph = lower(text, argv[i]);
        if (optimize) metreon::graphir::passes::runDefaultPipeline(graph);
        inspect(graph, context);
      }
    }
    std::cout << "Metreon semantic MLIR import checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
