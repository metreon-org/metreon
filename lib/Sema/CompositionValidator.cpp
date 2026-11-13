#include "metreon/Sema/CompositionValidator.h"
#include "metreon/Sema/CallableValidator.h"
#include "metreon/Basic/Diagnostic.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace metreon::sema {
namespace {
[[noreturn]] void fail(const std::string &code, const std::string &message,
                       SourceLocation location) {
  throw DiagnosticError({"sema." + code, message, location});
}

using Properties = std::unordered_map<std::string, const ast::CompositionProperty *>;
Properties properties(const std::vector<ast::CompositionProperty> &input,
                      std::initializer_list<std::string> allowed) {
  Properties result;
  for (const auto &property : input) {
    if (std::find(allowed.begin(), allowed.end(), property.name) == allowed.end())
      fail("unknown_property", "unknown property `" + property.name + "`", property.location);
    if (!result.emplace(property.name, &property).second)
      fail("duplicate_property", "duplicate property `" + property.name + "`", property.location);
  }
  return result;
}

const ast::CompositionProperty &required(const Properties &props,
                                        const char *name, SourceLocation location) {
  const auto found = props.find(name);
  if (found == props.end()) fail("missing_property", "missing property `" + std::string(name) + "`", location);
  return *found->second;
}

std::string named(const ast::CompositionProperty &property) {
  if (!std::holds_alternative<ast::TypeReference>(property.value))
    fail("invalid_property", "property `" + property.name + "` requires a name or type", property.location);
  return property.str();
}

void choice(const ast::CompositionProperty &property,
            std::initializer_list<std::string> values) {
  const auto value = named(property);
  if (std::find(values.begin(), values.end(), value) == values.end())
    fail("invalid_property", "invalid `" + property.name + "` value `" + value + "`", property.location);
}

std::uint64_t integer(const ast::LiteralInitializer &literal, bool positive,
                      const std::string &code) {
  std::uint64_t result = 0;
  const auto [end, error] = std::from_chars(literal.value.data(),
                                         literal.value.data() + literal.value.size(), result);
  if (literal.kind != ast::LiteralKind::Integer || error != std::errc{} ||
      end != literal.value.data() + literal.value.size() || (positive && result == 0))
    fail(code, "expected " + std::string(positive ? "a positive" : "a nonnegative") + " integer", literal.location);
  return result;
}

void positiveProperty(const ast::CompositionProperty &property, const std::string &code) {
  const auto *literal = std::get_if<ast::LiteralInitializer>(&property.value);
  if (!literal) fail(code, "property requires a positive integer", property.location);
  integer(*literal, true, code);
}

void validateStateInteger(const ast::ValueType &type,
                          const ast::LiteralInitializer &literal) {
  if (literal.kind != ast::LiteralKind::Integer) return;
  const auto name = type.reference.name.str();
  // Index width belongs to target lowering; sized integers have a fixed range.
  if (name == "index") return;
  const unsigned bits = static_cast<unsigned>(std::stoul(name.substr(1)));
  if (name.front() == 'u') {
    const auto value = integer(literal, false, "invalid_state_initializer");
    if (bits < 64 && value > ((std::uint64_t{1} << bits) - 1))
      fail("invalid_state_initializer", "state initializer exceeds `" + name + "` range", literal.location);
  } else {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(literal.value.data(),
        literal.value.data() + literal.value.size(), value);
    if (error != std::errc{} || end != literal.value.data() + literal.value.size())
      fail("invalid_state_initializer", "state initializer exceeds `" + name + "` range", literal.location);
    const auto minimum = bits == 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t{1} << (bits - 1));
    const auto maximum = bits == 64 ? std::numeric_limits<std::int64_t>::max() : (std::int64_t{1} << (bits - 1)) - 1;
    if (value < minimum || value > maximum)
      fail("invalid_state_initializer", "state initializer exceeds `" + name + "` range", literal.location);
  }
}

using Protocols = std::unordered_map<std::string, const ast::ProtocolDeclaration *>;
void validateProtocol(const ast::Module &module, const ast::ProtocolDeclaration &protocol) {
  std::unordered_set<std::string> roles, states;
  for (const auto &role : protocol.roles) {
    if (!roles.insert(role.name).second)
      fail("duplicate_protocol_role", "duplicate role `" + role.name + "`", role.location);
  }
  if (roles.size() < 2) fail("invalid_protocol_roles", "a protocol requires at least two roles", protocol.location);
  for (const auto &state : protocol.states) {
    if (!states.insert(state.name).second)
      fail("duplicate_protocol_state", "duplicate protocol state `" + state.name + "`", state.location);
  }
  if (states.empty()) fail("empty_protocol", "a protocol requires a state", protocol.location);
  for (const auto &state : protocol.states) {
    if (state.isFinal && !state.messages.empty())
      fail("invalid_final_state", "final states cannot have messages", state.location);
    if (!state.isFinal && state.messages.empty())
      fail("empty_protocol_state", "a nonfinal state requires a message", state.location);
    std::unordered_set<std::string> messages;
    for (const auto &message : state.messages) {
      for (const auto *role : {&message.sender, &message.receiver}) {
        if (!roles.contains(role->name))
          fail("unknown_protocol_role", "unknown role `" + role->name + "`", role->location);
      }
      if (message.sender.name == message.receiver.name)
        fail("invalid_protocol_direction", "message roles must differ", message.location);
      if (!messages.insert(message.name).second)
        fail("duplicate_protocol_message", "duplicate message `" + message.name + "`", message.location);
      if (!states.contains(message.successor.name))
        fail("unknown_protocol_state", "unknown successor state `" + message.successor.name + "`", message.successor.location);
      std::unordered_set<std::string> fields;
      for (const auto &field : message.payload) {
        if (!fields.insert(field.name).second)
          fail("duplicate_protocol_payload", "duplicate payload field `" + field.name + "`", field.location);
        validateValueType(module, field.type);
      }
    }
  }
}

struct Endpoint {
  const ast::ValueType *type;
  enum class Kind { Input, Output, State, Port } kind;
};

void validatePipeline(const ast::Module &module, const ast::PipelineDeclaration &pipeline,
                      const Protocols &protocols) {
  std::unordered_set<std::string> members;
  auto add = [&](const std::string &name, SourceLocation location) {
    if (!members.insert(name).second)
      fail("duplicate_pipeline_member", "duplicate pipeline member `" + name + "`", location);
  };
  std::unordered_map<std::string, Endpoint> endpoints;
  std::unordered_map<std::string, const ast::CallableDeclaration *> stages;
  for (const auto &parameter : pipeline.parameters) {
    add(parameter.name, parameter.location);
    validateValueType(module, parameter.type);
    endpoints.emplace(parameter.name, Endpoint{&parameter.type,
        parameter.direction == ast::PipelineDirection::Input ? Endpoint::Kind::Input : Endpoint::Kind::Output});
  }
  for (const auto &state : pipeline.states) {
    add(state.name, state.location);
    validateValueType(module, state.type);
    const auto props = properties(state.properties, {"lifetime", "placement", "initial"});
    if (props.contains("lifetime")) choice(*props.at("lifetime"), {"pipeline", "stage"});
    if (props.contains("placement")) choice(*props.at("placement"), {"host::pinned", "gpu::global", "gpu::shared", "gpu::register"});
    const auto &initial = required(props, "initial", state.location);
    const auto *literal = std::get_if<ast::LiteralInitializer>(&initial.value);
    if (!literal) fail("invalid_state_initializer", "state initial value must be a literal", initial.location);
    validateLiteralType(state.type, *literal);
    validateStateInteger(state.type, *literal);
    endpoints.emplace(state.name, Endpoint{&state.type, Endpoint::Kind::State});
  }
  for (const auto &stage : pipeline.stages) {
    const auto name = stage.name.components.back();
    add(name, stage.location);
    stages.emplace(name, &stage);
    for (const auto &parameter : stage.parameters) {
      if (!parameter.isContextEvidence) {
        validateValueType(module, parameter.type);
        endpoints.emplace(name + "." + parameter.name, Endpoint{&parameter.type, Endpoint::Kind::Port});
      }
    }
    validateValueType(module, stage.resultType, true);
  }
  struct Channel {
    const ast::ProtocolDeclaration *protocol;
    std::string initial;
  };
  std::unordered_map<std::string, Channel> channels;
  for (const auto &edge : pipeline.edges) {
    add(edge.name, edge.location);
    if (edge.type.name.str() != "channel" || edge.type.arguments.size() != 1 ||
        !edge.type.arguments[0].arguments.empty())
      fail("invalid_channel_type", "edge requires channel<Protocol>", edge.location);
    const auto protocol = protocols.find(edge.type.arguments[0].str());
    if (protocol == protocols.end())
      fail("unknown_channel_protocol", "unknown channel protocol `" + edge.type.arguments[0].str() + "`", edge.location);
    const auto props = properties(edge.properties, {"initial", "capacity", "storage", "scope", "delivery", "backpressure"});
    const auto &initial = required(props, "initial", edge.location);
    const auto state = named(initial);
    const auto found = std::find_if(protocol->second->states.begin(), protocol->second->states.end(),
                                    [&](const auto &candidate) { return candidate.name == state; });
    if (found == protocol->second->states.end())
      fail("unknown_protocol_state", "unknown channel initial state `" + state + "`", initial.location);
    positiveProperty(required(props, "capacity", edge.location), "invalid_capacity");
    if (props.contains("storage")) choice(*props.at("storage"), {"gpu::shared", "gpu::global", "host::pinned"});
    if (props.contains("scope")) choice(*props.at("scope"), {"thread", "warp", "block", "cluster", "device"});
    if (props.contains("backpressure")) choice(*props.at("backpressure"), {"cooperative", "blocking"});
    if (props.contains("delivery")) {
      const auto &property = *props.at("delivery");
      named(property);
      const auto &type = std::get<ast::TypeReference>(property.value);
      if (type.name.str() != "uniform" || type.arguments.size() != 1 ||
          (type.arguments[0].str() != "Warp" && type.arguments[0].str() != "Block" && type.arguments[0].str() != "Cluster"))
        fail("invalid_property", "delivery requires uniform<Warp|Block|Cluster>", property.location);
    }
    channels.emplace(edge.name, Channel{protocol->second, state});
  }
  if (pipeline.topology) {
    add("topology", pipeline.topology->location);
    std::unordered_set<std::string> destinations;
    for (const auto &connection : pipeline.topology->connections) {
      auto resolve = [&](const ast::TopologyEndpoint &endpoint) -> Endpoint {
        const auto found = endpoints.find(endpoint.str());
        if (found == endpoints.end())
          fail("unknown_topology_endpoint", "unknown topology endpoint `" + endpoint.str() + "`", endpoint.location);
        return found->second;
      };
      const auto source = resolve(connection.source), target = resolve(connection.target);
      if (source.kind == Endpoint::Kind::Output || target.kind == Endpoint::Kind::Input ||
          target.kind == Endpoint::Kind::State ||
          (source.kind != Endpoint::Kind::Port && target.kind != Endpoint::Kind::Port))
        fail("invalid_topology_direction", "invalid topology source/target direction", connection.location);
      if (!destinations.insert(connection.target.str()).second)
        fail("duplicate_topology_binding", "destination is bound more than once", connection.target.location);
      if (!connection.edge) {
        if (source.type->str() != target.type->str())
          fail("topology_type_mismatch", "direct connection types differ", connection.location);
        // Protocol endpoints must be transported through a declared channel.
        if (source.type->reference.name.components.size() > 1 &&
            source.type->reference.arguments.size() == 1) {
          auto components = source.type->reference.name.components;
          components.pop_back();
          if (protocols.contains(ast::QualifiedName{components, connection.location}.str()))
            fail("topology_type_mismatch", "endpoint connection requires a channel edge", connection.location);
        }
        continue;
      }
      const auto channel = channels.find(connection.edge->name);
      if (channel == channels.end())
        fail("unknown_pipeline_edge", "unknown pipeline edge `" + connection.edge->name + "`", connection.edge->location);
      auto role = [&](const Endpoint &endpoint) {
        const auto &type = endpoint.type->reference;
        auto components = type.name.components;
        if (endpoint.kind != Endpoint::Kind::Port || components.size() < 2 ||
            type.arguments.size() != 1 || type.arguments[0].str() != channel->second.initial)
          fail("topology_type_mismatch", "channel endpoint state/type mismatch", connection.location);
        const auto name = components.back();
        components.pop_back();
        if (ast::QualifiedName{components, connection.location}.str() != channel->second.protocol->name.str())
          fail("topology_type_mismatch", "channel endpoint protocol mismatch", connection.location);
        return name;
      };
      const auto sender = role(source), receiver = role(target);
      const auto &protocol = *channel->second.protocol;
      const auto state = std::find_if(protocol.states.begin(), protocol.states.end(),
                                      [&](const auto &candidate) { return candidate.name == channel->second.initial; });
      if (sender == receiver || std::none_of(state->messages.begin(), state->messages.end(),
            [&](const auto &message) { return message.sender.name == sender && message.receiver.name == receiver; }))
        fail("topology_type_mismatch", "channel endpoint role direction mismatch", connection.location);
    }
  }
  std::unordered_set<std::string> schedules;
  for (const auto &schedule : pipeline.schedules) {
    if (!schedules.insert(schedule.name).second)
      fail("duplicate_schedule", "duplicate schedule `" + schedule.name + "`", schedule.location);
    add(schedule.name, schedule.location);
    std::unordered_map<std::string, std::unordered_set<std::string>> workers;
    std::unordered_set<std::string> allFused;
    for (const auto &worker : schedule.kernels) {
      if (!workers.emplace(worker.name, std::unordered_set<std::string>{}).second)
        fail("duplicate_schedule_worker", "duplicate schedule worker", worker.location);
      if (worker.stages.empty()) fail("empty_fusion", "worker requires a fused stage", worker.location);
      for (const auto &stage : worker.stages) {
        if (!stages.contains(stage.name)) fail("unknown_schedule_stage", "unknown fused stage `" + stage.name + "`", stage.location);
        if (!allFused.insert(stage.name).second)
          fail("duplicate_fused_stage", "stage is fused more than once", stage.location);
        workers.at(worker.name).insert(stage.name);
      }
      const auto props = properties(worker.properties, {"blocks", "residency"});
      positiveProperty(required(props, "blocks", worker.location), "invalid_blocks");
      if (props.contains("residency")) choice(*props.at("residency"), {"persistent", "transient"});
    }
    std::unordered_set<std::string> placements;
    for (const auto &placement : schedule.placements) {
      if (!stages.contains(placement.stage.name))
        fail("unknown_schedule_stage", "unknown placed stage", placement.stage.location);
      const auto worker = workers.find(placement.worker.name);
      if (worker == workers.end()) fail("unknown_schedule_worker", "unknown placement worker", placement.worker.location);
      if (!worker->second.contains(placement.stage.name))
        fail("stage_not_fused", "placed stage is not fused into this worker", placement.location);
      if (!placements.insert(placement.stage.name).second)
        fail("duplicate_stage_placement", "stage is placed more than once", placement.location);
      if (placement.unit != "warp" && placement.unit != "warps")
        fail("invalid_placement", "placement unit must be warp or warps", placement.location);
      const auto first = integer(placement.first, false, "invalid_placement");
      if (placement.last) {
        const auto last = integer(*placement.last, false, "invalid_placement");
        if (placement.unit != "warps" || last < first)
          fail("invalid_placement", "invalid warp range", placement.location);
      } else if (placement.unit == "warps") {
        fail("invalid_placement", "warps requires a range", placement.location);
      }
    }
  }
}
} // namespace

void validateCompositions(const ast::Module &module) {
  Protocols protocols;
  std::unordered_set<std::string> names;
  for (const auto &resource : module.resources) names.insert(resource.name.str());
  for (const auto &protocol : module.protocols) {
    if (!names.insert(protocol.name.str()).second)
      fail("duplicate_composition", "duplicate declaration `" + protocol.name.str() + "`", protocol.location);
    protocols.emplace(protocol.name.str(), &protocol);
  }
  for (const auto &protocol : module.protocols) validateProtocol(module, protocol);
  for (const auto &pipeline : module.pipelines) {
    if (!names.insert(pipeline.name.str()).second)
      fail("duplicate_composition", "duplicate declaration `" + pipeline.name.str() + "`", pipeline.location);
    validatePipeline(module, pipeline, protocols);
  }
}
} // namespace metreon::sema
