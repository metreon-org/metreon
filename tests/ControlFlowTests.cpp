#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string lower(const std::string &source) {
  return metreon::graphir::lowerModule(
             metreon::parser::Parser(source).parseModule(), "cfg.mtr")
      .print();
}

std::string cfgSection(const std::string &printed) {
  const auto position = printed.find("\n  graphir.cfg {\n");
  check(position != std::string::npos, "GraphIR is missing its CFG section");
  return printed.substr(position);
}

void checksEmptyVoidAndExplicitReturns() {
  const auto printed = lower(
      "procedure empty() -> void {}\n"
      "procedure identity(x: i32) -> i32 { return x; }\n"
      "kernel work() { return; }\n");
  const auto cfg = cfgSection(printed);
  check(cfg.find("graphir.cfg.procedure @\"empty\" entry(^p0bb0) parameters()") !=
            std::string::npos &&
            cfg.find("graphir.cfg.return {implicit = \"true\"}") !=
                std::string::npos,
        "empty void body has no entry or implicit return");
  check(cfg.find("graphir.cfg.procedure @\"identity\" entry(^p1bb0) parameters(%p1v0)") !=
            std::string::npos &&
            cfg.find("graphir.cfg.return #p1op1 operands(%p1v0)") !=
                std::string::npos &&
            printed.find("graphir.return operands(%p1v0) id(#p1op1)") !=
                std::string::npos,
        "explicit return lost its operand or original operation identity");
  check(cfg.find("graphir.cfg.kernel @\"work\" entry(^k0bb0) parameters()") !=
            std::string::npos &&
            cfg.find("graphir.cfg.return #k0op0") != std::string::npos,
        "kernel CFG is missing or confused with a procedure CFG");
  check(printed == lower(
      "procedure empty() -> void {}\n"
      "procedure identity(x: i32) -> i32 { return x; }\n"
      "kernel work() { return; }\n"),
        "CFG printing is nondeterministic");
}

void checksNestedReturnExitsCallable() {
  const auto printed = lower(
      "procedure f(x: i32) -> i32 { { { return x; } } }");
  const auto cfg = cfgSection(printed);
  check(cfg.find("graphir.cfg.br ^p0bb1") != std::string::npos &&
            cfg.find("graphir.cfg.br ^p0bb2") != std::string::npos &&
            cfg.find("graphir.cfg.block ^p0bb2 scopes(#p0op1, #p0op2)") !=
                std::string::npos &&
            cfg.find("graphir.cfg.return #p0op3 operands(%p0v0)") !=
                std::string::npos,
        "nested return lost the enclosing scope path or callable exit");
  check(cfg.find("^p0bb3") == std::string::npos &&
            cfg.find("implicit") == std::string::npos &&
            cfg.find("graphir.cfg.op #p0op3") == std::string::npos,
        "nested return created a continuation or a second execution");
  check(printed.find("graphir.block id(#p0op1)") != std::string::npos &&
            printed.find("graphir.block id(#p0op2)") != std::string::npos,
        "CFG scopes cannot be resolved to the original lexical blocks");
}

void checksScopeFallthroughAndCallOrder() {
  const auto cfg = cfgSection(lower(
      "procedure identity(x: i32) -> i32 { return x; }\n"
      "procedure main(x: i32) -> i32 {\n"
      "  { i32 x = 2; identity(x); }\n"
      "  return identity(identity(x));\n"
      "}\n"));
  check(cfg.find("graphir.cfg.block ^p1bb1 scopes(#p1op1)") !=
            std::string::npos &&
            cfg.find("graphir.cfg.op #p1op2\n        graphir.cfg.op #p1op3\n") !=
                std::string::npos &&
            cfg.find("graphir.cfg.br ^p1bb2") != std::string::npos &&
            cfg.find("graphir.cfg.block ^p1bb2 scopes()") != std::string::npos &&
            cfg.find("graphir.cfg.op #p1op4\n        graphir.cfg.op #p1op5\n") !=
                std::string::npos &&
            cfg.find("graphir.cfg.return #p1op6 operands(%p1v5)") !=
                std::string::npos,
        "CFG changed call order or retained an exited lexical scope");
}

void checksEmptyScopesAndContextOnlyModule() {
  const auto cfg = cfgSection(lower("procedure f() -> void { {} {} }"));
  check(cfg.find("graphir.cfg.block ^p0bb1 scopes(#p0op0)") !=
            std::string::npos &&
            cfg.find("graphir.cfg.block ^p0bb3 scopes(#p0op1)") !=
                std::string::npos &&
            cfg.find("graphir.cfg.block ^p0bb4 scopes()") != std::string::npos &&
            cfg.find("graphir.cfg.return {implicit = \"true\"}") !=
                std::string::npos,
        "empty scope boundaries or final implicit return were lost");
  check(cfgSection(lower("context Host::Process grants {} host;")) ==
            "\n  graphir.cfg {\n  }\n}\n",
        "context-only module did not end with an empty CFG section");
}

} // namespace

int main() {
  try {
    checksEmptyVoidAndExplicitReturns();
    checksNestedReturnExitsCallable();
    checksScopeFallthroughAndCallOrder();
    checksEmptyScopesAndContextOnlyModule();
    std::cout << "control-flow tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
