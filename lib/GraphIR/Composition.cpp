#include "metreon/GraphIR/Composition.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace metreon::graphir {

const char *definitionKindName(DefinitionKind kind) {
  switch (kind) {
  case DefinitionKind::Protocol: return "protocol";
  case DefinitionKind::ProtocolState: return "protocol_state";
  case DefinitionKind::PipelineState: return "pipeline_state";
  case DefinitionKind::PipelineEdge: return "pipeline_edge";
  case DefinitionKind::Stage: return "stage";
  case DefinitionKind::Topology: return "topology";
  case DefinitionKind::Schedule: return "schedule";
  }
  throw std::logic_error("unknown composition definition kind");
}

StageGraph &Module::addStageGraph(std::string name, SourceLocation location) {
  stageGraphs_.emplace_back(std::move(name), location);
  return stageGraphs_.back();
}

DefinitionReference Module::addCompositionDefinition(CompositionDefinition definition) {
  if (definition.kind == DefinitionKind::Protocol || definition.kind == DefinitionKind::Stage)
    throw std::logic_error("protocols and stages have dedicated GraphIR definitions");
  const DefinitionReference reference{definition.kind, compositionDefinitions_.size()};
  compositionDefinitions_.push_back(std::move(definition));
  return reference;
}

CompositionDefinition &Module::compositionDefinition(DefinitionReference reference) {
  if (reference.kind == DefinitionKind::Protocol || reference.kind == DefinitionKind::Stage ||
      reference.index >= compositionDefinitions_.size() ||
      compositionDefinitions_[reference.index].kind != reference.kind)
    throw std::logic_error("invalid GraphIR composition definition reference");
  return compositionDefinitions_[reference.index];
}

std::string Module::definitionName(DefinitionReference reference) const {
  if (reference.kind == DefinitionKind::Protocol) {
    if (reference.index < protocolGraphs_.size()) return protocolGraphs_[reference.index].name;
  } else if (reference.kind == DefinitionKind::Stage) {
    if (reference.index < stageGraphs_.size()) return stageGraphs_[reference.index].name();
  } else if (reference.index < compositionDefinitions_.size() &&
             compositionDefinitions_[reference.index].kind == reference.kind) {
    return compositionDefinitions_[reference.index].name;
  }
  throw std::logic_error("GraphIR definition reference has an unknown target");
}

void Module::eraseCompositionDefinitions(const std::vector<std::size_t> &indices) {
  if (indices.empty()) return;
  std::vector<bool> removed(compositionDefinitions_.size(), false);
  for (const auto index : indices) {
    if (index >= removed.size())
      throw std::logic_error("cannot erase an unknown GraphIR definition");
    removed[index] = true;
  }
  std::vector<std::optional<std::size_t>> remapping(removed.size());
  std::size_t next = 0;
  for (std::size_t index = 0; index < removed.size(); ++index)
    if (!removed[index]) remapping[index] = next++;
  const auto isRemoved = [&](DefinitionReference reference) {
    return reference.kind != DefinitionKind::Protocol &&
           reference.kind != DefinitionKind::Stage && removed.at(reference.index);
  };
  const auto remap = [&](DefinitionReference &reference) {
    if (reference.kind == DefinitionKind::Protocol || reference.kind == DefinitionKind::Stage)
      return;
    if (!remapping.at(reference.index))
      throw std::logic_error("live GraphIR definition references an erased target");
    reference.index = *remapping[reference.index];
  };
  for (std::size_t index = 0; index < compositionDefinitions_.size(); ++index) {
    if (removed[index]) continue;
    for (auto &record : compositionDefinitions_[index].records) {
      for (auto &reference : record.references) remap(reference);
      for (auto &region : record.contextRegions)
        for (auto &reference : region.stages) remap(reference);
    }
  }
  std::vector<std::vector<std::optional<std::size_t>>> memberRemapping;
  for (auto &graph : resourceGraphs_) {
    auto &references = graph.references();
    std::vector<std::optional<std::size_t>> members(references.size());
    std::vector<ResourceDefinitionReference> retained;
    for (auto reference : references) {
      if (isRemoved(reference.target)) continue;
      members.at(reference.id) = retained.size();
      reference.id = retained.size();
      remap(reference.target);
      retained.push_back(std::move(reference));
    }
    references = std::move(retained);
    memberRemapping.push_back(std::move(members));
  }
  const auto remapMembers = [&](std::vector<ResourceMemberReference> &members) {
    std::vector<ResourceMemberReference> retained;
    for (auto member : members) {
      const auto &replacement = memberRemapping.at(member.graph).at(member.member);
      if (!replacement) continue;
      member.member = *replacement;
      retained.push_back(member);
    }
    members = std::move(retained);
  };
  for (auto &protocol : protocolGraphs_) {
    remapMembers(protocol.members);
    std::erase_if(protocol.entryStates, isRemoved);
    for (auto &reference : protocol.entryStates) remap(reference);
  }
  for (auto &pipeline : pipelineGraphs_) remapMembers(pipeline.members);
  std::vector<CompositionDefinition> retained;
  for (std::size_t index = 0; index < compositionDefinitions_.size(); ++index)
    if (!removed[index]) retained.push_back(std::move(compositionDefinitions_[index]));
  compositionDefinitions_ = std::move(retained);
}

ResourceMemberReference Module::addResourceReference(
    std::size_t graph, DefinitionReference definition, SourceLocation location) {
  if (graph >= resourceGraphs_.size()) throw std::logic_error("unknown resource graph reference");
  static_cast<void>(definitionName(definition));
  return {graph, resourceGraphs_[graph].addReference(definition, location)};
}

ProtocolGraph &Module::addProtocolGraph(std::string name, SourceLocation location) {
  protocolGraphs_.push_back({std::move(name), {}, {}, location});
  return protocolGraphs_.back();
}

void Module::setProtocolMembers(std::size_t index, std::vector<ResourceMemberReference> members) {
  if (index >= protocolGraphs_.size()) throw std::logic_error("unknown GraphIR protocol");
  protocolGraphs_[index].members = std::move(members);
}

PipelineGraph &Module::addPipelineGraph(PipelineGraph group) {
  pipelineGraphs_.push_back(std::move(group));
  return pipelineGraphs_.back();
}

namespace {
using References = std::unordered_map<std::string, DefinitionReference>;

std::string qualified(const std::string &owner, const std::string &name) {
  return owner + "::" + name;
}

void addProperties(CompositionDefinition &definition,
                   const std::vector<ast::CompositionProperty> &properties) {
  for (const auto &property : properties) {
    definition.attributes.emplace(property.name, property.str());
    CompositionRecord record{"property", property.name, {{"value", property.str()}}, {}, property.location};
    if (const auto *literal = std::get_if<ast::LiteralInitializer>(&property.value)) {
      const char *kind = literal->kind == ast::LiteralKind::Integer ? "integer" :
                         literal->kind == ast::LiteralKind::Boolean ? "boolean" :
                         literal->kind == ast::LiteralKind::Infinity ? "infinity" : "floating";
      record.attributes.emplace("value.kind", kind);
    } else {
      record.attributes.emplace("value.kind", "type_reference");
    }
    definition.records.push_back(std::move(record));
  }
}

std::string escaped(const std::string &value) {
  std::string result;
  for (const char character : value) {
    switch (character) {
    case '\\': result += "\\\\"; break;
    case '"': result += "\\\""; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result += character;
    }
  }
  return result;
}
} // namespace

void lowerCompositions(const ast::Module &source, Module &module) {
  References definitions;
  std::unordered_map<std::string, std::size_t> protocolIndices;
  for (const auto &protocol : source.protocols) {
    const auto index = module.protocolGraphs().size();
    protocolIndices.emplace(protocol.name.str(), index);
    auto &group = module.addProtocolGraph(protocol.name.str(), protocol.location);
    for (const auto &role : protocol.roles) group.roles.push_back(role.name);
  }
  for (std::size_t index = 0; index < module.stageGraphs().size(); ++index)
    definitions.emplace(module.stageGraphs()[index].name(), DefinitionReference{DefinitionKind::Stage, index});

  // Declare all definitions first so messages and topology may refer forward.
  auto declare = [&](DefinitionKind kind, const std::string &name,
                     SourceLocation location) {
    const auto reference = module.addCompositionDefinition({kind, name, {}, {}, location});
    if (!definitions.emplace(name, reference).second)
      throw std::logic_error("duplicate validated composition symbol");
  };
  for (const auto &protocol : source.protocols) {
    for (const auto &state : protocol.states)
      declare(DefinitionKind::ProtocolState, qualified(protocol.name.str(), state.name), state.location);
  }
  for (const auto &pipeline : source.pipelines) {
    const auto owner = pipeline.name.str();
    for (const auto &state : pipeline.states)
      declare(DefinitionKind::PipelineState, qualified(owner, state.name), state.location);
    for (const auto &edge : pipeline.edges)
      declare(DefinitionKind::PipelineEdge, qualified(owner, edge.name), edge.location);
    if (pipeline.topology) declare(DefinitionKind::Topology, qualified(owner, "topology"), pipeline.topology->location);
    for (const auto &schedule : pipeline.schedules)
      declare(DefinitionKind::Schedule, qualified(owner, schedule.name), schedule.location);
  }
  for (std::size_t index = 0; index < source.protocols.size(); ++index) {
    const auto &protocol = source.protocols[index];
    const auto owner = protocol.name.str();
    const auto graphIndex = module.resourceGraphs().size();
    module.addResourceGraph(owner, {{"kind", "protocol"}}, protocol.location);
    std::vector<ResourceMemberReference> members;
    for (const auto &state : protocol.states) {
      const auto reference = definitions.at(qualified(owner, state.name));
      auto &definition = module.compositionDefinition(reference);
      definition.attributes.emplace("final", state.isFinal ? "true" : "false");
      for (const auto &message : state.messages) {
        CompositionRecord record{"message", message.name,
            {{"sender", message.sender.name}, {"receiver", message.receiver.name},
             {"successor", message.successor.name}, {"payload_count", std::to_string(message.payload.size())}},
            {definitions.at(qualified(owner, message.successor.name))}, message.location};
        for (std::size_t field = 0; field < message.payload.size(); ++field) {
          const auto prefix = "payload." + std::to_string(field) + ".";
          record.attributes.emplace(prefix + "name", message.payload[field].name);
          record.attributes.emplace(prefix + "type", message.payload[field].type.str());
        }
        definition.records.push_back(std::move(record));
      }
      members.push_back(module.addResourceReference(graphIndex, reference, state.location));
    }
    // Numeric references remain valid when the underlying vectors grow.
    // Group vectors were declared before references were constructed.
    module.setProtocolMembers(protocolIndices.at(owner), std::move(members));
  }
  for (const auto &pipeline : source.pipelines) {
    const auto owner = pipeline.name.str();
    const auto graphIndex = module.resourceGraphs().size();
    module.addResourceGraph(owner, {{"kind", "pipeline"}}, pipeline.location);
    PipelineGraph group{owner, {}, {}, pipeline.location};
    for (const auto &parameter : pipeline.parameters) {
      group.parameters.push_back({"pipeline_parameter", parameter.name,
          {{"type", parameter.type.str()},
           {"direction", parameter.direction == ast::PipelineDirection::Input ? "input" : "output"}},
          {}, parameter.location});
    }
    auto member = [&](const std::string &name, SourceLocation location) {
      const auto reference = definitions.at(qualified(owner, name));
      group.members.push_back(module.addResourceReference(graphIndex, reference, location));
      return reference;
    };
    for (const auto &state : pipeline.states) {
      auto &definition = module.compositionDefinition(member(state.name, state.location));
      definition.attributes.emplace("type", state.type.str());
      addProperties(definition, state.properties);
    }
    for (const auto &edge : pipeline.edges) {
      auto &definition = module.compositionDefinition(member(edge.name, edge.location));
      definition.attributes.emplace("type", edge.type.str());
      addProperties(definition, edge.properties);
      definition.records.push_back({"channel_protocol", edge.type.arguments.front().str(), {},
          {{DefinitionKind::Protocol, protocolIndices.at(edge.type.arguments.front().str())}}, edge.location});
    }
    for (const auto &stage : pipeline.stages)
      group.members.push_back(module.addResourceReference(graphIndex, definitions.at(stage.name.str()), stage.location));
    if (pipeline.topology) {
      auto &definition = module.compositionDefinition(member("topology", pipeline.topology->location));
      for (const auto &connection : pipeline.topology->connections) {
        CompositionRecord record{"connection", "",
            {{"source", qualified(owner, connection.source.str())},
             {"target", qualified(owner, connection.target.str())}}, {}, connection.location};
        for (const auto *endpoint : {&connection.source, &connection.target}) {
          const auto found = definitions.find(qualified(owner, endpoint->node));
          if (found != definitions.end()) record.references.push_back(found->second);
        }
        if (connection.edge) {
          record.attributes.emplace("edge", qualified(owner, connection.edge->name));
          record.references.push_back(definitions.at(qualified(owner, connection.edge->name)));
        }
        definition.records.push_back(std::move(record));
      }
    }
    for (const auto &schedule : pipeline.schedules) {
      auto &definition = module.compositionDefinition(member(schedule.name, schedule.location));
      definition.attributes.emplace("target", schedule.name);
      for (const auto &worker : schedule.kernels) {
        CompositionRecord record{"fused_kernel", worker.name, {}, {}, worker.location};
        for (const auto &property : worker.properties) record.attributes.emplace(property.name, property.str());
        for (const auto &stage : worker.stages) record.references.push_back(definitions.at(qualified(owner, stage.name)));
        definition.records.push_back(std::move(record));
      }
      for (const auto &placement : schedule.placements) {
        CompositionRecord record{"placement", placement.stage.name,
            {{"worker", placement.worker.name}, {"unit", placement.unit}, {"index", placement.first.value}},
            {definitions.at(qualified(owner, placement.stage.name))}, placement.location};
        if (placement.last) record.attributes.emplace("end", placement.last->value);
        definition.records.push_back(std::move(record));
      }
    }
    module.addPipelineGraph(std::move(group));
  }
}

std::string printCompositions(const Module &module) {
  std::ostringstream output;
  auto attributes = [&](const std::map<std::string, std::string> &values) {
    output << " {";
    bool first = true;
    for (const auto &[key, value] : values) {
      if (!first) output << ", ";
      first = false;
      output << key << " = \"" << escaped(value) << '"';
    }
    output << '}';
  };
  auto location = [&](SourceLocation loc) {
    output << " loc(\"" << escaped(module.sourceName()) << "\":" << loc.line << ':' << loc.column << ')';
  };
  auto references = [&](const std::vector<DefinitionReference> &values) {
    output << " refs(";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index) output << ", ";
      output << "@\"" << escaped(module.definitionName(values[index])) << '"';
    }
    output << ')';
  };
  auto record = [&](const CompositionRecord &value) {
    output << "    graphir." << value.kind;
    if (!value.name.empty()) output << " \"" << escaped(value.name) << '"';
    attributes(value.attributes);
    if (!value.references.empty()) references(value.references);
    location(value.location);
    if (value.contextRegions.empty()) {
      output << '\n';
      return;
    }
    output << " {\n";
    for (const auto &region : value.contextRegions) {
      output << "      graphir.context_region context(#ctx"
             << module.contextOrdinal(region.context) << ')';
      references(region.stages);
      location(region.location);
      output << '\n';
    }
    output << "    }\n";
  };
  for (const auto &definition : module.compositionDefinitions()) {
    output << "\n  graphir." << definitionKindName(definition.kind) << " @\"" << escaped(definition.name) << '"';
    attributes(definition.attributes);
    location(definition.location);
    output << " {\n";
    for (const auto &value : definition.records) record(value);
    output << "  }\n";
  }
  auto members = [&](const std::vector<ResourceMemberReference> &values) {
    for (const auto &value : values) {
      if (value.graph >= module.resourceGraphs().size() ||
          value.member >= module.resourceGraphs()[value.graph].references().size())
        throw std::logic_error("GraphIR composition member refers to an unknown resource reference");
      const auto &reference = module.resourceGraphs()[value.graph].references()[value.member];
      output << "    graphir.member_ref #rg" << value.graph << "::%r" << value.member;
      attributes({{"kind", definitionKindName(reference.target.kind)},
                  {"name", module.definitionName(reference.target)}});
      location(reference.location);
      output << '\n';
    }
  };
  for (const auto &protocol : module.protocolGraphs()) {
    output << "\n  graphir.protocol @\"" << escaped(protocol.name) << '"';
    location(protocol.location);
    output << " {\n";
    for (const auto &role : protocol.roles) output << "    graphir.role \"" << escaped(role) << "\"\n";
    members(protocol.members);
    output << "  }\n";
  }
  for (const auto &pipeline : module.pipelineGraphs()) {
    output << "\n  graphir.pipeline @\"" << escaped(pipeline.name) << '"';
    location(pipeline.location);
    output << " {\n";
    for (const auto &parameter : pipeline.parameters) record(parameter);
    members(pipeline.members);
    output << "  }\n";
  }
  return output.str();
}
} // namespace metreon::graphir
