#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Parser/Parser.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using metreon::graphir::Edge;
using metreon::graphir::EdgeKind;
using metreon::graphir::Module;
using metreon::graphir::Node;
using metreon::graphir::NodeId;
using metreon::graphir::NodeKind;

void check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

metreon::ast::Module parse(const std::string &source) {
  metreon::parser::Parser parser(source);
  return parser.parseModule();
}

const Node *findNode(const Module &graph, NodeKind kind, const std::string &name,
                     const std::string &owner = "") {
  for (const Node &node : graph.nodes()) {
    if (node.kind != kind || node.name != name) {
      continue;
    }
    if (owner.empty()) {
      return &node;
    }
    const auto ownerAttribute = node.attributes.find("owner");
    if (ownerAttribute != node.attributes.end() &&
        ownerAttribute->second == owner) {
      return &node;
    }
  }
  return nullptr;
}

bool hasEdge(const Module &graph, NodeId source, NodeId target, EdgeKind kind,
             const std::string &path = "") {
  for (const Edge &edge : graph.edges()) {
    if (edge.source != source || edge.target != target || edge.kind != kind) {
      continue;
    }
    if (path.empty()) {
      return true;
    }
    const auto pathAttribute = edge.attributes.find("path");
    if (pathAttribute != edge.attributes.end() && pathAttribute->second == path) {
      return true;
    }
  }
  return false;
}

void expectDiagnostic(const std::string &expectedCode,
                      const std::function<void()> &operation) {
  try {
    operation();
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == expectedCode,
          "expected diagnostic `" + expectedCode + "`, got `" +
              error.diagnostic().code + "`");
    return;
  }
  throw std::runtime_error("expected diagnostic `" + expectedCode + "`");
}

const char *contextSource = R"(
context Host::Process grants {
  atomic,
  allocate,
  sleep,
  block,
  host_io
};

context Host::SoftIrq grants {
  atomic,
  mmio,
  defer<Host::Process>
};

context Host::HardIrq<I: Irq> grants {
  atomic,
  mmio,
  irq_ack<I>,
  defer<Host::SoftIrq>
};

context Host::Callback<S: StreamId> grants {
  atomic,
  defer<Host::Process>
};
)";

void parsesContextDeclarations() {
  const metreon::ast::Module module = parse(contextSource);
  check(module.contexts.size() == 4, "expected four context declarations");

  const metreon::ast::ContextDeclaration &hardIrq = module.contexts[2];
  check(hardIrq.name.str() == "Host::HardIrq", "lost qualified context name");
  check(hardIrq.parameters.size() == 1, "lost HardIrq context variable");
  check(hardIrq.parameters[0].name == "I", "wrong context variable name");
  check(hardIrq.parameters[0].constraint.str() == "Irq",
        "wrong context variable constraint");
  check(hardIrq.grants.size() == 4, "wrong HardIrq grant count");
  check(hardIrq.grants[2].capability.str() == "irq_ack<I>",
        "lost parameterized grant");

  const metreon::ast::Module empty =
      parse("context Host::Callback<S: StreamId> grants {};");
  check(empty.contexts.front().grants.empty(),
        "empty grant set should be accepted");
}

void lowersContextTopology() {
  const Module graph = metreon::graphir::lowerContexts(parse(contextSource),
                                                        "contexts.mtr");

  const Node *process =
      findNode(graph, NodeKind::Context, "Host::Process");
  const Node *softIrq =
      findNode(graph, NodeKind::Context, "Host::SoftIrq");
  const Node *hardIrq =
      findNode(graph, NodeKind::Context, "Host::HardIrq");
  const Node *callback =
      findNode(graph, NodeKind::Context, "Host::Callback");
  const Node *irqVariable =
      findNode(graph, NodeKind::ContextVariable, "I", "Host::HardIrq");
  const Node *streamVariable =
      findNode(graph, NodeKind::ContextVariable, "S", "Host::Callback");
  const Node *irqAck =
      findNode(graph, NodeKind::Grant, "irq_ack<I>", "Host::HardIrq");
  const Node *hardIrqDefer = findNode(graph, NodeKind::Grant,
                                     "defer<Host::SoftIrq>",
                                     "Host::HardIrq");
  const Node *callbackDefer = findNode(graph, NodeKind::Grant,
                                      "defer<Host::Process>",
                                      "Host::Callback");

  check(process != nullptr && softIrq != nullptr && hardIrq != nullptr &&
            callback != nullptr,
        "missing context node");
  check(irqVariable != nullptr && streamVariable != nullptr,
        "missing context-variable node");
  check(irqAck != nullptr && hardIrqDefer != nullptr &&
            callbackDefer != nullptr,
        "missing grant node");

  check(hasEdge(graph, hardIrq->id, irqVariable->id, EdgeKind::Binds),
        "HardIrq must bind I");
  check(hasEdge(graph, callback->id, streamVariable->id, EdgeKind::Binds),
        "Callback must bind S even when no grant consumes it");
  check(hasEdge(graph, hardIrq->id, irqAck->id, EdgeKind::Grants),
        "HardIrq must own irq_ack<I>");
  check(hasEdge(graph, irqAck->id, irqVariable->id, EdgeKind::Argument, "0"),
        "irq_ack<I> must reference I through argument edge 0");
  check(hasEdge(graph, hardIrqDefer->id, softIrq->id,
                EdgeKind::Transition),
        "HardIrq defer must transition to SoftIrq");
  check(hasEdge(graph, callbackDefer->id, process->id,
                EdgeKind::Transition),
        "Callback defer must transition to Process");

  const std::string firstPrint = graph.print();
  const std::string secondPrint = graph.print();
  check(firstPrint == secondPrint, "GraphIR printing must be deterministic");
  check(firstPrint.find("graphir.context_var \"I\"") != std::string::npos,
        "textual GraphIR omitted I node");
  check(firstPrint.find("loc(\"contexts.mtr\":") != std::string::npos,
        "textual GraphIR omitted source provenance");
}

void preservesNestedVariableReferences() {
  const std::string source = R"(
/* nested generic argument */
context Gpu::Thread<D: Device, U: Uniformity> grants {
  gpu_barrier<Scope<U>>, // trailing grant comma is accepted
};
)";
  const Module graph =
      metreon::graphir::lowerContexts(parse(source), "nested.mtr");
  const Node *grant = findNode(graph, NodeKind::Grant,
                               "gpu_barrier<Scope<U>>", "Gpu::Thread");
  const Node *uniformity =
      findNode(graph, NodeKind::ContextVariable, "U", "Gpu::Thread");
  check(grant != nullptr && uniformity != nullptr,
        "nested generic nodes were not lowered");
  check(hasEdge(graph, grant->id, uniformity->id, EdgeKind::Argument, "0.0"),
        "nested context-variable reference must retain argument path 0.0");
}

void preservesExternalTransitionTargets() {
  const std::string source = R"(
context Host::HardIrq<I: Irq> grants {
  defer<Host::SoftIrq>
};
)";
  const Module graph =
      metreon::graphir::lowerContexts(parse(source), "partial.mtr");
  const Node *grant = findNode(graph, NodeKind::Grant,
                               "defer<Host::SoftIrq>", "Host::HardIrq");
  const Node *target =
      findNode(graph, NodeKind::ExternalContext, "Host::SoftIrq");
  check(grant != nullptr && target != nullptr,
        "unresolved defer target must remain an explicit node");
  check(hasEdge(graph, grant->id, target->id, EdgeKind::Transition),
        "unresolved defer target must remain connected");

  const std::string genericSource =
      "context Host::Any<C: Context> grants { defer<C> };";
  const Module genericGraph = metreon::graphir::lowerContexts(
      parse(genericSource), "generic-transition.mtr");
  const Node *genericGrant =
      findNode(genericGraph, NodeKind::Grant, "defer<C>", "Host::Any");
  const Node *genericTarget =
      findNode(genericGraph, NodeKind::ContextVariable, "C", "Host::Any");
  check(genericGrant != nullptr && genericTarget != nullptr,
        "generic defer target nodes were not lowered");
  check(hasEdge(genericGraph, genericGrant->id, genericTarget->id,
                EdgeKind::Transition),
        "defer<C> must transition through its context-variable node");
}

void diagnosesInvalidInput() {
  expectDiagnostic("sema.duplicate_context", [] {
    const auto module = parse(
        "context Host::Process grants {}; "
        "context Host::Process grants {};");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-context.mtr"));
  });

  expectDiagnostic("sema.duplicate_context_variable", [] {
    const auto module = parse(
        "context Gpu::Thread<D: Device, D: Device> grants {};");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-variable.mtr"));
  });

  expectDiagnostic("sema.duplicate_grant", [] {
    const auto module =
        parse("context Host::Process grants { atomic, atomic };");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-grant.mtr"));
  });

  expectDiagnostic("sema.defer_arity", [] {
    const auto module = parse("context Host::Process grants { defer };");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "bad-defer.mtr"));
  });

  expectDiagnostic("parse.expected_context", [] {
    static_cast<void>(parse("fn acknowledge() {}"));
  });

  expectDiagnostic("lex.unexpected_character", [] {
    static_cast<void>(parse("context Host::Process grants { $ };"));
  });

  expectDiagnostic("lex.unterminated_block_comment", [] {
    static_cast<void>(parse("/* never closed"));
  });
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"parses context declarations", parsesContextDeclarations},
      {"lowers context topology", lowersContextTopology},
      {"preserves nested variable references", preservesNestedVariableReferences},
      {"preserves external transition targets", preservesExternalTransitionTargets},
      {"diagnoses invalid input", diagnosesInvalidInput},
  };

  std::size_t failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}
