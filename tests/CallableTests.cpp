#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <functional>
#include <fstream>
#include <iterator>
#include <unordered_set>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

metreon::ast::Module parse(const std::string &source) {
  return metreon::parser::Parser(source).parseModule();
}

void parsesCallableBodies() {
  const auto module = parse(R"(
procedure finish(@cx: host_ctx; slot: own Slot::Pending<gpu::global>)
    -> own Slot::Ready<gpu::global> !{poll}
    where host_ctx allows {poll} {
  {
    own Slot::Ready<gpu::global> ready = Slot::complete(@cx; slot);
    return ready;
  }
}
kernel work(@cx: gpu_ctx; data: buffer<gpu::global>) !{gpu_load}
    where gpu_ctx allows {gpu_load} {
  helper(@cx; data);
  return;
}
)");
  check(module.procedures.size() == 1 && module.kernels.size() == 1,
        "callable declaration kinds were lost");
  const auto &procedure = module.procedures.front();
  check(procedure.parameters.size() == 2 &&
            procedure.parameters[0].isContextEvidence &&
            procedure.parameters[1].type.str() ==
                "own Slot::Pending<gpu::global>" &&
            procedure.resultType.str() == "own Slot::Ready<gpu::global>" &&
            procedure.effects[0].str() == "poll" &&
            procedure.allowsClauses[0].context.str() == "host_ctx",
        "typed signature or requirements were lost");
  const auto &block = procedure.body.statements.front();
  check(block.kind == metreon::ast::StatementKind::Block &&
            block.statements.size() == 2,
        "nested statement order was lost");
  const auto &call = *block.statements[0].variable.initializer;
  check(call.kind == metreon::ast::ExpressionKind::Call &&
            call.name.str() == "Slot::complete" && call.arguments.size() == 2 &&
            call.arguments[0].kind == metreon::ast::ExpressionKind::ContextReference &&
            block.statements[1].expression->name.str() == "ready" &&
            block.statements[1].location.line > call.location.line,
        "call operands, return reference, or source locations were lost");
  check(module.kernels[0].resultType.str() == "void" &&
            module.kernels[0].body.statements[0].kind ==
                metreon::ast::StatementKind::Call &&
            !module.kernels[0].body.statements[1].expression,
        "kernel call or bare return was lost");
}

metreon::graphir::Module lower(const std::string &source) {
  return metreon::graphir::lowerModule(parse(source), "callables.mtr");
}

void rejects(const std::string &code, const std::string &source) {
  try {
    static_cast<void>(lower(source));
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == code,
          "expected " + code + ", got " + error.diagnostic().code +
              ": " + error.diagnostic().message + " in " + source);
    check(error.diagnostic().location.line != 0 &&
              error.diagnostic().location.column != 0,
          "diagnostic has no source location");
    return;
  }
  throw std::runtime_error("expected " + code + " for " + source);
}

void checksCallableDiagnostics() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"parse.expected_declaration", "proc f() -> void {}"},
      {"parse.unexpected_token", "procedure f() {}"},
      {"parse.unexpected_token", "procedure f(x) -> void {}"},
      {"parse.unterminated_block", "procedure f() -> void { {"},
      {"parse.unexpected_token", "kernel f() -> i32 { return 1; }"},
      {"sema.duplicate_callable", "procedure f() -> void {} procedure f() -> void {}"},
      {"sema.duplicate_callable", "kernel f() {} procedure f() -> void {}"},
      {"sema.duplicate_callable_parameter", "procedure f(x: i32, x: f32) -> void {}"},
      {"sema.duplicate_local_variable", "procedure f(x: i32) -> void { i32 x = 1; }"},
      {"sema.missing_return", "procedure f() -> i32 {}"},
      {"sema.missing_return_value", "procedure f() -> i32 { return; }"},
      {"sema.unexpected_return_value", "procedure f() -> void { return 1; }"},
      {"sema.unexpected_return_value", "kernel f() { return 1; }"},
      {"sema.type_mismatch", "procedure f() -> i32 { return true; }"},
      {"sema.type_mismatch", "procedure f() -> void { bool x = 1.0; }"},
      {"sema.type_mismatch", "procedure f() -> u32 { return -1; }"},
      {"sema.unknown_variable", "kernel f() { f32 value = other; }"},
      {"sema.unknown_variable", "procedure f() -> i32 { { i32 x = 1; } return x; }"},
      {"sema.uninitialized_variable", "procedure f() -> i32 { i32 x; return x; }"},
      {"sema.uninitialized_variable", "procedure f(x: i32) -> i32 { { i32 x = x; } return x; }"},
      {"sema.uninitialized_const_variable", "procedure f() -> void { const i32 x; }"},
      {"sema.unreachable_statement", "procedure f() -> void { { return; } i32 x; }"},
      {"sema.unknown_operation", "procedure f() -> void { missing(); }"},
      {"sema.call_arity", "procedure f(x: i32) -> i32 { return x; } procedure g() -> i32 { return f(); }"},
      {"sema.type_mismatch", "procedure f(x: i32) -> i32 { return x; } procedure g() -> i32 { return f(true); }"},
      {"sema.kernel_call", "kernel f() {} procedure g() -> void { f(); }"},
      {"sema.unknown_value_type", "procedure f(x: UnknownType) -> void {}"},
      {"sema.unknown_value_type", "procedure f() -> UnknownType {}"},
      {"sema.invalid_void_type", "procedure f(x: void) -> void {}"},
      {"sema.invalid_void_type", "procedure f() -> own void {}"},
      {"sema.type_arity", "procedure f(x: buffer) -> void {}"},
      {"sema.type_arity", "procedure f(x: i32<f32>) -> void {}"},
      {"sema.consumed_resource", R"(
procedure forward(x: own buffer<global>) -> own buffer<global> { return x; }
procedure f(x: own buffer<global>) -> own buffer<global> {
  { own buffer<global> moved = forward(x); }
  return x;
})"},
      {"sema.discarded_owned_result", R"(
procedure forward(x: own buffer<global>) -> own buffer<global> { return x; }
procedure f(x: own buffer<global>) -> void { forward(x); }
)"},
  };
  for (const auto &[code, source] : cases) rejects(code, source);
}

const std::string contexts = R"(
context Host::Process grants {poll, extra} host;
context Host::Process grants {poll, extra} other;
context Gpu::Thread grants {poll} gpu;
context Host::Restricted grants {} restricted;
)";

void checksContextsAndEffects() {
  rejects("sema.kernel_execution_context",
          "context Gpu grants {} gpu; kernel f(@cx: gpu) {}");
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"sema.unknown_execution_context", "procedure f(@cx: missing) -> void {}"},
      {"sema.invalid_execution_context", "procedure f(@other: host) -> void {}"},
      {"sema.invalid_execution_context", "procedure f(@cx: own host) -> void {}"},
      {"sema.kernel_execution_context", "kernel f(@cx: host) {}"},
      {"sema.missing_capability", "procedure f() -> void !{poll} {}"},
      {"sema.missing_capability", "procedure f(@cx: restricted) -> void !{poll} {}"},
      {"sema.invalid_context_requirement", "procedure f(@cx: host) -> void where other allows {poll} {}"},
      {"sema.missing_capability", "procedure f(@cx: gpu) -> void where gpu allows {extra} {}"},
      {"sema.runtime_context_value", "procedure f(cx: host) -> void {}"},
      {"sema.runtime_context_value", "procedure f(@cx: host) -> i32 { return @cx; }"},
      {"sema.runtime_context_value", "procedure f(@cx: host) -> i32 { return cx; }"},
      {"sema.runtime_context_value", "procedure f(@cx: host) -> void { i32 x = @cx; }"},
      {"sema.duplicate_local_variable", "procedure f(@cx: host) -> void { i32 cx = 1; }"},
      {"sema.execution_context_mismatch", R"(
procedure f(@cx: host) -> void !{poll} {}
procedure g(@cx: other) -> void !{poll} { f(@cx); }
)"},
      {"sema.execution_context_mismatch", R"(
procedure f(@cx: host) -> void !{poll} {}
kernel g(@cx: gpu) !{poll} { f(@cx); }
)"},
      {"sema.invalid_context_argument", R"(
procedure f(@cx: host) -> void {}
procedure g(@cx: host) -> void { f(@host); }
)"},
      {"sema.invalid_context_argument", R"(
procedure f(@cx: host) -> void {}
procedure g(@cx: host) -> void { f(1); }
)"},
      {"sema.undeclared_effect", R"(
procedure f(@cx: host) -> void !{poll} {}
procedure g(@cx: host) -> void { f(@cx); }
)"},
  };
  for (const auto &[code, source] : cases) rejects(code, contexts + source);
}

const std::string resource = R"(
resource Slot<S: AddressSpace> {
  state Pending();
  state Ready();
  transition complete<C: Context>(@cx: C; self: own Pending) -> Ready
      !{poll} where C allows {poll};
}
)";

void checksTransitions() {
  const std::string prefix = contexts + resource;
  rejects("sema.type_arity", prefix +
          "procedure f(x: own Slot::Pending) -> void {}");
  rejects("sema.resource_not_owned", prefix +
          "procedure f(x: Slot::Pending<global>) -> void {}");
  rejects("sema.type_mismatch", prefix + R"(
procedure f(@cx: host; x: own Slot::Ready<global>) -> own Slot::Ready<global> !{poll} {
  return Slot::complete(@cx; x);
})");
  rejects("sema.type_mismatch", prefix + R"(
procedure f(@cx: host; x: own Slot::Pending<global>) -> own Slot::Ready<shared> !{poll} {
  return Slot::complete(@cx; x);
})");
  rejects("sema.undeclared_effect", prefix + R"(
procedure f(@cx: host; x: own Slot::Pending<global>) -> own Slot::Ready<global> {
  return Slot::complete(@cx; x);
})");
  rejects("sema.consumed_resource", prefix + R"(
procedure f(@cx: host; x: own Slot::Pending<global>) -> own Slot::Ready<global> !{poll} {
  own Slot::Ready<global> ready = Slot::complete(@cx; x);
  return Slot::complete(@cx; x);
})");
  rejects("sema.type_mismatch", prefix + R"(
resource Other { state Pending(); }
procedure f(@cx: host; x: own Other::Pending) -> own Slot::Ready<global> !{poll} {
  return Slot::complete(@cx; x);
})");
  rejects("sema.duplicate_transition", R"(
resource R { state A(); transition f(self: own A) -> A; transition f(self: own A) -> A; }
)");
  rejects("sema.duplicate_callable", prefix +
          "procedure Slot::complete() -> void {}");
  rejects("sema.missing_capability", contexts + R"(
resource R {
  state A();
  transition advance<C: Context>(@cx: C; self: own A) -> A where C allows {extra};
}
procedure f(@cx: gpu; x: own R::A) -> own R::A { return R::advance(@cx; x); }
)");
  rejects("sema.consumed_resource", R"(
procedure take(a: own buffer<global>, b: own buffer<global>) -> void {}
procedure f(x: own buffer<global>) -> void { take(x, x); }
)");
  rejects("sema.invalid_context_argument", R"(
resource R {
  state A();
  transition step<C: Context>(self: own A, fake: C) -> A;
}
procedure f(x: own R::A) -> own R::A { return R::step(x, 1); }
)");
  rejects("sema.invalid_context_argument", R"(
resource R<C: Context> { state A(); }
procedure f(x: own R::A<i32>) -> own R::A<i32> { return x; }
)");
  rejects("sema.unresolved_type_argument", R"(
resource R {
  state A();
  transition step<T: Type>(self: own A) -> A;
}
procedure f(x: own R::A) -> own R::A { return R::step(x); }
)");
  rejects("sema.type_mismatch", R"(
resource R<S: AddressSpace> {
  state A();
  transition step(self: own A, src: buffer<S>) -> A;
}
procedure f(x: own R::A<global>, src: buffer<shared>) -> own R::A<global> {
  return R::step(x, src);
}
)");
  static_cast<void>(lower(R"(
resource R<S: AddressSpace> {
  state A();
  transition combine(self: own A, other: own A) -> A;
}
procedure f(x: own R::A<global>, y: own R::A<global>) -> own R::A<global> {
  return R::combine(x, y);
}
)"));
  static_cast<void>(lower(R"(
resource R<S: AddressSpace> {
  state A();
  transition combine<T: AddressSpace>(self: own A, other: own A<T>) -> A<T>;
}
procedure f(x: own R::A<global>, y: own R::A<shared>) -> own R::A<shared> {
  return R::combine(x, y);
}
)"));
}

void checksGenericTransitionLowering() {
  const auto graph = lower(R"(
context Host::Process grants {copy<gpu::global>} host;
resource Slot<S: AddressSpace> {
  state Pending();
  state Ready();
  transition complete<C: Context, T: Type>(
      @cx: C; self: own Pending<S>, src: view<T, S>
  ) -> Ready<S> !{copy<S>} where C allows {copy<S>};
}
procedure f(@cx: host; slot: own Slot::Pending<gpu::global>, src: view<f32, gpu::global>)
    -> own Slot::Ready<gpu::global> !{copy<gpu::global>} {
  return Slot::complete(@cx; slot, src);
}
)");
  const auto &procedure = graph.procedureGraphs().front();
  const auto &call = procedure.body().front();
  check(call.attributes.at("type") == "own Slot::Ready<gpu::global>" &&
            call.attributes.at("effects") == "copy<gpu::global>" &&
            call.operands == std::vector<metreon::graphir::CallableValueId>{0, 1} &&
            procedure.body().back().operands.front() == call.id,
        "generic state, effect substitution, or transition dataflow was lost");
}

void checksShadowingAndNestedCalls() {
  const auto graph = lower(R"(
procedure main(x: i32) -> i32 {
  { i32 x = 2; identity(x); }
  return identity(identity(x));
}
procedure identity(x: i32) -> i32 { return x; }
)");
  const auto &procedure = graph.procedureGraphs().front();
  const auto &body = procedure.body();
  check(body[0].body[1].operands.front() == body[0].body[0].id &&
            body[1].operands.front() == *procedure.parameters()[0].id &&
            body[2].operands.front() == body[1].id &&
            body[3].operands.front() == body[2].id,
        "shadowing, forward calls, or nested call evaluation order was lost");
  static_cast<void>(lower("procedure f() -> f64 { return 1.25; }"));
  static_cast<void>(lower("procedure f() -> u64 { return 1; }"));
  static_cast<void>(lower("procedure f() -> void { {} return; }"));
}

void checksExampleGraph(const std::string &path) {
  std::ifstream input(path);
  check(static_cast<bool>(input), "cannot read procedure example " + path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  auto graph = lower(source);
  check(graph.procedureGraphs().size() == 4 && graph.kernelGraphs().size() == 1,
        "example lost callable graphs");
  const auto &finish = graph.procedureGraphs()[0];
  const auto &kernel = graph.kernelGraphs()[0];
  check(finish.context()->identifier() == "host_ctx" &&
            kernel.context()->identifier() == "gpu_ctx" &&
            !finish.context()->hasSameIdentity(*kernel.context()) &&
            finish.parameters()[0].context->hasSameIdentity(*finish.context()) &&
            !finish.parameters()[0].id && finish.parameters()[1].id,
        "context parameters became values or lost execution identity");
  const auto &block = finish.body().front();
  const auto &call = block.body[0];
  const auto &variable = block.body[1];
  const auto &returned = block.body[2];
  check(call.attributes.at("await") == "true" &&
            call.attributes.at("callee_kind") == "transition" &&
            call.context->hasSameIdentity(*finish.context()) &&
            call.operands.front() == *finish.parameters()[1].id &&
            variable.operands.front() == call.id && returned.operands.front() == variable.id,
        "resource parameter to transition to local to return dataflow was lost");
  const auto &orchestrate = graph.procedureGraphs()[1];
  check(orchestrate.body()[2].attributes.at("callee_kind") == "procedure" &&
            orchestrate.body()[2].operands.front() == orchestrate.body()[1].id,
        "ordinary procedure call did not consume the preceding transition result");
  const auto &buffer = graph.procedureGraphs()[2];
  check(buffer.body()[0].operands.front() == *buffer.parameters()[0].id &&
            buffer.attributes().at("return_type") == "own buffer<gpu::global>",
        "owned buffer return was lost");
  const auto printed = graph.print();
  check(printed.find("graphir.procedure @\"finish\"") != std::string::npos &&
            printed.find("graphir.block") != std::string::npos &&
            printed.find("graphir.return operands(") != std::string::npos &&
            printed.find("execution_domain = \"Host\"") != std::string::npos &&
            printed.find("execution_domain = \"Gpu\"") != std::string::npos &&
            printed == lower(source).print(),
        "textual callable GraphIR is incomplete or nondeterministic");
  metreon::graphir::Module moved(std::move(graph));
  metreon::graphir::Module assigned("unused");
  assigned = std::move(moved);
  check(assigned.print() == printed && graph.procedureGraphs().empty() &&
            moved.procedureGraphs().empty(),
        "module moves lost callable graphs or context provenance");
}

} // namespace

int main(int argc, char **argv) {
  try {
    parsesCallableBodies();
    checksCallableDiagnostics();
    checksContextsAndEffects();
    checksTransitions();
    checksGenericTransitionLowering();
    checksShadowingAndNestedCalls();
    check(argc == 2, "expected procedure example path");
    checksExampleGraph(argv[1]);
    std::cout << "callable tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
