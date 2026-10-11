#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/GraphIR/ControlFlow.h"
#include "metreon/Parser/Parser.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

metreon::graphir::Module lower(const std::string &source) {
  return metreon::graphir::lowerModule(
      metreon::parser::Parser(source).parseModule(), "composition.mtr");
}

std::string replace(std::string source, const std::string &from,
                    const std::string &to) {
  const auto position = source.find(from);
  check(position != std::string::npos, "test replacement not found: " + from);
  source.replace(position, from.size(), to);
  return source;
}

void rejects(const std::string &source, const std::string &code) {
  try {
    static_cast<void>(lower(source));
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == code,
          "expected " + code + ", got " + error.diagnostic().code + ": " +
              error.diagnostic().message);
    check(error.diagnostic().location.line > 0 &&
              error.diagnostic().location.column > 0,
          "diagnostic lost source location");
    return;
  }
  throw std::runtime_error("expected rejection: " + code);
}

void checksExample(const std::string &source) {
  auto graph = lower(source);
  check(graph.protocolGraphs().size() == 1 && graph.pipelineGraphs().size() == 1 &&
            graph.stageGraphs().size() == 3 && graph.compositionDefinitions().size() == 7,
        "composition definitions missing from in-memory GraphIR");
  const std::vector<std::string> expectedMembers = {
      "TiledGemm::epoch", "TiledGemm::a_tiles", "TiledGemm::b_tiles",
      "TiledGemm::load_a", "TiledGemm::load_b", "TiledGemm::mma",
      "TiledGemm::topology", "TiledGemm::sm90"};
  const auto &members = graph.pipelineGraphs()[0].members;
  check(members.size() == expectedMembers.size(), "pipeline membership incomplete");
  for (std::size_t index = 0; index < members.size(); ++index) {
    const auto &member = members[index];
    const auto &reference = graph.resourceGraphs().at(member.graph).references().at(member.member);
    check(graph.definitionName(reference.target) == expectedMembers[index],
          "pipeline reference resolves to the wrong definition");
  }
  check(graph.protocolGraphs()[0].members.size() == 2 &&
            graph.stageGraphs()[0].body()[0].operands[0] == *graph.stageGraphs()[0].parameters()[0].id,
        "protocol membership or stage operand linkage missing");
  const auto text = graph.print();
  for (const auto &expected : {
           "graphir.protocol @\"TileFlow\"", "graphir.pipeline @\"TiledGemm\"",
           "graphir.protocol_state @\"TileFlow::Open\"",
           "graphir.pipeline_state @\"TiledGemm::epoch\"",
           "graphir.pipeline_edge @\"TiledGemm::a_tiles\"",
           "graphir.stage @\"TiledGemm::load_a\"",
           "graphir.topology @\"TiledGemm::topology\"",
           "graphir.schedule @\"TiledGemm::sm90\"", "graphir.member_ref",
           "graphir.definition_ref", "graphir.message", "graphir.connection",
           "graphir.placement", "graphir.cfg.stage @\"TiledGemm::mma\"",
           "initializer.value = \"0.0\"", "capacity = \"1\"",
           "delivery = \"uniform<Block>\"", "placement = \"host::pinned\""}) {
    check(text.find(expected) != std::string::npos,
          std::string("missing emitted definition/field: ") + expected);
  }
  check(text == lower(source).print(), "composition output is nondeterministic");
  metreon::graphir::Module moved(std::move(graph));
  metreon::graphir::Module assigned("unused");
  assigned = std::move(moved);
  check(assigned.print() == text, "module moves lost composition references");
}

void checksScopedStages() {
  const std::string source = R"(
pipeline First() {
  stage a() -> f32 { return b(); }
  stage b() -> f32 { return 1.0; }
}
pipeline Second() {
  stage a() -> f32 { return b(); }
  stage b() -> f32 { return 2.0; }
}
)";
  auto graph = lower(source);
  check(graph.stageGraphs()[0].body()[0].name == "First::b" &&
            graph.stageGraphs()[2].body()[0].name == "Second::b" &&
            graph.stageGraphs()[0].body()[0].attributes.at("callee_kind") == "stage" &&
            graph.pipelineGraphs()[0].members[0].graph != graph.pipelineGraphs()[1].members[0].graph,
        "stage calls or resource membership escaped their pipeline scope");
  rejects(source + "procedure bad() -> f32 { return First::b(); }", "sema.stage_call_outside_pipeline");
  rejects("pipeline A() { stage a() -> f32 { return b(); } } pipeline B() { stage b() -> f32 { return 1.0; } }",
          "sema.unknown_operation");

  auto contextual = lower(R"(
context Gpu::Thread grants {poll} gpu;
procedure helper(@cx: gpu; x: f32) -> f32 !{poll} where gpu allows {poll} { return x; }
pipeline P() {
  stage compute(@cx: gpu; x: f32) -> f32 !{poll} where gpu allows {poll} {
    { return helper(@cx; x); }
  }
}
)");
  const auto &stage = contextual.stageGraphs()[0];
  check(stage.context() && !stage.parameters()[0].id &&
            stage.body()[0].body[0].context == stage.context() &&
            metreon::graphir::buildControlFlow(stage).blocks.size() == 2,
        "stage context evidence, effectful call, or nested CFG lost");
}

void checksReferenceValidation(const std::string &source) {
  auto graph = lower(source);
  for (const auto invalid : {
           metreon::graphir::DefinitionReference{metreon::graphir::DefinitionKind::Stage, 99},
           metreon::graphir::DefinitionReference{metreon::graphir::DefinitionKind::ProtocolState, 2}}) {
    bool rejected = false;
    try { graph.addResourceReference(0, invalid, {0, 1, 1}); }
    catch (const std::logic_error &) { rejected = true; }
    check(rejected, "unknown or wrong-kind definition target was accepted");
  }
  graph.setProtocolMembers(0, {{99, 0}});
  bool rejected = false;
  try { static_cast<void>(graph.print()); }
  catch (const std::logic_error &) { rejected = true; }
  check(rejected, "printer accepted a dangling resource member reference");
}

void checksForwardProtocolAndExistingResources(const std::string &source) {
  const auto split = source.find("pipeline TiledGemm(");
  check(split != std::string::npos, "pipeline fixture missing");
  auto graph = lower("resource Existing { state Ready(); }\n" +
                     source.substr(split) + source.substr(0, split));
  check(graph.resourceGraphs()[0].states()[0].name == "Ready" &&
            graph.protocolGraphs()[0].members[0].graph == 1 &&
            graph.pipelineGraphs()[0].members[0].graph == 2 &&
            graph.print().find("graphir.member_ref #rg2::%r7") != std::string::npos,
        "forward protocol resolution or existing-resource reference indices broke");
}

void checksParsedExample(const std::string &source) {
  const auto module = metreon::parser::Parser(source).parseModule();
  check(module.protocols.size() == 1 && module.pipelines.size() == 1,
        "protocol/pipeline declaration missing from AST");
  const auto &protocol = module.protocols.front();
  check(protocol.roles.size() == 2 && protocol.states.size() == 2 &&
            protocol.states[0].messages.size() == 2 && protocol.states[1].isFinal &&
            protocol.states[0].messages[0].payload[0].type.str() == "f32" &&
            protocol.states[0].messages[1].successor.name == "End",
        "protocol role/state/message payload lost");
  const auto &pipeline = module.pipelines.front();
  check(pipeline.parameters.size() == 3 && pipeline.states.size() == 1 &&
            pipeline.edges.size() == 2 && pipeline.stages.size() == 3 &&
            pipeline.topology->connections.size() == 7 &&
            pipeline.schedules[0].kernels[0].stages.size() == 3 &&
            pipeline.schedules[0].placements[2].first.value == "2" &&
            pipeline.stages[0].name.str() == "TiledGemm::load_a" &&
            pipeline.stages[0].body.statements.size() == 3 &&
            pipeline.edges[0].properties[4].str() == "uniform<Block>" &&
            pipeline.schedules[0].placements[0].location.line > pipeline.states[0].location.line,
        "structured pipeline fields or source locations lost");
}

void checksStageNameDiagnosticLocation() {
  try {
    static_cast<void>(metreon::parser::Parser(
        "pipeline P() {\n  stage nested::f() -> void {\n    return;\n  }\n  state x: u8 { initial 0; }\n}").parseModule());
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == "parse.qualified_stage_name" &&
              error.diagnostic().location.line == 2 && error.diagnostic().location.column == 9,
          "qualified-stage diagnostic points after the offending name");
    return;
  }
  throw std::runtime_error("qualified stage name was accepted");
}

void checksDiagnostics(const std::string &source) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {replace(source, "roles producer, consumer", "roles producer, producer"), "sema.duplicate_protocol_role"},
      {replace(source, "producer -> consumer: tile", "missing -> consumer: tile"), "sema.unknown_protocol_role"},
      {replace(source, "stop => End", "stop => Missing"), "sema.unknown_protocol_state"},
      {replace(source, "state End final;", "state Open final;"), "sema.duplicate_protocol_state"},
      {replace(source, "tile(value: f32)", "tile(value: f32, value: f32)"), "sema.duplicate_protocol_payload"},
      {replace(source, "epoch: u64 {", "epoch: Mystery {"), "sema.unknown_value_type"},
      {replace(source, "initial 0;", "initial true;"), "sema.type_mismatch"},
      {replace(source, "initial 0;", "initial 0; initial 1;"), "sema.duplicate_property"},
      {replace(source, "channel<TileFlow>", "channel<Unknown>"), "sema.unknown_channel_protocol"},
      {replace(source, "capacity 1;", "capacity 0;"), "sema.invalid_capacity"},
      {replace(source, "capacity 1;", "capacity 999999999999999999999999999;"), "sema.invalid_capacity"},
      {replace(source, "initial Open;", "initial Missing;"), "sema.unknown_protocol_state"},
      {replace(source, "load_a.src;", "load_a.missing;"), "sema.unknown_topology_endpoint"},
      {replace(source, "a          =>", "c          =>"), "sema.invalid_topology_direction"},
      {replace(source, "load_a.out =>[a_tiles]", "load_a.out =>[missing]"), "sema.unknown_pipeline_edge"},
      {replace(source, "a_in: TileFlow::consumer<Open>", "a_in: TileFlow::producer<Open>"), "sema.topology_type_mismatch"},
      {replace(source, "a_in: TileFlow::consumer<Open>", "a_in: TileFlow::consumer<End>"), "sema.topology_type_mismatch"},
      {replace(source, "src: f32,", "src: bool,"), "sema.topology_type_mismatch"},
      {replace(source, "a          => load_a.src;", "a => load_a.src; b => load_a.src;"), "sema.duplicate_topology_binding"},
      {replace(source, "fuse(load_a, load_b, mma)", "fuse(load_a, missing, mma)"), "sema.unknown_schedule_stage"},
      {replace(source, "fuse(load_a, load_b, mma)", "fuse(load_a, load_a, mma)"), "sema.duplicate_fused_stage"},
      {replace(source, "on worker.warp[0]", "on missing.warp[0]"), "sema.unknown_schedule_worker"},
      {replace(source, "fuse(load_a, load_b, mma)", "fuse(load_b, mma)"), "sema.stage_not_fused"},
      {replace(source, "blocks 1;", "blocks 0;"), "sema.invalid_blocks"},
      {replace(source, "f32 value = src;", "f32 value = missing;"), "sema.unknown_variable"},
      {replace(source, "f32 value = src;", "f32 value = true;"), "sema.type_mismatch"},
      {replace(source, "stage load_a(", "stage load_a::nested("), "parse.qualified_stage_name"},
  };
  for (const auto &[input, code] : cases) rejects(input, code);
  rejects("protocol P roles a, b { state S { a -> b: go => S; }", "parse.unterminated_protocol");
  rejects("pipeline P() {", "parse.unterminated_pipeline");
  rejects(replace(source, "worker.warp[0]", "worker.warp[-1]"), "parse.unexpected_token");
  rejects("pipeline P() { state x: u64 { initial 18446744073709551616; } }", "sema.invalid_state_initializer");
  rejects("pipeline P() { state x: u8 { initial 256; } }", "sema.invalid_state_initializer");
  rejects("pipeline P() { state x: i8 { initial -129; } }", "sema.invalid_state_initializer");
  rejects("pipeline P() { state x: i64 { initial 9223372036854775808; } }", "sema.invalid_state_initializer");
  static_cast<void>(lower("pipeline P() { state x: u64 { initial 18446744073709551615; } state y: i64 { initial -9223372036854775808; } }"));
}
} // namespace

int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected protocol/pipeline example path");
    std::ifstream input(argv[1]);
    check(static_cast<bool>(input), "cannot read example");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    checksParsedExample(source);
    checksStageNameDiagnosticLocation();
    checksDiagnostics(source);
    checksExample(source);
    checksScopedStages();
    checksReferenceValidation(source);
    checksForwardProtocolAndExistingResources(source);
    std::cout << "composition tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
