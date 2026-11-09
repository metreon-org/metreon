#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/ControlFlow.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <variant>

namespace {
using namespace metreon::graphir;
using Value = std::variant<std::int64_t, std::vector<std::int64_t>>;
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
Module lower(const std::string &text) {
  return lowerModule(metreon::parser::Parser(text).parseModule(), "control-flow.mtr");
}

// A bounded test interpreter follows emitted CFG edges. It detects misplaced
// predicates, skipped loop steps and broken break/continue destinations rather
// than asserting that the implementation printed particular strings.
class Interpreter {
  const Module &module;
  std::size_t remaining = 100000;
public:
  std::map<std::string, unsigned> calls;
  explicit Interpreter(const Module &module) : module(module) {}
  Value run(const std::string &name, const std::vector<Value> &arguments = {}) {
    ++calls[name];
    auto found = std::find_if(module.procedureGraphs().begin(), module.procedureGraphs().end(),
        [&](const auto &graph) { return graph.name() == name; });
    check(found != module.procedureGraphs().end(), "interpreter cannot find procedure " + name);
    const auto &graph = *found;
    std::map<CallableValueId, Value> values;
    std::size_t index = 0;
    for (const auto &param : graph.parameters()) if (param.id) values.emplace(*param.id, arguments.at(index++));
    check(index == arguments.size(), "interpreter arity mismatch");
    std::map<CallableValueId, const CallableOperation *> operations;
    walkCallableOperations(graph.body(), [&](const auto &op) { operations.emplace(op.id, &op); });
    auto cfg = buildControlFlow(graph);
    std::map<ControlFlowBlockId, const ControlFlowBlock *> blocks;
    for (const auto &block : cfg.blocks) blocks.emplace(block.id, &block);
    struct Iterator { std::vector<std::int64_t> elements; std::size_t position = 0; };
    std::map<CallableValueId, Iterator> iterators;
    auto scalar = [&](CallableValueId id) { return std::get<std::int64_t>(values.at(id)); };
    auto current = cfg.entry;
    for (;;) {
      check(remaining-- > 0, "emitted CFG did not terminate within its test budget");
      const auto &block = *blocks.at(current);
      for (auto id : block.operations) {
        const auto &op = *operations.at(id);
        switch (op.kind) {
        case CallableOperationKind::Literal: {
          auto value = op.attributes.at("value");
          values[id] = value == "true" ? 1 : value == "false" ? 0 : std::stoll(value);
          break;
        }
        case CallableOperationKind::Variable:
          if (!op.operands.empty()) values[id] = values.at(op.operands[0]);
          else if (op.attributes.at("initialized") == "true") {
            auto value = op.attributes.at("initializer.value");
            values[id] = value == "true" ? 1 : value == "false" ? 0 : std::stoll(value);
          } else values.erase(id);
          break;
        case CallableOperationKind::Load: values[id] = values.at(op.operands[0]); break;
        case CallableOperationKind::Assign: values[op.operands[0]] = values.at(op.operands[1]); break;
        case CallableOperationKind::Unary: {
          const auto value = scalar(op.operands[0]);
          const auto &kind = op.attributes.at("operator");
          values[id] = kind == "!" ? !value : kind == "-" ? -value : value;
          break;
        }
        case CallableOperationKind::Binary: {
          const auto a = scalar(op.operands[0]), b = scalar(op.operands[1]);
          const auto &kind = op.attributes.at("operator");
          std::int64_t result;
          if (kind == "+") result = a + b;
          else if (kind == "-") result = a - b;
          else if (kind == "*") result = a * b;
          else if (kind == "/") { check(b != 0, "unexpected division by zero"); result = a / b; }
          else if (kind == "%") { check(b != 0, "unexpected remainder by zero"); result = a % b; }
          else if (kind == "==") result = a == b;
          else if (kind == "!=") result = a != b;
          else if (kind == "<") result = a < b;
          else if (kind == "<=") result = a <= b;
          else if (kind == ">") result = a > b;
          else if (kind == ">=") result = a >= b;
          else throw std::runtime_error("unknown emitted binary operator");
          values[id] = result;
          break;
        }
        case CallableOperationKind::Call: {
          std::vector<Value> args;
          for (auto operand : op.operands) args.push_back(values.at(operand));
          auto value = run(op.name, args);
          if (op.attributes.at("type") != "void") values[id] = std::move(value);
          break;
        }
        case CallableOperationKind::IteratorInit:
          iterators[id] = Iterator{std::get<std::vector<std::int64_t>>(values.at(op.operands[0]))};
          break;
        case CallableOperationKind::IteratorHasNext: {
          const auto &iterator = iterators.at(op.operands[0]);
          values[id] = static_cast<std::int64_t>(iterator.position < iterator.elements.size());
          break;
        }
        case CallableOperationKind::IteratorValue: {
          const auto &iterator = iterators.at(op.operands[0]);
          values[id] = iterator.elements.at(iterator.position);
          break;
        }
        case CallableOperationKind::IteratorAdvance: ++iterators.at(op.operands[0]).position; break;
        default: throw std::runtime_error("structured container appeared as a CFG instruction");
        }
      }
      const auto &term = block.terminator;
      if (term.kind == ControlFlowTerminatorKind::Return)
        return term.operands.empty() ? Value{std::int64_t{0}} : values.at(term.operands[0]);
      if (term.kind == ControlFlowTerminatorKind::Branch) current = term.successor.value();
      else current = scalar(term.operands.at(0)) ? term.successor.value() : term.falseSuccessor.value();
    }
  }
};

const std::string context = "context Host::Loop repeat_ctx;\n";
void executes(const std::string &program, std::int64_t expected) {
  for (bool optimize : {false, true}) {
    auto graph = lower(context + program);
    if (optimize) metreon::graphir::passes::runDefaultPipeline(graph);
    Interpreter interpreter(graph);
    check(std::get<std::int64_t>(interpreter.run("test")) == expected,
        "CFG execution disagrees with expected value in " + program);
    const auto text = graph.print();
    const auto cfg = text.find("\n  graphir.cfg {\n");
    check(cfg != std::string::npos, "missing existing CFG section");
    check(text.find("graphir.loop_section") == std::string::npos &&
          text.find("graphir.conditional_section") == std::string::npos, "separate control-flow section introduced");
    if (hasStructuredControlFlow(graph.procedureGraphs().back().body()))
      check(text.substr(0, cfg).find("loop_context(") == std::string::npos,
          "loop execution metadata escaped the CFG");
  }
}

void rejects(const std::string &program, const std::string &code) {
  try { auto module = lower(context + program); (void)module; }
  catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == code, "expected " + code + ", got " + error.diagnostic().code + ": " + error.what());
    return;
  }
  throw std::runtime_error("expected rejection: " + code);
}

void checkSemantics() {
  executes("procedure test() -> i32 { i32 x = 0; while<@cx: repeat_ctx> (x < 4) { x = x + 1; } return x; }", 4);
  executes("procedure test() -> i32 { i32 x = 0; until<@cx: repeat_ctx> (x == 4 is true) { x = x + 1; } return x; }", 4);
  executes("procedure test() -> i32 { i32 x = 0; until<@cx: repeat_ctx> (x < 4 is false) { x = x + 1; } return x; }", 4);
  executes("procedure test() -> i32 { i32 x = 0; until<@cx: repeat_ctx> (true is true) { x = 9; } return x; }", 0);
  executes("procedure test() -> i32 { i32 x = 0; until<@cx: repeat_ctx> (false is false) { x = 9; } return x; }", 0);
  executes("procedure test() -> i32 { i32 x = 0; while<@cx: repeat_ctx> (false) { x = 9; } return x; }", 0);
  executes("procedure test() -> i32 { i32 x = 0; loop (@cx: repeat_ctx) { x = x + 1; if (x < 4) { continue; } break; } return x; }", 4);
  executes("procedure test() -> i32 { i32 x = 0; for<@cx: repeat_ctx> (i32 i) in 0..5 { if (i == 2) { continue; } x = x + i; } return x; }", 8);
  executes("procedure test() -> i32 { i32 x = 0; for<@cx: repeat_ctx> (i32 i) in 4..4 { x = 9; } return x; }", 0);
  executes("procedure test() -> i32 { i32 x = 0; for<@cx: repeat_ctx> (i32 i) in 5..2 { x = 9; } return x; }", 0);
  executes("procedure test() -> i32 { i32 x = 0; for<@cx: repeat_ctx> (i32 i) in -2..2 { x = x + i; } return x; }", -2);
  executes("procedure test() -> i32 { i32 x = 0; for<@cx: repeat_ctx> (i32 i) in 0..3 { for<@cx: repeat_ctx> (i32 j) in 0..5 { if (j == 2) { break; } x = x + 1; } } return x; }", 6);
  executes("procedure test() -> i32 { i32 x; if (true) { x = 7; } else { x = 9; } return x; }", 7);
  executes("procedure test() -> i32 { if (false) { return 1; } else if (true) { return 2; } else { return 3; } }", 2);
  executes("procedure test() -> i32 { while<@cx: repeat_ctx> (true) { { return 7; } } return 8; }", 7);
  executes("procedure test() -> i32 { i32 x = 0; loop (@cx: repeat_ctx) { x = 4; break; } return x; }", 4);
  executes("procedure test() -> i32 { i32 x; loop (@cx: repeat_ctx) { x = 4; break; } return x; }", 4);
  executes("procedure test() -> i32 { i32 n = 0; i32 sum = 0; while<@cx: repeat_ctx> (n < 6) { n = n + 1; if (n % 2 == 0) { continue; } sum = sum + n; } return sum; }", 9);
  executes("procedure test() -> i32 { i32 n = 0; for<@cx: repeat_ctx> (i32 i) in 0..2 { for<@cx: repeat_ctx> (i32 j) in 0..3 { if (j == 1) { continue; } n = n + 1; } } return n; }", 4);
  executes("procedure test() -> i32 { i32 end = 3; i32 n = 0; for<@cx: repeat_ctx> (i32 i) in 0..end { end = 0; n = n + 1; } return n; }", 3);
  executes("procedure test() -> i32 { bool flag = true; flag = flag && false; if (flag) { return 0; } return 4; }", 4);
  executes("procedure test() -> i32 { if (false && (1 / 0 == 1)) { return 0; } if (true || (1 / 0 == 1)) { return 7; } return 0; }", 7);
  executes("procedure test() -> i32 { i32 x = 1 + 2 * 3; if (!(x != 7) && -x == -7) { return x; } return 0; }", 7);

  auto graph = lower(context + "procedure bound() -> i32 { return 4; } procedure test() -> i32 { i32 n = 0; for<@cx: repeat_ctx> (i32 i) in 0..bound() { n = n + 1; } return n; }");
  Interpreter interpreter(graph);
  check(std::get<std::int64_t>(interpreter.run("test")) == 4 && interpreter.calls.at("bound") == 1,
      "for bounds were re-evaluated on the backedge");

  auto predicates = lower(context + "procedure predicate(n: i32) -> bool { return n < 3; } procedure test() -> i32 { i32 n = 0; while<@cx: repeat_ctx> (predicate(n)) { n = n + 1; } return n; }");
  Interpreter reevaluated(predicates);
  check(std::get<std::int64_t>(reevaluated.run("test")) == 3 && reevaluated.calls.at("predicate") == 4,
      "while predicate was not re-evaluated on each header visit");

  rejects("procedure test() -> void { break; }", "sema.loop_control_outside_loop");
  rejects("procedure test() -> void { continue; }", "sema.loop_control_outside_loop");
  rejects("procedure test() -> void { if (1) {} }", "sema.condition_type");
  rejects("procedure test() -> void { while<@cx: repeat_ctx> (1) {} }", "sema.condition_type");
  rejects("procedure test() -> void { until<@cx: repeat_ctx> (true) {} }", "parse.unexpected_token");
  rejects("procedure test() -> void { until<@cx: repeat_ctx> (true is 1) {} }", "parse.unexpected_token");
  rejects("procedure test() -> void { loop (@cx: missing) {} }", "sema.unknown_loop_context");
  rejects("context Gpu::Loop gpu_loop; procedure test() -> void { loop (@cx: gpu_loop) {} }", "sema.loop_context_domain");
  rejects("context Host::Loop bad grants {work};", "parse.unexpected_token");
  rejects("context Host::Loop grants {work} bad;", "sema.loop_context_grants");
  rejects("procedure test() -> void { const i32 x = 0; x = 1; }", "sema.assignment_to_const");
  rejects("procedure test() -> void { for<@cx: repeat_ctx> (i32 i) in 0..4 { i = 3; } }", "sema.assignment_to_const");
  rejects("procedure test() -> i32 { i32 x; if (true) { x = 1; } return x; }", "sema.uninitialized_variable");
  rejects("procedure test() -> i32 { i32 x; while<@cx: repeat_ctx> (false) { x = 1; } return x; }", "sema.uninitialized_variable");
  rejects("procedure test() -> i32 { for<@cx: repeat_ctx> (i32 i) in 0..4 {} return i; }", "sema.unknown_variable");
  rejects("procedure test() -> i32 { if (true) { return 1; } }", "sema.missing_return");
  rejects("procedure test() -> void { loop (@cx: repeat_ctx) { continue; i32 x = 0; } }", "sema.unreachable_statement");
  rejects("procedure test(x: own buffer<global>) -> void { while<@cx: repeat_ctx> (true) { own buffer<global> moved = x; } }", "sema.loop_carried_ownership");
  rejects("procedure test(x: own buffer<global>) -> void { if (true) { own buffer<global> moved = x; } own buffer<global> moved_again = x; }", "sema.consumed_resource");
}
} // namespace

int main(int argc, char **argv) {
  try {
    checkSemantics();
    check(argc == 2, "expected tracked control-flow example");
    std::ifstream input(argv[1]);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    for (bool optimize : {false, true}) {
      auto graph = lower(text);
      if (optimize) metreon::graphir::passes::runDefaultPipeline(graph);
      Interpreter interpreter(graph);
      check(std::get<std::int64_t>(interpreter.run("count")) == 14, "combined loop example returned wrong result");
      check(std::get<std::int64_t>(interpreter.run("sum_positive", {std::vector<std::int64_t>{1, -2, 3, 0, 9}})) == 4,
          "view iteration did not preserve break/continue");
      check(std::get<std::int64_t>(interpreter.run("sum_positive", {std::vector<std::int64_t>{}})) == 0,
          "empty view executed its loop body");
      check(std::get<std::int64_t>(interpreter.run("advance", {std::int64_t{0}, std::int64_t{4}})) == 4,
          "mutable parameter did not preserve loop-carried updates");
      check(std::get<std::int64_t>(interpreter.run("initialized_by_branches", {std::int64_t{1}})) == 7 &&
            std::get<std::int64_t>(interpreter.run("initialized_by_branches", {std::int64_t{0}})) == 9,
          "branch initialization or join changed the selected value");
      check(graph.print().find("loop_context(#ctx1)") != std::string::npos, "GPU loop context missing from CFG");
    }
    std::cout << "structured CFG tests passed\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
