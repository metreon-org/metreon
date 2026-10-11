#include "metreon/Parser/Parser.h"
#include "metreon/Basic/Diagnostic.h"

#include <utility>

namespace metreon::parser {

bool Parser::consumeWord(const std::string &word) {
  if (current_.kind != lexer::TokenKind::Identifier || current_.text != word) return false;
  advance();
  return true;
}

lexer::Token Parser::expectWord(const std::string &word) {
  const auto token = current_;
  if (!consumeWord(word)) fail("parse.unexpected_token", "expected `" + word + "`");
  return token;
}

ast::ProtocolDeclaration Parser::parseProtocolDeclaration() {
  ast::ProtocolDeclaration declaration;
  declaration.location = expectWord("protocol").location;
  declaration.name = parseQualifiedName();
  expectWord("roles");
  do {
    const auto role = expect(lexer::TokenKind::Identifier, "a protocol role");
    declaration.roles.push_back({role.text, role.location});
  } while (consume(lexer::TokenKind::Comma));
  expect(lexer::TokenKind::LeftBrace, "`{` before protocol states");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile)
      fail("parse.unterminated_protocol", "expected `}` after protocol states");
    ast::ProtocolState state;
    state.location = expect(lexer::TokenKind::KeywordState, "`state`").location;
    state.name = expect(lexer::TokenKind::Identifier, "a protocol state name").text;
    state.isFinal = consumeWord("final");
    if (state.isFinal) {
      expect(lexer::TokenKind::Semicolon, "`;` after a final state");
    } else {
      expect(lexer::TokenKind::LeftBrace, "`{` before protocol messages");
      while (current_.kind != lexer::TokenKind::RightBrace) {
        if (current_.kind == lexer::TokenKind::EndOfFile)
          fail("parse.unterminated_protocol", "expected `}` after protocol messages");
        ast::ProtocolMessage message;
        const auto sender = expect(lexer::TokenKind::Identifier, "a sender role");
        message.sender = {sender.text, sender.location};
        message.location = sender.location;
        expect(lexer::TokenKind::Arrow, "`->` after sender role");
        const auto receiver = expect(lexer::TokenKind::Identifier, "a receiver role");
        message.receiver = {receiver.text, receiver.location};
        expect(lexer::TokenKind::Colon, "`:` before message name");
        message.name = expect(lexer::TokenKind::Identifier, "a message name").text;
        if (consume(lexer::TokenKind::LeftParen)) {
          message.payload = parseResourceFields();
          expect(lexer::TokenKind::RightParen, "`)` after message payload");
        }
        expect(lexer::TokenKind::FatArrow, "`=>` before successor state");
        const auto successor = expect(lexer::TokenKind::Identifier, "a successor state");
        message.successor = {successor.text, successor.location};
        expect(lexer::TokenKind::Semicolon, "`;` after protocol message");
        state.messages.push_back(std::move(message));
      }
      advance();
    }
    declaration.states.push_back(std::move(state));
  }
  advance();
  return declaration;
}

std::vector<ast::CompositionProperty> Parser::parseCompositionProperties() {
  std::vector<ast::CompositionProperty> properties;
  expect(lexer::TokenKind::LeftBrace, "`{` before properties");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile)
      fail("parse.unterminated_properties", "expected `}` after properties");
    const auto name = expect(lexer::TokenKind::Identifier, "a property name");
    ast::CompositionProperty property;
    property.name = name.text;
    property.location = name.location;
    if (current_.kind == lexer::TokenKind::IntegerLiteral ||
        current_.kind == lexer::TokenKind::FloatingLiteral ||
        current_.kind == lexer::TokenKind::BooleanLiteral ||
        current_.kind == lexer::TokenKind::Minus || current_.text == "infinity") {
      property.value = parseLiteralInitializer();
    } else {
      property.value = parseTypeReference();
    }
    expect(lexer::TokenKind::Semicolon, "`;` after property value");
    properties.push_back(std::move(property));
  }
  advance();
  return properties;
}

ast::TopologyEndpoint Parser::parseTopologyEndpoint() {
  const auto node = expect(lexer::TokenKind::Identifier, "a topology endpoint");
  ast::TopologyEndpoint endpoint{node.text, "", node.location};
  if (consume(lexer::TokenKind::Dot))
    endpoint.port = expect(lexer::TokenKind::Identifier, "a stage port").text;
  return endpoint;
}

ast::PipelineTopology Parser::parseTopology() {
  ast::PipelineTopology topology;
  topology.location = expectWord("topology").location;
  expect(lexer::TokenKind::LeftBrace, "`{` before topology connections");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile)
      fail("parse.unterminated_topology", "expected `}` after topology connections");
    ast::TopologyConnection connection;
    connection.source = parseTopologyEndpoint();
    connection.location = connection.source.location;
    expect(lexer::TokenKind::FatArrow, "`=>` in topology connection");
    if (consume(lexer::TokenKind::LeftBracket)) {
      const auto edge = expect(lexer::TokenKind::Identifier, "a channel edge name");
      connection.edge = ast::NamedReference{edge.text, edge.location};
      expect(lexer::TokenKind::RightBracket, "`]` after channel edge name");
    }
    connection.target = parseTopologyEndpoint();
    expect(lexer::TokenKind::Semicolon, "`;` after topology connection");
    topology.connections.push_back(std::move(connection));
  }
  advance();
  return topology;
}

ast::PipelineSchedule Parser::parseSchedule() {
  ast::PipelineSchedule schedule;
  schedule.location = expectWord("schedule").location;
  schedule.name = expect(lexer::TokenKind::Identifier, "a schedule target").text;
  expect(lexer::TokenKind::LeftBrace, "`{` before schedule members");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile)
      fail("parse.unterminated_schedule", "expected `}` after schedule members");
    if (current_.kind == lexer::TokenKind::KeywordKernel) {
      ast::ScheduleKernel worker;
      worker.location = current_.location;
      advance();
      worker.name = expect(lexer::TokenKind::Identifier, "a worker name").text;
      expect(lexer::TokenKind::Equal, "`=` before fused stages");
      expectWord("fuse");
      expect(lexer::TokenKind::LeftParen, "`(` before fused stages");
      if (current_.kind != lexer::TokenKind::RightParen) {
        do {
          const auto stage = expect(lexer::TokenKind::Identifier, "a fused stage");
          worker.stages.push_back({stage.text, stage.location});
        } while (consume(lexer::TokenKind::Comma));
      }
      expect(lexer::TokenKind::RightParen, "`)` after fused stages");
      worker.properties = parseCompositionProperties();
      schedule.kernels.push_back(std::move(worker));
    } else {
      ast::SchedulePlacement placement;
      placement.location = expectWord("place").location;
      const auto stage = expect(lexer::TokenKind::Identifier, "a placed stage");
      placement.stage = {stage.text, stage.location};
      expectWord("on");
      const auto worker = expect(lexer::TokenKind::Identifier, "a placement worker");
      placement.worker = {worker.text, worker.location};
      expect(lexer::TokenKind::Dot, "`.` before placement unit");
      placement.unit = expect(lexer::TokenKind::Identifier, "a placement unit").text;
      expect(lexer::TokenKind::LeftBracket, "`[` before placement index");
      const auto first = expect(lexer::TokenKind::IntegerLiteral, "a nonnegative placement index");
      placement.first = {ast::LiteralKind::Integer, first.text, first.location};
      if (consume(lexer::TokenKind::DotDot)) {
        const auto last = expect(lexer::TokenKind::IntegerLiteral, "a range end");
        placement.last = ast::LiteralInitializer{ast::LiteralKind::Integer, last.text, last.location};
      }
      expect(lexer::TokenKind::RightBracket, "`]` after placement index");
      expect(lexer::TokenKind::Semicolon, "`;` after placement");
      schedule.placements.push_back(std::move(placement));
    }
  }
  advance();
  return schedule;
}

ast::PipelineDeclaration Parser::parsePipelineDeclaration() {
  ast::PipelineDeclaration pipeline;
  pipeline.location = expectWord("pipeline").location;
  pipeline.name = parseQualifiedName();
  expect(lexer::TokenKind::LeftParen, "`(` before pipeline parameters");
  if (current_.kind != lexer::TokenKind::RightParen) {
    do {
      ast::PipelineParameter parameter;
      parameter.location = current_.location;
      if (consumeWord("input")) parameter.direction = ast::PipelineDirection::Input;
      else { expectWord("output"); parameter.direction = ast::PipelineDirection::Output; }
      parameter.name = expect(lexer::TokenKind::Identifier, "a pipeline parameter name").text;
      expect(lexer::TokenKind::Colon, "`:` before pipeline parameter type");
      parameter.type = parseValueType();
      pipeline.parameters.push_back(std::move(parameter));
    } while (consume(lexer::TokenKind::Comma));
  }
  expect(lexer::TokenKind::RightParen, "`)` after pipeline parameters");
  expect(lexer::TokenKind::LeftBrace, "`{` before pipeline members");
  while (current_.kind != lexer::TokenKind::RightBrace) {
    if (current_.kind == lexer::TokenKind::EndOfFile)
      fail("parse.unterminated_pipeline", "expected `}` after pipeline members");
    if (current_.kind == lexer::TokenKind::KeywordState) {
      ast::PipelineState state;
      state.location = current_.location;
      advance();
      state.name = expect(lexer::TokenKind::Identifier, "a pipeline state name").text;
      expect(lexer::TokenKind::Colon, "`:` before pipeline state type");
      state.type = parseValueType();
      state.properties = parseCompositionProperties();
      pipeline.states.push_back(std::move(state));
    } else if (current_.text == "edge") {
      ast::PipelineEdge edge;
      edge.location = expectWord("edge").location;
      edge.name = expect(lexer::TokenKind::Identifier, "a pipeline edge name").text;
      expect(lexer::TokenKind::Colon, "`:` before channel type");
      edge.type = parseTypeReference();
      edge.properties = parseCompositionProperties();
      pipeline.edges.push_back(std::move(edge));
    } else if (current_.text == "stage") {
      auto stage = parseCallableDeclaration(false);
      if (stage.name.components.size() != 1)
        throw DiagnosticError({"parse.qualified_stage_name",
                               "stage names must be local to their pipeline", stage.name.location});
      auto components = pipeline.name.components;
      components.insert(components.end(), stage.name.components.begin(), stage.name.components.end());
      stage.name.components = std::move(components);
      pipeline.stages.push_back(std::move(stage));
    } else if (current_.text == "topology") {
      if (pipeline.topology) fail("parse.duplicate_topology", "pipeline has more than one topology");
      pipeline.topology = parseTopology();
    } else if (current_.text == "schedule") {
      pipeline.schedules.push_back(parseSchedule());
    } else {
      fail("parse.expected_pipeline_member", "expected a state, edge, stage, topology, or schedule");
    }
  }
  advance();
  return pipeline;
}
} // namespace metreon::parser
