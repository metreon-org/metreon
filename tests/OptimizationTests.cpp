#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/Passes.h"
#include "metreon/Parser/Parser.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace metreon::graphir;
using namespace metreon::graphir::passes;

void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

Module lower(const std::string &source) {
  return lowerModule(metreon::parser::Parser(source).parseModule(), "optimization.mtr");
}

std::string cfg(const Module &module) {
  const auto text = module.print();
  return text.substr(text.find("\n  graphir.cfg {\n"));
}

void checksResourceReachability() {
  // Model a graph after an earlier rewrite disconnected part of its state
  // machine. Source validation intentionally still rejects such declarations.
  Module module("reachability.mtr");
  const auto context = module.addContextMetadata("Host::Process", "host", 0,
      ContextResolution::Declared, {0, 1, 1});
  auto &resource = module.addResourceGraph("R", {}, {0, 1, 1});
  for (const auto *name : {"Dead", "Entry", "Middle", "Exit", "Ghost"})
    resource.addState(name, {}, {0, 1, 1});
  resource.addEntryState(1);
  resource.addEntryState(3);
  resource.addTransition(1, 2, {{"name", "begin"}, {"effects", "poll"}}, {0, 1, 1});
  resource.addTransition(2, 3, {{"name", "finish"}, {"await", "true"},
                              {"effects", "gpu_await"}}, {0, 1, 1});
  resource.addTransition(0, 4, {{"name", "dead"}}, {0, 1, 1});
  resource.addTransition(4, 0, {{"name", "cycle"}}, {0, 1, 1});
  auto &specialization = module.addResourceContextTemplate("R", "C", {0, 1, 1})
                            .addSpecialization(context);
  for (const auto *name : {"begin", "finish", "dead"})
    specialization.checks.push_back({name, {}, {}, {0, 1, 1}});
  auto &library = module.addResourceGraph("Library", {}, {0, 1, 1});
  library.addState("First", {}, {0, 1, 1});
  library.addState("Other", {}, {0, 1, 1});
  DeadTransitionEliminationPass{}.run(module);
  auto &graph = module.resourceGraphs()[0];
  check(graph.states().size() == 3 && graph.states()[0].id == 1 &&
            graph.states()[1].name == "Middle" && graph.states()[2].name == "Exit" &&
            graph.transitions().size() == 2,
        "reachability used declaration order, removed reachable states, or retained a dead cycle");
  check(graph.transitions()[1].attributes.at("await") == "true" &&
            graph.transitions()[1].attributes.at("effects") == "gpu_await",
        "dead transition removal erased a live completion obligation");
  check(module.resourceGraphs()[1].states().size() == 2,
        "declaration without entry information was pruned");
  check(module.resourceContextTemplates()[0].specializations()[0].checks.size() == 2,
        "dead transitions left stale context template checks");
  const auto later = graph.addState("Later", {}, {0, 1, 1});
  check(later == 5, "state IDs were reused after removing states");
  graph.addTransition(3, later, {{"name", "later"}}, {0, 1, 1});
  check(graph.transitions().back().target == later,
        "sparse state IDs cannot be used after optimization");
}

void checksChainMetadata() {
  auto module = lower(R"(
resource R {
  state S1(value: f32); state S2(ticket: own event<Copy>); state S3();
  transition begin<C: Context>(@cx: C; self: own S1) -> S2
      !{poll} where C allows {poll};
  await transition finish<C: Context>(@cx: C; self: own S2) -> S3
      !{gpu_await} where C allows {gpu_await};
}
)");
  const auto originalStates = module.resourceGraphs()[0].states();
  const auto originalEdges = module.resourceGraphs()[0].transitions();
  StateChainCollapsePass{}.run(module);
  const auto &graph = module.resourceGraphs()[0];
  check(graph.states().size() == 1 && graph.transitions().empty(),
        "linear chain was not collapsed to one state");
  const auto &state = graph.states()[0];
  check(state.members.size() == 3 && state.internalTransitions.size() == 2,
        "collapsed chain lost member states or required transitions");
  for (std::size_t index = 0; index < originalStates.size(); ++index) {
    const auto &member = state.members[index];
    check(member.id == originalStates[index].id && member.name == originalStates[index].name &&
              member.attributes == originalStates[index].attributes &&
              member.location.line == originalStates[index].location.line,
          "chain collapse changed original state metadata");
  }
  for (std::size_t index = 0; index < originalEdges.size(); ++index)
    check(state.internalTransitions[index].attributes == originalEdges[index].attributes &&
              state.internalTransitions[index].source == originalEdges[index].source &&
              state.internalTransitions[index].target == originalEdges[index].target,
          "chain collapse changed transition ordering or contracts");
  const auto text = module.print();
  check(text.find("graphir.state_member %s1 \"S2\"") != std::string::npos &&
            text.find("graphir.internal_transition %s1 -> %s2") != std::string::npos,
        "collapsed metadata is absent from serialized GraphIR");
  StateChainCollapsePass{}.run(module);
  check(module.print() == text, "chain collapse is not idempotent");
  runDefaultPipeline(module);
  check(module.print() == text, "later passes lost collapsed transition metadata");
}

void checksSourceEntryBoundaries() {
  auto module = lower(R"(
resource R {
  state First(); state Exposed(); state Last();
  transition begin(self: own First) -> Exposed;
  transition finish(self: own Exposed) -> Last;
}
procedure accept(value: own R::Exposed) -> own R::Exposed { return value; }
)");
  runDefaultPipeline(module);
  const auto &graph = module.resourceGraphs()[0];
  check(graph.entryStates().size() == 2 && graph.entryStates()[0] == 0 &&
            graph.entryStates()[1] == 1 && graph.states().size() == 2 &&
            graph.states()[0].name == "First" && graph.states()[1].name == "Exposed",
        "source entry contract or externally referenced state identity was lost");
  check(graph.states()[1].members.size() == 2 && graph.transitions().size() == 1,
        "entry protection prevented an otherwise legal trailing chain collapse");
}

void checksChainBoundaries() {
  Module module("boundaries.mtr");
  auto &branch = module.addResourceGraph("Branch", {}, {0, 1, 1});
  for (const auto *name : {"A", "B", "C", "D", "E"}) branch.addState(name, {}, {0, 1, 1});
  for (const auto &[source, target] : std::vector<std::pair<ResourceNodeId, ResourceNodeId>>{
           {0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4}})
    branch.addTransition(source, target, {}, {0, 1, 1});
  auto &cycle = module.addResourceGraph("Cycle", {}, {0, 1, 1});
  cycle.addState("A", {}, {0, 1, 1}); cycle.addState("B", {}, {0, 1, 1});
  cycle.addTransition(0, 1, {}, {0, 1, 1}); cycle.addTransition(1, 0, {}, {0, 1, 1});
  auto &entry = module.addResourceGraph("Entry", {}, {0, 1, 1});
  for (const auto *name : {"A", "B", "C"}) entry.addState(name, {}, {0, 1, 1});
  entry.addTransition(0, 1, {}, {0, 1, 1}); entry.addTransition(1, 2, {}, {0, 1, 1});
  entry.addEntryState(1);
  StateChainCollapsePass{}.run(module);
  check(module.resourceGraphs()[0].states().size() == 4 &&
            module.resourceGraphs()[0].transitions().size() == 4,
        "chain collapse crossed a branch or join");
  check(module.resourceGraphs()[1].states().size() == 2 &&
            module.resourceGraphs()[1].transitions().size() == 2,
        "cyclic resource states were collapsed");
  check(module.resourceGraphs()[2].states().size() == 2 &&
            module.resourceGraphs()[2].states()[1].id == 1 &&
            module.resourceGraphs()[2].states()[1].members.size() == 2,
        "an independently referenced entry was absorbed into its predecessor");
}

void checksProtocolReachabilityAndReferences() {
  const std::string source = R"(
resource Existing { state Ready(); }
protocol Flow roles producer, consumer {
  state Dead { producer -> consumer: lost => Dead; }
  state Open { producer -> consumer: next => End; }
  state End final;
  state Other final;
  state TypedOnly final;
}
protocol Library roles a, b { state First final; state Second final; }
pipeline P(input x: f32, output y: f32) {
  edge data: channel<Flow> { initial Open; capacity 1; }
  stage send(src: f32, out: Flow::producer<Open>) -> void { return; }
  stage recv(in: Flow::consumer<Open>, dst: f32) -> void { return; }
  topology { x => send.src; send.out =>[data] recv.in; recv.dst => y; }
  schedule test { kernel worker = fuse(send, recv) { blocks 1; } }
}
pipeline OtherRoot() { edge data: channel<Flow> { initial Other; capacity 1; } }
pipeline TypedRoot() { stage keep(p: Flow::producer<TypedOnly>) -> void {} }
)";
  auto module = lower(source);
  runDefaultPipeline(module);
  const auto text = module.print();
  check(text.find("Flow::Dead") == std::string::npos &&
            text.find("Flow::Open") != std::string::npos &&
            text.find("Flow::End") != std::string::npos &&
            text.find("Flow::Other") != std::string::npos &&
            text.find("Flow::TypedOnly") != std::string::npos &&
            text.find("Library::Second") != std::string::npos,
        "protocol reachability lost a shared root or pruned an uninstantiated protocol");
  check(module.protocolGraphs()[0].members.size() == 4,
        "dead protocol member reference was retained");
  for (const auto &pipeline : module.pipelineGraphs())
    for (const auto member : pipeline.members) {
      const auto reference = module.resourceGraphs().at(member.graph).references().at(member.member);
      check(!module.definitionName(reference.target).empty(), "definition compaction left a dangling reference");
    }
  auto fresh = lower(source);
  runDefaultPipeline(fresh);
  check(text == fresh.print(), "optimized output is nondeterministic");
  runDefaultPipeline(module);
  check(text == module.print(), "default pipeline is not idempotent");
  Module moved(std::move(module));
  check(text == moved.print(), "moving optimized GraphIR invalidated its references");
}

void checksContextHoisting() {
  auto module = lower(R"(
context Host::Process grants {poll} host;
procedure step(@cx: host; x: i32) -> i32 !{poll} { return x; }
procedure work(@cx: host; x: i32) -> i32 !{poll} {
  i32 a = step(@cx; x);
  { step(@cx; a); step(@cx; a); }
  i32 b = step(@cx; a);
  return step(@cx; b);
}
pipeline P() {
  stage a(@cx: host; x: i32) -> i32 !{poll} { return step(@cx; x); }
  stage b(@cx: host; x: i32) -> i32 !{poll} { return step(@cx; x); }
  topology { a.x => b.x; }
  schedule test { kernel worker = fuse(a, b) { blocks 1; } }
}
)");
  const auto originalCFG = cfg(module);
  const auto originalBody = module.procedureGraphs()[1].body();
  ContextHoistingPass{}.run(module);
  check(cfg(module) == originalCFG, "context hoisting changed call order, value IDs, or lexical control flow");
  const auto &body = module.procedureGraphs()[1].body();
  check(body[2].kind == CallableOperationKind::Block &&
            body[2].body.size() == 1 &&
            body[2].body[0].kind == CallableOperationKind::ContextRegion &&
            body[2].body[0].context == module.procedureGraphs()[1].context() &&
            !body[2].body[0].body[0].context,
        "nested common evidence was not hoisted into a transparent region");
  check(body[3].kind == CallableOperationKind::ContextRegion &&
            body[3].body[0].id == originalBody[3].id &&
            body[3].body[0].attributes.at("effects") == "poll" &&
            body.back().kind == CallableOperationKind::Return,
        "context hoisting changed effects, operation identity, or return scope");
  bool foundStageRegion = false;
  for (const auto &definition : module.compositionDefinitions())
    for (const auto &record : definition.records)
      if (record.kind == "fused_kernel")
        foundStageRegion = record.contextRegions.size() == 1 &&
                           record.contextRegions[0].stages.size() == 2;
  check(foundStageRegion, "same-context fused stages lack a shared context region");
  const auto text = module.print();
  ContextHoistingPass{}.run(module);
  FusionLegalityPass{}.run(module);
  check(text == module.print(), "context hoisting is not idempotent");
}

void checksNominalContextBoundaries() {
  Module module("contexts.mtr");
  const auto first = module.addContextMetadata("Host::Process", "a", 0, ContextResolution::Declared, {0, 1, 1});
  const auto second = module.addContextMetadata("Host::Process", "b", 0, ContextResolution::Declared, {0, 1, 1});
  auto &callable = module.addProcedureGraph("work", {0, 1, 1});
  callable.setSignature({{"return_type", "void"}}, {});
  std::vector<CallableOperation> body;
  for (const auto &context : {first, second, first}) {
    auto call = callable.makeOperation(CallableOperationKind::Call, "step", {0, 1, 1});
    call.attributes.emplace("type", "void");
    call.context = context;
    body.push_back(std::move(call));
  }
  callable.setBody(std::move(body));
  ContextHoistingPass{}.run(module);
  check(callable.body().size() == 3 && callable.body()[0].kind == CallableOperationKind::Call,
        "same-spelled nominal contexts were incorrectly combined");
}

void checksCompletionCallsAndContextFreeBoundaries() {
  auto module = lower(R"(
context Host::Process grants {gpu_await} host;
resource R {
  state Pending(ticket: own event<Copy>); state Ready();
  await transition complete<C: Context>(@cx: C; self: own Pending) -> Ready
      !{gpu_await} where C allows {gpu_await};
}
procedure plain() -> void {}
procedure wait_both(@cx: host; x: own R::Pending, y: own R::Pending)
    -> void !{gpu_await} {
  own R::Ready first = R::complete(@cx; x);
  own R::Ready second = R::complete(@cx; y);
  return;
}
procedure separate(@cx: host; x: own R::Pending, y: own R::Pending)
    -> void !{gpu_await} {
  own R::Ready first = R::complete(@cx; x);
  plain();
  own R::Ready second = R::complete(@cx; y);
}
)");
  const auto originalCFG = cfg(module);
  runDefaultPipeline(module);
  const auto &region = module.procedureGraphs()[1].body()[0];
  check(region.kind == CallableOperationKind::ContextRegion && region.body.size() == 3 &&
            region.body[0].attributes.at("await") == "true" &&
            region.body[2].attributes.at("await") == "true" &&
            region.body[0].operands != region.body[2].operands,
        "optimization removed, merged, or changed a required completion call");
  const auto &separate = module.procedureGraphs()[2].body();
  check(separate.size() == 5 && separate[0].context && separate[3].context &&
            separate[2].name == "plain" && cfg(module) == originalCFG,
        "context hoisting crossed an independent call or changed completion order");
  const auto text = module.print();
  runDefaultPipeline(module);
  check(module.print() == text, "repeated default optimization changed completion contracts");
}

std::string fusionSource(const std::string &fused, const std::string &extra = "") {
  return "pipeline P() {"
         "stage a(x: i32) -> i32 { return x; }"
         "stage b(x: i32) -> i32 { return x; }"
         "stage c(x: i32) -> i32 { return x; }"
         "topology { a.x => b.x; b.x => c.x; " + extra + " }"
         "schedule test { kernel worker = fuse(" + fused + ") { blocks 1; } } }";
}

void rejectsFusion(const std::string &source, const std::string &code) {
  auto module = lower(source);
  try {
    FusionLegalityPass{}.run(module);
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == "graphir." + code,
          "unexpected fusion diagnostic: " + error.diagnostic().code);
    check(error.diagnostic().location.line > 0 && error.diagnostic().location.column > 0,
          "fusion diagnostic lost its source location");
    return;
  }
  throw std::runtime_error("fusion should have failed: " + code);
}

void checksFusionLegality() {
  auto valid = lower(fusionSource("a,b,c"));
  FusionLegalityPass{}.run(valid);
  rejectsFusion(fusionSource("c,b,a"), "fusion_order");
  rejectsFusion(fusionSource("a,c"), "fusion_dependency_cycle");
  rejectsFusion(fusionSource("a,b,c", "c.x => a.x;"), "cyclic_fusion");
  rejectsFusion(R"(
context Host::Process grants {} first;
context Host::Process grants {} second;
pipeline P() {
  stage a(@cx: first) -> void {}
  stage b(@cx: second) -> void {}
  schedule test { kernel worker = fuse(a, b) { blocks 1; } }
}
)", "fusion_context_mismatch");
  rejectsFusion(R"(
pipeline P() {
  stage a() -> i32 { return b(); }
  stage b() -> i32 { return a(); }
  schedule test { kernel worker = fuse(a, b) { blocks 1; } }
}
)", "cyclic_fusion");
}
} // namespace

int main() {
  try {
    checksResourceReachability();
    checksChainMetadata();
    checksSourceEntryBoundaries();
    checksChainBoundaries();
    checksProtocolReachabilityAndReferences();
    checksContextHoisting();
    checksNominalContextBoundaries();
    checksCompletionCallsAndContextFreeBoundaries();
    checksFusionLegality();
    std::cout << "optimization tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
