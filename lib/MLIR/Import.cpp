#include "metreon/MLIR/Import.h"
#include "metreon/MLIR/Dialect.h"
#include "metreon/GraphIR/ControlFlow.h"
#include "metreon/Basic/Diagnostic.h"
#include "CanonicalSyntax.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;

namespace metreon {
namespace {
using Syntax = parser::CanonicalSyntax;
using Attributes = std::map<std::string, std::string>;

std::string required(const Attributes &attrs, const std::string &key) {
  auto found = attrs.find(key);
  if (found == attrs.end()) throw std::runtime_error("missing GraphIR attribute `" + key + "`");
  return found->second;
}

std::size_t count(const Attributes &attrs, const std::string &key) {
  auto text = required(attrs, key);
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
    throw std::runtime_error("invalid GraphIR count `" + key + "`");
  return std::stoull(text);
}

class Importer {
  const graphir::Module &source;
  MLIRContext &context;
  OpBuilder builder;
  OwningOpRef<ModuleOp> module;
  SourceLocation current;
  std::vector<SymbolRefAttr> contexts;
  std::set<std::string> contextTypeNames;
  std::map<std::string, Operation *> symbols;
  std::map<std::string, SymbolRefAttr> refs;
  std::map<std::string, const graphir::ResourceGraph *> resources;
  std::map<std::string, const graphir::ResourceStateNode *> states;
  std::map<std::string, std::string> stateOwners;
  std::map<std::string, const graphir::CallableGraph *> callables;
  std::map<std::string, std::string> callableKinds;
  std::map<std::string, const graphir::ResourceTransitionEdge *> transitions;
  std::set<std::string> localContextParameters;

  [[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
  Location location(SourceLocation loc) {
    return FileLineColLoc::get(&context, source.sourceName(), loc.line, loc.column);
  }
  DictionaryAttr dict(ArrayRef<NamedAttribute> values) { return builder.getDictionaryAttr(values); }
  NamedAttribute named(StringRef name, Attribute value) { return builder.getNamedAttr(name, value); }
  StringAttr str(StringRef value) { return builder.getStringAttr(value); }
  ArrayAttr array(ArrayRef<Attribute> values = {}) { return builder.getArrayAttr(values); }
  IntegerAttr integer(std::size_t value) { return builder.getI64IntegerAttr(value); }
  SymbolRefAttr reference(const std::string &name) {
    auto found = refs.find(name);
    if (found == refs.end()) fail("unknown GraphIR declaration `" + name + "`");
    return found->second;
  }
  SymbolRefAttr contextRef(const graphir::ContextMetadataRef &value) {
    source.requireContextUse(value, graphir::ContextUse::MetadataAttachment);
    return contexts.at(source.contextOrdinal(value));
  }
  static std::string leaf(const std::string &owner, const std::string &name) {
    const auto prefix = owner + "::";
    if (!name.starts_with(prefix)) throw std::runtime_error("member is outside its GraphIR owner");
    return name.substr(prefix.size());
  }
  void reserve(const std::string &name, const std::string &owner = {}) {
    if (refs.contains(name)) fail("duplicate declaration `" + name + "`");
    if (owner.empty()) refs.emplace(name, SymbolRefAttr::get(&context, name));
    else {
      auto parent = reference(owner);
      SmallVector<FlatSymbolRefAttr> nested(parent.getNestedReferences());
      nested.push_back(FlatSymbolRefAttr::get(&context, leaf(owner, name)));
      refs.emplace(name, SymbolRefAttr::get(parent.getRootReference(), nested));
    }
  }
  Operation *create(StringRef mnemonic, const std::string &name,
                    ArrayRef<NamedAttribute> attrs, bool group = false,
                    const std::string &owner = {}) {
    if (!owner.empty()) {
      auto found = symbols.find(owner);
      if (found == symbols.end() || !found->second->getNumRegions()) fail("missing declaration owner");
      builder.setInsertionPointToEnd(&found->second->getRegion(0).front());
    } else builder.setInsertionPointToEnd(module->getBody());
    OperationState state(location(current), ("metreon." + mnemonic).str());
    state.addAttributes(attrs);
    state.addAttribute("sym_name", str(owner.empty() ? name : leaf(owner, name)));
    if (group || mnemonic == "transition_decl") state.addRegion();
    auto *op = builder.create(state);
    if (group) op->getRegion(0).push_back(new Block);
    if (!symbols.emplace(name, op).second) fail("duplicate operation declaration");
    return op;
  }

  DictionaryAttr application(const ast::TypeReference &type) {
    SmallVector<Attribute> args;
    for (const auto &arg : type.arguments) args.push_back(application(arg));
    return dict({named("name", str(type.name.str())), named("arguments", array(args))});
  }
  ArrayAttr genericParameters(const std::vector<ast::ContextParameter> &params) {
    SmallVector<Attribute> result;
    std::set<std::string> names;
    for (const auto &param : params) {
      if (!names.insert(param.name).second) fail("duplicate generic parameter");
      result.push_back(dict({named("name", str(param.name)), named("constraint", application(param.constraint))}));
    }
    return array(result);
  }
  ArrayAttr canonicalApplications(const std::vector<ast::TypeReference> &values) {
    std::map<std::string, Attribute> sorted;
    for (const auto &value : values) {
      auto attr = application(value);
      std::string key;
      llvm::raw_string_ostream out(key);
      attr.print(out);
      if (!sorted.emplace(key, attr).second) fail("duplicate capability application");
    }
    SmallVector<Attribute> result;
    for (const auto &[key, attr] : sorted) { (void)key; result.push_back(attr); }
    return array(result);
  }
  ArrayAttr effects(const std::string &text) { return canonicalApplications(Syntax::effects(text)); }
  ArrayAttr requirements(const std::string &text) {
    SmallVector<Attribute> result;
    for (const auto &clause : Syntax::requirements(text)) {
      SmallVector<NamedAttribute> attrs{named("context_type", application(clause.context)),
          named("capabilities", canonicalApplications(clause.capabilities))};
      const auto found = refs.find(clause.context.name.str());
      if (found != refs.end()) attrs.push_back(named("context", found->second));
      result.push_back(dict(attrs));
    }
    return array(result);
  }
  DictionaryAttr properties(const Attributes &attrs) {
    SmallVector<NamedAttribute> values;
    for (const auto &[key, value] : attrs) {
      if (key == "type") {
        auto type = Syntax::valueType(value);
        SmallVector<NamedAttribute> typeAttrs(application(type.reference).getValue());
        typeAttrs.push_back(named("ownership", str(type.ownership == ast::OwnershipQualifier::Own ? "own" : "none")));
        values.push_back(named(key, dict(typeAttrs)));
      }
      else values.push_back(named(key, str(value)));
    }
    return dict(values);
  }
  Type valueType(const ast::ValueType &value, const std::string &owner = {}) {
    const auto name = value.reference.name.str();
    std::function<void(const ast::TypeReference &)> checkContext = [&](const auto &ref) {
      bool genericContext = localContextParameters.contains(ref.name.str());
      if (!owner.empty())
        for (const auto &param : Syntax::generics(required(resources.at(owner)->attributes(), "generic_parameters")))
          genericContext |= param.name == ref.name.str() && param.constraint.name.str() == "Context";
      if (genericContext || contextTypeNames.contains(ref.name.str()) || ref.name.str() == "Context")
        fail("context evidence cannot be a runtime type");
      for (const auto &arg : ref.arguments) checkContext(arg);
    };
    checkContext(value.reference);
    Type type;
    auto stateName = name;
    if (!owner.empty() && states.contains(owner + "::" + name)) stateName = owner + "::" + name;
    if (states.contains(stateName)) {
      SmallVector<Attribute> args;
      for (const auto &arg : value.reference.arguments) args.push_back(application(arg));
      if (args.empty() && !owner.empty()) {
        for (const auto &param : Syntax::generics(required(resources.at(owner)->attributes(), "generic_parameters")))
          args.push_back(dict({named("name", str(param.name)), named("arguments", array())}));
      }
      const auto &resource = *resources.at(stateOwners.at(stateName));
      if (args.size() != count(resource.attributes(), "generic_arity"))
        fail("resource state generic arity mismatch");
      type = ResourceType::get(&context, reference(stateName), array(args));
    } else if (name == "void") type = builder.getNoneType();
    else if (name == "bool") type = builder.getI1Type();
    else if (name == "index") type = builder.getIndexType();
    else if (name == "f16") type = builder.getF16Type();
    else if (name == "bf16") type = builder.getBF16Type();
    else if (name == "f32") type = builder.getF32Type();
    else if (name == "f64") type = builder.getF64Type();
    else if ((name.starts_with('i') || name.starts_with('u')) && name.size() > 1 &&
        std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      const auto width = std::stoul(name.substr(1));
      if (!width || width > 65536) fail("unsupported integer width");
      type = builder.getIntegerType(width);
    } else type = UnresolvedType::get(&context, application(value.reference));
    if (isa<IntegerType, FloatType, IndexType, NoneType>(type) && !value.reference.arguments.empty())
      fail("builtin scalar type cannot have generic arguments");
    if (value.ownership == ast::OwnershipQualifier::Own && !isa<ResourceType>(type))
      type = OwnedType::get(&context, type);
    return type;
  }
  DictionaryAttr parameter(const std::string &name, const ast::ValueType &value,
                           const std::string &owner = {}) {
    return dict({named("name", str(name)), named("type", TypeAttr::get(valueType(value, owner))),
        named("ownership", str(value.ownership == ast::OwnershipQualifier::Own ? "own" : "none")),
        named("source_type", application(value.reference))});
  }
  SymbolRefAttr definitionRef(graphir::DefinitionReference ref) {
    // definitionName validates both the index and the declared definition kind.
    const auto name = source.definitionName(ref);
    return reference(name);
  }
  SymbolRefAttr memberRef(graphir::ResourceMemberReference ref) {
    const auto &graph = source.resourceGraphs().at(ref.graph);
    const auto &member = graph.references().at(ref.member);
    if (member.id != ref.member) fail("inconsistent GraphIR member identity");
    return definitionRef(member.target);
  }

  void allocateSymbols() {
    for (const auto &metadata : source.contexts()) {
      source.requireContextUse(metadata, graphir::ContextUse::MetadataAttachment);
      const auto name = metadata->hasIdentifier() ? metadata->identifier()
          : "__context_" + std::to_string(contexts.size());
      reserve(name);
      contexts.push_back(reference(name));
      contextTypeNames.insert(Syntax::type(metadata->name()).name.str());
      contextTypeNames.insert(name);
    }
    std::function<void(const graphir::ResourceStateNode &, const graphir::ResourceGraph &)> state;
    state = [&](const auto &node, const auto &graph) {
      if (!node.members.empty()) {
        for (const auto &member : node.members) state(member, graph);
        return;
      }
      const auto name = graph.name() + "::" + node.name;
      reserve(name, graph.name());
      states.emplace(name, &node);
      stateOwners.emplace(name, graph.name());
    };
    for (const auto &graph : source.resourceGraphs()) {
      if (auto kind = graph.attributes().find("kind"); kind != graph.attributes().end()) {
        if (kind->second != "protocol" && kind->second != "pipeline") fail("unknown GraphIR resource graph kind");
        continue;
      }
      reserve(graph.name());
      resources.emplace(graph.name(), &graph);
      for (const auto &node : graph.states()) state(node, graph);
      const auto transition = [&](const auto &edge) {
        const auto name = graph.name() + "::" + required(edge.attributes, "name");
        reserve(name, graph.name());
        transitions.emplace(name, &edge);
      };
      std::function<void(const graphir::ResourceStateNode &)> internal = [&](const auto &node) {
        for (const auto &edge : node.internalTransitions) transition(edge);
        for (const auto &member : node.members) internal(member);
      };
      for (const auto &node : graph.states()) internal(node);
      for (const auto &edge : graph.transitions()) transition(edge);
    }
    for (const auto &protocol : source.protocolGraphs()) reserve(protocol.name);
    for (const auto &pipeline : source.pipelineGraphs()) reserve(pipeline.name);
    const auto callable = [&](const auto &graph, const std::string &owner, const std::string &kind) {
      reserve(graph.name(), owner);
      if (!callables.emplace(graph.name(), &graph).second) fail("duplicate callable");
      callableKinds.emplace(graph.name(), kind);
    };
    for (const auto &graph : source.procedureGraphs()) callable(graph, {}, "procedure");
    for (const auto &graph : source.kernelGraphs()) callable(graph, {}, "kernel");
    for (const auto &graph : source.stageGraphs()) {
      std::string owner;
      for (const auto &pipeline : source.pipelineGraphs())
        for (const auto &member : pipeline.members)
          if (source.definitionName(source.resourceGraphs().at(member.graph).references().at(member.member).target) == graph.name()) {
            if (!owner.empty()) fail("stage has multiple pipeline owners");
            owner = pipeline.name;
          }
      if (owner.empty()) fail("stage has no pipeline owner");
      callable(graph, owner, "stage");
    }
    for (const auto &definition : source.compositionDefinitions()) {
      std::string owner;
      for (const auto &protocol : source.protocolGraphs())
        if (definition.name.starts_with(protocol.name + "::")) owner = protocol.name;
      for (const auto &pipeline : source.pipelineGraphs())
        if (definition.name.starts_with(pipeline.name + "::")) owner = pipeline.name;
      if (owner.empty()) fail("composition declaration has no owner");
      reserve(definition.name, owner);
    }
  }

  void importContexts() {
    for (std::size_t index = 0; index < source.contexts().size(); ++index) {
      const auto &metadata = source.contexts()[index];
      current = metadata->location();
      SmallVector<Attribute> params;
      std::map<std::string, Attribute> grants;
      for (const auto &node : source.nodes()) {
        if (!node.context) fail("context node has no context identity");
        source.requireContextUse(node.context, graphir::ContextUse::MetadataAttachment);
        if (!node.context->hasSameIdentity(*metadata)) continue;
        if (node.kind == graphir::NodeKind::ContextVariable) {
          params.push_back(dict({named("name", str(node.name)),
              named("constraint", application(Syntax::type(required(node.attributes, "constraint"))))}));
          continue;
        }
        auto capability = Syntax::type(node.name);
        if (count(node.attributes, "arity") != capability.arguments.size() ||
            required(node.attributes, "capability") != capability.name.str()) fail("inconsistent grant application");
        SmallVector<NamedAttribute> attrs(application(capability).getValue());
        SmallVector<Attribute> targets;
        for (const auto &edge : source.edges()) {
          if (edge.source != node.id || edge.kind != graphir::EdgeKind::Transition) continue;
          if (auto *target = std::get_if<graphir::ContextMetadataRef>(&edge.target)) targets.push_back(contextRef(*target));
          else {
            const auto &targetNode = source.nodes().at(std::get<graphir::NodeId>(edge.target));
            if (targetNode.kind != graphir::NodeKind::ContextVariable || !targetNode.context->hasSameIdentity(*metadata))
              fail("invalid generic context transition target");
            targets.push_back(dict({named("parameter", str(targetNode.name))}));
          }
        }
        if (!targets.empty()) attrs.push_back(named("context_targets", array(targets)));
        auto grant = dict(attrs);
        std::string key;
        llvm::raw_string_ostream out(key);
        grant.print(out);
        if (!grants.emplace(key, grant).second) fail("duplicate grant");
      }
      auto nominal = Syntax::type(metadata->name());
      if (params.size() + nominal.arguments.size() != metadata->genericArity()) fail("context generic arity mismatch");
      SmallVector<Attribute> grantValues;
      for (const auto &[key, value] : grants) { (void)key; grantValues.push_back(value); }
      SmallVector<NamedAttribute> attrs{named("nominal_type", application(nominal)),
          named("generic_parameters", array(params)), named("grants", array(grantValues)),
          named("resolution", str(graphir::contextResolutionName(metadata->resolution())))};
      if (metadata->hasIdentifier()) attrs.push_back(named("identifier", str(metadata->identifier())));
      create("context", contexts[index].getRootReference().str(), attrs);
    }
    for (const auto &edge : source.edges()) {
      const auto &origin = source.nodes().at(edge.source);
      if (origin.kind != graphir::NodeKind::Grant) fail("context edge must originate from a grant");
      if (edge.kind != graphir::EdgeKind::Argument) continue;
      const auto &target = source.nodes().at(std::get<graphir::NodeId>(edge.target));
      if (target.kind != graphir::NodeKind::ContextVariable ||
          !target.context->hasSameIdentity(*origin.context)) fail("invalid argument edge context");
      auto type = Syntax::type(origin.name);
      auto path = required(edge.attributes, "path");
      std::size_t start = 0;
      while (start < path.size()) {
        auto end = path.find('.', start);
        auto component = path.substr(start, end - start);
        Attributes number{{"index", component}};
        const auto part = count(number, "index");
        type = type.arguments.at(part);
        if (end == std::string::npos) break;
        start = end + 1;
      }
      if (type.name.str() != target.name || !type.arguments.empty()) fail("argument edge path disagrees with grant type");
    }
  }

  void importResources() {
    for (const auto &graph : source.resourceGraphs()) {
      if (graph.attributes().contains("kind")) continue;
      current = graph.location();
      auto params = Syntax::generics(required(graph.attributes(), "generic_parameters"));
      if (params.size() != count(graph.attributes(), "generic_arity")) fail("resource generic arity mismatch");
      SmallVector<Attribute> entries;
      for (auto id : graph.entryStates()) {
        auto found = std::find_if(graph.states().begin(), graph.states().end(), [&](const auto &node) { return node.id == id; });
        if (found == graph.states().end()) fail("unknown resource entry state");
        const auto *node = &*found;
        while (!node->members.empty()) node = &node->members.front();
        entries.push_back(FlatSymbolRefAttr::get(&context, node->name));
      }
      create("resource", graph.name(), {named("generic_parameters", genericParameters(params)),
          named("entry_states", array(entries))}, true);
      std::function<void(const graphir::ResourceStateNode &)> state = [&](const auto &node) {
        if (!node.members.empty()) { for (const auto &member : node.members) state(member); return; }
        current = node.location;
        SmallVector<Attribute> fields;
        const auto fieldCount = count(node.attributes, "field_count");
        if (node.attributes.size() != 1 + 2 * fieldCount + node.attributes.count("accumulator"))
          fail("unknown resource-state attribute or inconsistent field count");
        for (std::size_t index = 0; index < fieldCount; ++index) {
          const auto prefix = "field." + std::to_string(index) + ".";
          fields.push_back(parameter(required(node.attributes, prefix + "name"),
              Syntax::valueType(required(node.attributes, prefix + "type")), graph.name()));
        }
        SmallVector<NamedAttribute> attrs{named("fields", array(fields)), named("lifecycle", str("unspecified"))};
        if (auto found = node.attributes.find("accumulator"); found != node.attributes.end())
          attrs.push_back(named("accumulator_scope", application(Syntax::type(found->second))));
        create("state", graph.name() + "::" + node.name, attrs, false, graph.name());
      };
      for (const auto &node : graph.states()) state(node);
      const auto transition = [&](const graphir::ResourceTransitionEdge &edge) {
        current = edge.location;
        const auto &attrs = edge.attributes;
        localContextParameters.clear();
        for (const auto &param : Syntax::generics(required(attrs, "generic_parameters")))
          if (param.constraint.name.str() == "Context") localContextParameters.insert(param.name);
        const auto name = required(attrs, "name");
        auto params = Syntax::parameters(required(attrs, "parameters"));
        auto evidence = Syntax::parameters(required(attrs, "context_evidence"));
        auto sourceType = Syntax::valueType(required(attrs, "source_type"));
        auto targetType = Syntax::valueType(required(attrs, "target_type"));
        const auto sourceName = sourceType.reference.name.str();
        const auto targetName = targetType.reference.name.str();
        if (!states.contains(graph.name() + "::" + sourceName) || !states.contains(graph.name() + "::" + targetName))
          fail("transition refers to unknown logical state");
        // Validate representative membership, rather than assuming collapsed
        // external edges enter/exit the container head.
        const auto endpointContains = [&](graphir::ResourceNodeId id, const std::string &logical) {
          std::function<bool(const graphir::ResourceStateNode &)> contains = [&](const auto &node) {
            if (node.members.empty()) return node.name == logical;
            return std::any_of(node.members.begin(), node.members.end(), contains);
          };
          for (const auto &node : graph.states()) {
            if (node.id == id && contains(node)) return true;
            std::function<bool(const graphir::ResourceStateNode &)> original = [&](const auto &member) {
              if (member.id == id && contains(member)) return true;
              return std::any_of(member.members.begin(), member.members.end(), original);
            };
            if (original(node)) return true;
          }
          return false;
        };
        if (!endpointContains(edge.source, sourceName) || !endpointContains(edge.target, targetName))
          fail("transition endpoint container disagrees with its signature");
        SmallVector<Type> inputs;
        SmallVector<Attribute> paramAttrs, contextRequirements;
        std::optional<std::size_t> self;
        std::size_t evidenceIndex = 0;
        for (const auto &param : params) {
          if (param.isContextEvidence) {
            if (evidenceIndex >= evidence.size() || evidence[evidenceIndex].str() != param.str())
              fail("inconsistent context evidence parameter list");
            ++evidenceIndex;
            contextRequirements.push_back(dict({named("name", str(param.name)),
                named("context_type", application(param.type.reference))}));
            continue;
          }
          if (param.name == "self") {
            if (self) fail("duplicate self parameter");
            if (param.type.str() != sourceType.str()) fail("self disagrees with source_type");
            self = inputs.size();
          }
          inputs.push_back(valueType(param.type, graph.name()));
          paramAttrs.push_back(parameter(param.name, param.type, graph.name()));
        }
        if (!self || evidenceIndex != evidence.size()) fail("invalid transition self/evidence parameters");
        auto where = requirements(required(attrs, "where"));
        contextRequirements.append(where.begin(), where.end());
        targetType.ownership = ast::OwnershipQualifier::Own;
        const auto resultType = valueType(targetType, graph.name());
        const auto awaitText = required(attrs, "await");
        if (awaitText != "true" && awaitText != "false") fail("invalid await flag");
        auto *declaration = create("transition_decl", graph.name() + "::" + name, {
            named("source_state", FlatSymbolRefAttr::get(&context, sourceName)),
            named("target_state", FlatSymbolRefAttr::get(&context, targetName)),
            named("function_type", TypeAttr::get(builder.getFunctionType(inputs, {resultType}))),
            named("self_index", integer(*self)), named("parameter_attrs", array(paramAttrs)),
            named("result_attrs", array({parameter("", targetType, graph.name())})),
            named("generic_parameters", genericParameters(Syntax::generics(required(attrs, "generic_parameters")))),
            named("context_requirements", array(contextRequirements)), named("effects", effects(required(attrs, "effects"))),
            named("is_await", builder.getBoolAttr(awaitText == "true"))}, false, graph.name());
        if (auto condition = attrs.find("await_condition"); condition != attrs.end())
          declaration->setAttr("metreon.source_await_condition", str(condition->second));
        localContextParameters.clear();
      };
      std::function<void(const graphir::ResourceStateNode &)> internal = [&](const auto &node) {
        for (const auto &edge : node.internalTransitions) transition(edge);
        for (const auto &member : node.members) internal(member);
      };
      for (const auto &node : graph.states()) internal(node);
      for (const auto &edge : graph.transitions()) transition(edge);
    }
    for (const auto &item : source.resourceContextTemplates()) {
      if (!resources.contains(item.resourceName())) fail("context template refers to missing resource");
      // The authoritative constraints are on transition declarations. Cached
      // hypothetical grant checks are not imported as verification certificates.
      for (const auto &specialization : item.specializations()) (void)contextRef(specialization.context);
    }
  }

  ArrayAttr records(const std::vector<graphir::CompositionRecord> &values) {
    SmallVector<Attribute> result;
    for (const auto &record : values) {
      SmallVector<Attribute> targets, regions;
      for (auto ref : record.references) targets.push_back(definitionRef(ref));
      for (const auto &region : record.contextRegions) {
        SmallVector<Attribute> stages;
        for (auto ref : region.stages) {
          if (ref.kind != graphir::DefinitionKind::Stage) fail("worker context region contains a non-stage");
          stages.push_back(definitionRef(ref));
        }
        regions.push_back(dict({named("context", contextRef(region.context)), named("stages", array(stages))}));
      }
      result.push_back(dict({named("kind", str(record.kind)), named("name", str(record.name)),
          named("properties", properties(record.attributes)), named("references", array(targets)),
          named("context_regions", array(regions)), named("location", location(record.location))}));
    }
    return array(result);
  }
  void importCompositions() {
    for (const auto &protocol : source.protocolGraphs()) {
      current = protocol.location;
      SmallVector<Attribute> roles, entries, members;
      for (const auto &role : protocol.roles) roles.push_back(str(role));
      for (auto ref : protocol.entryStates) entries.push_back(definitionRef(ref));
      for (auto ref : protocol.members) members.push_back(memberRef(ref));
      create("protocol", protocol.name, {named("semantic_properties", dict({named("roles", array(roles)),
          named("members", array(members))})), named("records", array()), named("entry_states", array(entries))}, true);
    }
    for (const auto &pipeline : source.pipelineGraphs()) {
      current = pipeline.location;
      SmallVector<Attribute> members;
      for (auto ref : pipeline.members) members.push_back(memberRef(ref));
      create("pipeline", pipeline.name, {named("semantic_properties", dict({named("members", array(members))})),
          named("records", records(pipeline.parameters)), named("entry_states", array())}, true);
    }
    for (const auto &definition : source.compositionDefinitions()) {
      current = definition.location;
      auto ref = reference(definition.name);
      const auto owner = ref.getRootReference().str();
      const bool isState = definition.kind == graphir::DefinitionKind::ProtocolState;
      SmallVector<NamedAttribute> attrs{named("semantic_properties", properties(definition.attributes)),
          named("records", isState ? array() : records(definition.records))};
      if (isState) attrs.push_back(named("entry_states", array()));
      const auto mnemonic = definition.kind == graphir::DefinitionKind::PipelineEdge
          ? "edge" : graphir::definitionKindName(definition.kind);
      create(mnemonic, definition.name, attrs, isState, owner);
      if (!isState) continue;
      for (const auto &message : definition.records) {
        current = message.location;
        if (message.kind != "message") fail("unexpected protocol-state record");
        SmallVector<Attribute> fields;
        for (std::size_t index = 0; index < count(message.attributes, "payload_count"); ++index) {
          const auto prefix = "payload." + std::to_string(index) + ".";
          fields.push_back(parameter(required(message.attributes, prefix + "name"),
              Syntax::valueType(required(message.attributes, prefix + "type"))));
        }
        const auto name = definition.name + "::" + message.name;
        reserve(name, definition.name);
        create("message_decl", name, {named("semantic_properties", dict({
            named("sender", str(required(message.attributes, "sender"))),
            named("receiver", str(required(message.attributes, "receiver"))),
            named("payload", array(fields))})), named("records", records({message}))}, false, definition.name);
      }
    }
  }

  void importCallable(const graphir::CallableGraph &graph, StringRef kind) {
    current = graph.location();
    SmallVector<Type> inputs, results;
    SmallVector<Attribute> params, resultAttrs;
    SmallVector<Attribute> contextRequirements;
    std::map<graphir::CallableValueId, const graphir::CallableOperation *> operations;
    std::map<graphir::CallableValueId, Type> valueTypes;
    std::map<graphir::CallableValueId, std::vector<graphir::CallableValueId>> valueScopes;
    for (const auto &param : graph.parameters()) {
      if (param.context) {
        if (param.id) fail("context parameter has a runtime value ID");
        (void)contextRef(param.context);
        if (!graph.context() || !graph.context()->hasSameIdentity(*param.context)) fail("inconsistent callable context");
        contextRequirements.push_back(dict({named("name", str(param.name)),
            named("context", contextRef(param.context)),
            named("context_type", application(Syntax::valueType(param.type).reference))}));
        continue;
      }
      if (!param.id) fail("runtime parameter has no value ID");
      auto sourceType = Syntax::valueType(param.type);
      auto type = valueType(sourceType);
      if (!valueTypes.emplace(*param.id, type).second) fail("duplicate callable value ID");
      valueScopes.emplace(*param.id, std::vector<graphir::CallableValueId>{});
      inputs.push_back(type);
      auto metadata = parameter(param.name, sourceType);
      SmallVector<NamedAttribute> paramValues(metadata.getValue());
      paramValues.push_back(named("graphir_id", integer(*param.id)));
      params.push_back(dict(paramValues));
    }
    auto resultSource = Syntax::valueType(required(graph.attributes(), "return_type"));
    if (resultSource.reference.name.str() != "void") {
      results.push_back(valueType(resultSource));
      resultAttrs.push_back(parameter("", resultSource));
    }
    std::function<ArrayAttr(const std::vector<graphir::CallableOperation> &, graphir::ContextMetadataRef)> importBody;
    importBody = [&](const auto &ops, graphir::ContextMetadataRef ambient) {
      SmallVector<Attribute> output;
      for (const auto &op : ops) {
        current = op.location;
        if (!operations.emplace(op.id, &op).second || valueTypes.contains(op.id)) fail("duplicate callable operation ID");
        auto effective = op.context ? op.context : ambient;
        if (op.context) (void)contextRef(op.context);
        if (op.kind == graphir::CallableOperationKind::ContextRegion && !op.context) fail("context region has no context");
        SmallVector<Attribute> operands;
        for (auto operand : op.operands) operands.push_back(integer(operand));
        SmallVector<NamedAttribute> attrs{named("id", integer(op.id)), named("name", str(op.name)),
            named("properties", properties(op.attributes)), named("operands", array(operands)),
            named("location", location(op.location))};
        const char *opKind = nullptr;
        switch (op.kind) {
        case graphir::CallableOperationKind::Literal: opKind = "literal"; break;
        case graphir::CallableOperationKind::Variable: opKind = "variable"; break;
        case graphir::CallableOperationKind::Call: {
          opKind = "call";
          const auto calleeKind = required(op.attributes, "callee_kind");
          if (calleeKind != "procedure" && calleeKind != "stage" && calleeKind != "transition") fail("unsupported call kind");
          attrs.push_back(named("callee", reference(op.name)));
          if (calleeKind == "transition" && !transitions.contains(op.name)) fail("transition call has wrong callee kind");
          if (calleeKind != "transition" && !callables.contains(op.name)) fail("call has wrong callee kind");
          if (calleeKind != "transition" && callableKinds.at(op.name) != calleeKind)
            fail("call kind disagrees with declaration");
          const auto &declAttrs = calleeKind == "transition" ? transitions.at(op.name)->attributes
              : callables.at(op.name)->attributes();
          if (effects(required(op.attributes, "effects")) != effects(required(declAttrs, "effects")))
            fail("call effects disagree with declaration");
          const auto declaredEffects = Syntax::effects(required(graph.attributes(), "effects"));
          for (const auto &effect : Syntax::effects(required(op.attributes, "effects")))
            if (std::none_of(declaredEffects.begin(), declaredEffects.end(), [&](const auto &declared) {
                  return declared.str() == effect.str();
                })) fail("call effect is not declared by enclosing callable");
          if (calleeKind != "transition" && callables.at(op.name)->context()) {
            auto target = callables.at(op.name)->context();
            if (!effective || !target->hasSameIdentity(*effective)) fail("call requires a different nominal execution context");
          }
          if (effective) {
            for (const auto &effect : Syntax::effects(required(op.attributes, "effects"))) {
              const bool allowed = std::any_of(source.nodes().begin(), source.nodes().end(), [&](const auto &node) {
                return node.kind == graphir::NodeKind::Grant && node.context &&
                    node.context->hasSameIdentity(*effective) && node.name == effect.str();
              });
              if (!allowed) fail("effective context lacks capability `" + effect.str() + "`");
            }
          } else if (!required(op.attributes, "effects").empty()) fail("effectful call has no context evidence");
          break;
        }
        case graphir::CallableOperationKind::Block: opKind = "block"; break;
        case graphir::CallableOperationKind::Return: opKind = "return"; break;
        case graphir::CallableOperationKind::ContextRegion: opKind = "context_region"; break;
        default: opKind = graphir::callableOperationKindName(op.kind); break;
        }
        if (!opKind) fail("unknown callable operation kind");
        attrs.push_back(named("kind", str(opKind)));
        if (effective) attrs.push_back(named("context", contextRef(effective)));
        if (!op.body.empty()) attrs.push_back(named("body", importBody(op.body, effective)));
        if (op.loopContext) attrs.push_back(named("loop_context", contextRef(op.loopContext)));
        if (!op.condition.empty()) attrs.push_back(named("condition", importBody(op.condition, effective)));
        if (!op.alternative.empty()) attrs.push_back(named("alternative", importBody(op.alternative, effective)));
        if (!op.initialization.empty()) attrs.push_back(named("initialization", importBody(op.initialization, effective)));
        if (!op.step.empty()) attrs.push_back(named("step", importBody(op.step, effective)));
        output.push_back(dict(attrs));
      }
      return array(output);
    };
    const auto deferredBody = importBody(graph.body(), graph.context());
    const auto cfg = graphir::buildControlFlow(graph);
    // Mutable scalar bindings are abstract local slots, not SSA values that
    // magically change on a backedge. Compute must-availability and definite
    // initialization over the CFG before checking individual operations.
    using Values = std::set<graphir::CallableValueId>;
    Values parameters, universe, slots;
    for (const auto &[id, type] : valueTypes) { (void)type; parameters.insert(id); universe.insert(id); }
    const auto isSlot = [](const graphir::CallableOperation &op) {
      auto found = op.attributes.find("mutable_local");
      return op.kind == graphir::CallableOperationKind::Variable &&
          found != op.attributes.end() && found->second == "true";
    };
    const auto defines = [&](const graphir::CallableOperation &op) {
      return required(op.attributes, "type") != "void" &&
          (op.kind != graphir::CallableOperationKind::Variable || isSlot(op) ||
           required(op.attributes, "initialized") == "true");
    };
    std::map<graphir::ControlFlowBlockId, std::vector<graphir::ControlFlowBlockId>> predecessors;
    for (const auto &block : cfg.blocks) {
      for (auto id : block.operations) {
        const auto &op = *operations.at(id);
        auto type = valueType(Syntax::valueType(required(op.attributes, "type")));
        if (!isa<NoneType>(type)) {
          valueTypes.emplace(id, type);
          valueScopes.emplace(id, block.scopes);
          if (defines(op)) universe.insert(id);
        }
        if (isSlot(op)) slots.insert(id);
      }
      if (block.terminator.successor) predecessors[*block.terminator.successor].push_back(block.id);
      if (block.terminator.falseSuccessor) predecessors[*block.terminator.falseSuccessor].push_back(block.id);
    }
    using Facts = std::pair<Values, Values>;
    std::map<graphir::ControlFlowBlockId, Facts> in, out;
    for (const auto &block : cfg.blocks) out[block.id] = {universe, slots};
    bool changed;
    do {
      changed = false;
      for (const auto &block : cfg.blocks) {
        Facts facts{parameters, {}};
        if (block.id != cfg.entry) {
          facts = {universe, slots};
          for (auto predecessor : predecessors[block.id]) {
            std::erase_if(facts.first, [&](auto id) { return !out.at(predecessor).first.contains(id); });
            std::erase_if(facts.second, [&](auto id) { return !out.at(predecessor).second.contains(id); });
          }
        }
        in[block.id] = facts;
        for (auto id : block.operations) {
          const auto &op = *operations.at(id);
          if (defines(op)) facts.first.insert(id);
          if (isSlot(op)) {
            if (required(op.attributes, "initialized") == "true") facts.second.insert(id);
            else facts.second.erase(id);
          }
          if (op.kind == graphir::CallableOperationKind::Assign && !op.operands.empty())
            facts.second.insert(op.operands.front());
        }
        if (out[block.id] != facts) { out[block.id] = std::move(facts); changed = true; }
      }
    } while (changed);
    SmallVector<Attribute> cfgBlocks;
    std::set<graphir::ControlFlowBlockId> blockIds;
    for (const auto &block : cfg.blocks) blockIds.insert(block.id);
    for (const auto &block : cfg.blocks) {
      current = block.location;
      auto available = in.at(block.id).first;
      auto initialized = in.at(block.id).second;
      SmallVector<Attribute> ids, scopes, returns;
      for (auto id : block.scopes) scopes.push_back(integer(id));
      const auto operand = [&](graphir::CallableValueId id) {
        if (!valueTypes.contains(id) || !available.contains(id)) fail("read of undefined or uninitialized callable value");
        const auto &scope = valueScopes.at(id);
        if (scope.size() > block.scopes.size() || !std::equal(scope.begin(), scope.end(), block.scopes.begin()))
          fail("callable value escapes its lexical scope");
      };
      for (auto id : block.operations) {
        auto found = operations.find(id);
        if (found == operations.end()) fail("CFG references an unknown operation");
        const auto &op = *found->second;
        current = op.location;
        for (auto arg : op.operands) operand(arg);
        if (op.kind == graphir::CallableOperationKind::Load) {
          if (op.operands.size() != 1 || !slots.contains(op.operands[0])) fail("invalid local load");
          if (!initialized.contains(op.operands[0])) fail("read of uninitialized mutable local");
        }
        if (op.kind == graphir::CallableOperationKind::Assign) {
          if (op.operands.size() != 2 || !slots.contains(op.operands[0]) ||
              valueTypes.at(op.operands[0]) != valueTypes.at(op.operands[1])) fail("invalid local assignment");
          initialized.insert(op.operands[0]);
        }
        if (op.kind == graphir::CallableOperationKind::Variable) {
          const auto initialized = required(op.attributes, "initialized");
          if (initialized != "true" && initialized != "false") fail("invalid initialized flag");
          if (op.operands.size() > 1 || (!op.operands.empty() && op.attributes.contains("initializer.kind")))
            fail("invalid variable initializer representation");
          if (initialized == "true" && op.operands.empty() &&
              (!op.attributes.contains("initializer.kind") || !op.attributes.contains("initializer.value")))
            fail("initialized variable has no initializer");
          if (!op.operands.empty() && valueTypes.at(op.operands.front()) != valueType(Syntax::valueType(required(op.attributes, "type"))))
            fail("variable initializer type mismatch");
        }
        if (op.kind == graphir::CallableOperationKind::Call) {
          const auto calleeKind = required(op.attributes, "callee_kind");
          SmallVector<Type> expectedInputs, expectedResults;
          if (calleeKind != "transition") {
            const auto &target = *callables.at(op.name);
            for (const auto &param : target.parameters())
              if (!param.context) expectedInputs.push_back(valueType(Syntax::valueType(param.type)));
            auto type = valueType(Syntax::valueType(required(target.attributes(), "return_type")));
            if (!isa<NoneType>(type)) expectedResults.push_back(type);
          } else {
            auto declaration = cast<TransitionDeclOp>(symbols.at(op.name));
            auto signature = declaration.getFunctionType();
            if (op.operands.size() != signature.getNumInputs()) fail("transition argument count mismatch");
            auto actualSelf = dyn_cast<ResourceType>(valueTypes.at(op.operands[declaration.getSelfIndex()]));
            auto declaredSelf = cast<ResourceType>(signature.getInput(declaration.getSelfIndex()));
            if (!actualSelf || actualSelf.getState() != declaredSelf.getState()) fail("transition source resource mismatch");
            std::map<std::string, DictionaryAttr> substitutions;
            for (auto [formal, actual] : llvm::zip_equal(declaredSelf.getArguments(), actualSelf.getArguments())) {
              auto formalType = cast<DictionaryAttr>(formal);
              if (!formalType.getAs<ArrayAttr>("arguments").empty()) fail("unsupported resource substitution pattern");
              substitutions.emplace(formalType.getAs<StringAttr>("name").str(), cast<DictionaryAttr>(actual));
            }
            std::function<DictionaryAttr(DictionaryAttr)> substitute = [&](DictionaryAttr type) {
              const auto name = type.getAs<StringAttr>("name").str();
              if (auto replacement = substitutions.find(name); replacement != substitutions.end()) return replacement->second;
              SmallVector<Attribute> args;
              for (auto arg : type.getAs<ArrayAttr>("arguments")) args.push_back(substitute(cast<DictionaryAttr>(arg)));
              return dict({named("name", str(name)), named("arguments", array(args))});
            };
            std::function<Type(Type)> instantiate = [&](Type type) -> Type {
              if (auto resource = dyn_cast<ResourceType>(type)) {
                SmallVector<Attribute> args;
                for (auto arg : resource.getArguments()) args.push_back(substitute(cast<DictionaryAttr>(arg)));
                return ResourceType::get(&context, resource.getState(), array(args));
              }
              if (auto owned = dyn_cast<OwnedType>(type)) return OwnedType::get(&context, instantiate(owned.getValueType()));
              if (auto unresolved = dyn_cast<UnresolvedType>(type)) return UnresolvedType::get(&context, substitute(unresolved.getSourceType()));
              return type;
            };
            for (auto type : signature.getInputs()) expectedInputs.push_back(instantiate(type));
            for (auto type : signature.getResults()) expectedResults.push_back(instantiate(type));
          }
          if (op.operands.size() != expectedInputs.size()) fail("call argument count mismatch");
          for (auto [arg, type] : llvm::zip_equal(op.operands, expectedInputs))
            if (valueTypes.at(arg) != type) fail("call argument type mismatch");
          auto result = valueType(Syntax::valueType(required(op.attributes, "type")));
          if ((expectedResults.empty() && !isa<NoneType>(result)) ||
              (!expectedResults.empty() && result != expectedResults.front())) fail("call result type mismatch");
        }
        ids.push_back(integer(id));
        if (defines(op)) available.insert(id);
        if (isSlot(op)) {
          if (required(op.attributes, "initialized") == "true") initialized.insert(id);
          else initialized.erase(id);
        }
      }
      SmallVector<NamedAttribute> attrs{named("id", integer(block.id)), named("scopes", array(scopes)),
          named("operations", array(ids)), named("location", location(block.location))};
      if (!block.attributes.empty()) attrs.push_back(named("properties", properties(block.attributes)));
      if (block.loopContext) attrs.push_back(named("loop_context", contextRef(block.loopContext)));
      if (block.terminator.kind == graphir::ControlFlowTerminatorKind::Branch) {
        if (!block.terminator.successor || !blockIds.contains(*block.terminator.successor)) fail("invalid CFG successor");
        attrs.push_back(named("terminator", str("branch")));
        attrs.push_back(named("successor", integer(*block.terminator.successor)));
        if (!block.terminator.control.empty()) attrs.push_back(named("control", str(block.terminator.control)));
      } else if (block.terminator.kind == graphir::ControlFlowTerminatorKind::ConditionalBranch) {
        if (!block.terminator.successor || !block.terminator.falseSuccessor ||
            !blockIds.contains(*block.terminator.successor) || !blockIds.contains(*block.terminator.falseSuccessor) ||
            block.terminator.operands.size() != 1) fail("invalid conditional branch");
        const auto condition = block.terminator.operands.front();
        operand(condition);
        if (!valueTypes.at(condition).isInteger(1)) fail("conditional branch predicate is not Boolean");
        attrs.push_back(named("terminator", str("conditional_branch")));
        attrs.push_back(named("condition", integer(condition)));
        attrs.push_back(named("successor", integer(*block.terminator.successor)));
        attrs.push_back(named("false_successor", integer(*block.terminator.falseSuccessor)));
      } else {
        if (block.terminator.operands.size() != results.size()) fail("return signature mismatch");
        for (auto id : block.terminator.operands) {
          operand(id);
          if (valueTypes.at(id) != results.front()) fail("return value type mismatch");
          returns.push_back(integer(id));
        }
        attrs.push_back(named("terminator", str("return")));
        attrs.push_back(named("operands", array(returns)));
      }
      cfgBlocks.push_back(dict(attrs));
    }
    current = graph.location();
    auto where = requirements(required(graph.attributes(), "where"));
    contextRequirements.append(where.begin(), where.end());
    SmallVector<NamedAttribute> attrs{named("function_type", TypeAttr::get(builder.getFunctionType(inputs, results))),
        named("parameter_attrs", array(params)), named("result_attrs", array(resultAttrs)),
        named("context_requirements", array(contextRequirements)),
        named("effects", effects(required(graph.attributes(), "effects"))),
        named("execution_domain", str(required(graph.attributes(), "execution_domain"))),
        named("deferred_body", deferredBody), named("deferred_cfg", array(cfgBlocks))};
    if (graph.context()) attrs.push_back(named("context", contextRef(graph.context())));
    std::string owner;
    if (kind == "stage") owner = reference(graph.name()).getRootReference().str();
    create(kind, graph.name(), attrs, false, owner);
  }

public:
  Importer(const graphir::Module &source, MLIRContext &context)
      : source(source), context(context), builder(&context) {
    context.getOrLoadDialect<MetreonDialect>();
    module = ModuleOp::create(location({}));
    (*module)->setAttr("metreon.schema_version", integer(1));
    (*module)->setAttr("metreon.import_stage", str("semantic_structure"));
    (*module)->setAttr("metreon.ordinary_lowering", str("deferred"));
  }
  FailureOr<OwningOpRef<ModuleOp>> run() {
    try {
      allocateSymbols();
      importContexts();
      importResources();
      importCompositions();
      for (const auto &graph : source.procedureGraphs()) importCallable(graph, "procedure");
      for (const auto &graph : source.kernelGraphs()) importCallable(graph, "kernel");
      for (const auto &graph : source.stageGraphs()) importCallable(graph, "stage");
      if (failed(verify(*module))) return failure();
      return std::move(module);
    } catch (const std::exception &error) {
      emitError(location(current)) << "GraphIR-to-Metreon import: " << error.what();
      return failure();
    }
  }
};
} // namespace

FailureOr<OwningOpRef<ModuleOp>> importGraphIR(const graphir::Module &source, MLIRContext &context) {
  return Importer(source, context).run();
}
} // namespace metreon
