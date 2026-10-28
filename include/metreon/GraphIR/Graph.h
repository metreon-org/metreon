#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace metreon::graphir {

using NodeId = std::size_t;

enum class NodeKind {
  ContextVariable,
  Grant,
};

enum class EdgeKind {
  Argument,
  Transition,
};

enum class ContextResolution {
  Declared,
  External,
};

// Context evidence is compiler metadata, never a runtime value. These are the
// only ways a later lowering is allowed to ask to use that evidence.
enum class ContextUse {
  MetadataAttachment,
  RuntimeStore,
  Transmute,
  Cast,
  CrossThreadSend,
  RuntimeCapture,
};

class ContextKey final {
public:
  ContextKey(const ContextKey &) = delete;
  ContextKey &operator=(const ContextKey &) = delete;
  ContextKey(ContextKey &&) = delete;
  ContextKey &operator=(ContextKey &&) = delete;

  bool operator==(const ContextKey &other) const noexcept;
  bool operator!=(const ContextKey &other) const noexcept {
    return !(*this == other);
  }

private:
  ContextKey(std::uint64_t moduleIdentity, std::size_t ordinal)
      : moduleIdentity_(moduleIdentity), ordinal_(ordinal) {}

  std::uint64_t moduleIdentity_ = 0;
  std::size_t ordinal_ = 0;

  friend class ContextMetadata;
  friend class Module;
};

class ContextMetadata final {
public:
  ContextMetadata(const ContextMetadata &) = delete;
  ContextMetadata &operator=(const ContextMetadata &) = delete;
  ContextMetadata(ContextMetadata &&) = delete;
  ContextMetadata &operator=(ContextMetadata &&) = delete;

  bool hasSameIdentity(const ContextMetadata &other) const noexcept {
    return key_ == other.key_;
  }
  const std::string &name() const noexcept { return name_; }
  const std::string &identifier() const noexcept { return identifier_; }
  bool hasIdentifier() const noexcept { return !identifier_.empty(); }
  std::size_t genericArity() const noexcept { return genericArity_; }
  ContextResolution resolution() const noexcept { return resolution_; }
  const SourceLocation &location() const noexcept { return location_; }

  // Metadata attachment is the sole legal use. In particular, a context key
  // cannot be materialized as a value and therefore cannot be stored, cast,
  // transmuted, sent to another runtime thread, or captured by a closure.
  bool permits(ContextUse use) const noexcept {
    return use == ContextUse::MetadataAttachment;
  }
  bool isRuntimeMaterializable() const noexcept { return false; }

private:
  ContextMetadata(std::uint64_t moduleIdentity, std::size_t ordinal,
                  std::string name, std::string identifier,
                  std::size_t genericArity, ContextResolution resolution,
                  SourceLocation location)
      : key_(moduleIdentity, ordinal), name_(std::move(name)),
        identifier_(std::move(identifier)), genericArity_(genericArity),
        resolution_(resolution), location_(location) {}

  ContextKey key_;
  std::string name_;
  std::string identifier_;
  std::size_t genericArity_ = 0;
  ContextResolution resolution_ = ContextResolution::Declared;
  SourceLocation location_;

  friend class Module;
};

using ContextMetadataRef = std::shared_ptr<const ContextMetadata>;
using EdgeTarget = std::variant<NodeId, ContextMetadataRef>;

struct Node {
  NodeId id = 0;
  NodeKind kind = NodeKind::Grant;
  std::string name;
  std::map<std::string, std::string> attributes;
  ContextMetadataRef context;
  SourceLocation location;
};

struct Edge {
  NodeId source = 0;
  EdgeTarget target = NodeId{0};
  EdgeKind kind = EdgeKind::Argument;
  std::map<std::string, std::string> attributes;
};

using ResourceNodeId = std::size_t;

enum class DefinitionKind { Protocol, ProtocolState, PipelineState, PipelineEdge,
                            Stage, Topology, Schedule };

struct DefinitionReference {
  DefinitionKind kind = DefinitionKind::ProtocolState;
  std::size_t index = 0;
};

struct ResourceDefinitionReference {
  ResourceNodeId id = 0;
  DefinitionReference target;
  SourceLocation location;
};

struct ResourceMemberReference {
  std::size_t graph = 0;
  ResourceNodeId member = 0;
};

struct ResourceTransitionEdge {
  ResourceNodeId source = 0;
  ResourceNodeId target = 0;
  std::map<std::string, std::string> attributes;
  SourceLocation location;
};

struct ResourceStateNode {
  ResourceNodeId id = 0;
  std::string name;
  std::map<std::string, std::string> attributes;
  SourceLocation location;
  // A collapsed node is a structural container, not permission to skip its
  // transitions. Members retain their original IDs, types and source locations.
  std::vector<ResourceStateNode> members{};
  std::vector<ResourceTransitionEdge> internalTransitions{};
};

class ResourceGraph {
public:
  ResourceGraph(std::string name,
                std::map<std::string, std::string> attributes,
                SourceLocation location)
      : name_(std::move(name)), attributes_(std::move(attributes)),
        location_(location) {}

  ResourceNodeId addState(std::string name,
                          std::map<std::string, std::string> attributes,
                          SourceLocation location);
  void addTransition(ResourceNodeId source, ResourceNodeId target,
                     std::map<std::string, std::string> attributes,
                     SourceLocation location);
  void addEntryState(ResourceNodeId state);

  const std::string &name() const noexcept { return name_; }
  const std::map<std::string, std::string> &attributes() const noexcept {
    return attributes_;
  }
  const SourceLocation &location() const noexcept { return location_; }
  const std::vector<ResourceStateNode> &states() const noexcept {
    return states_;
  }
  std::vector<ResourceStateNode> &states() noexcept { return states_; }
  const std::vector<ResourceTransitionEdge> &transitions() const noexcept {
    return transitions_;
  }
  std::vector<ResourceTransitionEdge> &transitions() noexcept {
    return transitions_;
  }
  const auto &entryStates() const noexcept { return entryStates_; }
  ResourceNodeId addReference(DefinitionReference target, SourceLocation location) {
    const auto id = references_.size();
    references_.push_back({id, target, location});
    return id;
  }
  const auto &references() const noexcept { return references_; }
  auto &references() noexcept { return references_; }

private:
  std::string name_;
  std::map<std::string, std::string> attributes_;
  SourceLocation location_;
  std::vector<ResourceStateNode> states_;
  std::vector<ResourceTransitionEdge> transitions_;
  std::vector<ResourceDefinitionReference> references_;
  std::vector<ResourceNodeId> entryStates_;
  ResourceNodeId nextState_ = 0;
};

struct ResourceTypeCheck {
  std::string transitionName;
  std::vector<std::string> requiredCapabilities;
  std::vector<std::string> missingCapabilities;
  SourceLocation location;

  bool isValid() const noexcept { return missingCapabilities.empty(); }
};

struct ContextTemplateSpecialization {
  ContextMetadataRef context;
  std::vector<ResourceTypeCheck> checks;
};

class ResourceContextTemplate {
public:
  ResourceContextTemplate(std::string resourceName,
                          std::string contextParameter,
                          SourceLocation location)
      : resourceName_(std::move(resourceName)),
        contextParameter_(std::move(contextParameter)), location_(location) {}

  ContextTemplateSpecialization &
  addSpecialization(ContextMetadataRef context);

  const std::string &resourceName() const noexcept { return resourceName_; }
  const std::string &contextParameter() const noexcept {
    return contextParameter_;
  }
  const SourceLocation &location() const noexcept { return location_; }
  const std::vector<ContextTemplateSpecialization> &specializations()
      const noexcept {
    return specializations_;
  }
  auto &specializations() noexcept { return specializations_; }

private:
  std::string resourceName_;
  std::string contextParameter_;
  SourceLocation location_;
  std::vector<ContextTemplateSpecialization> specializations_;
};

using CallableValueId = std::size_t;

enum class CallableOperationKind {
  Literal, Variable, Call, Block, Return, ContextRegion
};

struct CallableOperation {
  CallableValueId id = 0;
  CallableOperationKind kind = CallableOperationKind::Variable;
  std::string name;
  std::map<std::string, std::string> attributes;
  std::vector<CallableValueId> operands;
  std::vector<CallableOperation> body;
  ContextMetadataRef context;
  SourceLocation location;
};

struct CallableParameter {
  // Context parameters have metadata identity, never a runtime value ID.
  std::optional<CallableValueId> id;
  std::string name;
  std::string type;
  ContextMetadataRef context;
  SourceLocation location;
};

class CallableGraph {
public:
  CallableGraph(std::string name, SourceLocation location)
      : name_(std::move(name)), location_(location) {}

  void setSignature(std::map<std::string, std::string> attributes,
                    ContextMetadataRef context) {
    attributes_ = std::move(attributes);
    context_ = std::move(context);
  }
  std::optional<CallableValueId> addParameter(std::string name, std::string type,
                                            ContextMetadataRef context,
                                            SourceLocation location);
  CallableOperation makeOperation(CallableOperationKind kind, std::string name,
                                  SourceLocation location);
  void setBody(std::vector<CallableOperation> body) { body_ = std::move(body); }

  const std::string &name() const noexcept { return name_; }
  const SourceLocation &location() const noexcept { return location_; }
  const auto &attributes() const noexcept { return attributes_; }
  const ContextMetadataRef &context() const noexcept { return context_; }
  const auto &parameters() const noexcept { return parameters_; }
  const auto &body() const noexcept { return body_; }

private:
  std::string name_;
  SourceLocation location_;
  std::map<std::string, std::string> attributes_;
  ContextMetadataRef context_;
  std::vector<CallableParameter> parameters_;
  std::vector<CallableOperation> body_;
  CallableValueId nextValue_ = 0;
};

using KernelGraph = CallableGraph;
using ProcedureGraph = CallableGraph;
using StageGraph = CallableGraph;

struct StageContextRegion {
  ContextMetadataRef context;
  // References to executions already listed by the fused worker, not extra
  // stage invocations. Context regions do not introduce lexical lifetimes.
  std::vector<DefinitionReference> stages;
  SourceLocation location;
};

struct CompositionRecord {
  std::string kind;
  std::string name;
  std::map<std::string, std::string> attributes;
  std::vector<DefinitionReference> references;
  SourceLocation location;
  std::vector<StageContextRegion> contextRegions{};
};

struct CompositionDefinition {
  DefinitionKind kind;
  std::string name;
  std::map<std::string, std::string> attributes;
  std::vector<CompositionRecord> records;
  SourceLocation location;
};

struct ProtocolGraph {
  std::string name;
  std::vector<std::string> roles;
  std::vector<ResourceMemberReference> members;
  SourceLocation location;
  std::vector<DefinitionReference> entryStates{};
};

struct PipelineGraph {
  std::string name;
  std::vector<CompositionRecord> parameters;
  std::vector<ResourceMemberReference> members;
  SourceLocation location;
};

class Module {
public:
  explicit Module(std::string sourceName);

  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Module(Module &&other) noexcept;
  Module &operator=(Module &&other) noexcept;

  ContextMetadataRef addContextMetadata(std::string name,
                                        std::string identifier,
                                        std::size_t genericArity,
                                        ContextResolution resolution,
                                        SourceLocation location);
  NodeId addNode(NodeKind kind, std::string name,
                 std::map<std::string, std::string> attributes,
                 ContextMetadataRef context, SourceLocation location);
  void addEdge(NodeId source, NodeId target, EdgeKind kind,
               std::map<std::string, std::string> attributes = {});
  void addContextEdge(NodeId source, ContextMetadataRef target,
                      EdgeKind kind,
                      std::map<std::string, std::string> attributes = {});
  ResourceGraph &addResourceGraph(
      std::string name, std::map<std::string, std::string> attributes,
      SourceLocation location);
  ResourceContextTemplate &addResourceContextTemplate(
      std::string resourceName, std::string contextParameter,
      SourceLocation location);
  KernelGraph &addKernelGraph(std::string name, SourceLocation location);
  ProcedureGraph &addProcedureGraph(std::string name, SourceLocation location);
  StageGraph &addStageGraph(std::string name, SourceLocation location);
  DefinitionReference addCompositionDefinition(CompositionDefinition definition);
  CompositionDefinition &compositionDefinition(DefinitionReference reference);
  void eraseCompositionDefinitions(const std::vector<std::size_t> &indices);
  std::string definitionName(DefinitionReference reference) const;
  ResourceMemberReference addResourceReference(std::size_t graph,
                                               DefinitionReference definition,
                                               SourceLocation location);
  ProtocolGraph &addProtocolGraph(std::string name, SourceLocation location);
  void setProtocolMembers(std::size_t index, std::vector<ResourceMemberReference> members);
  PipelineGraph &addPipelineGraph(PipelineGraph group);

  // This is the enforcement point future runtime lowerings must use before
  // consuming context evidence. Every runtime-oriented use is rejected.
  void requireContextUse(const ContextMetadataRef &context,
                         ContextUse use) const;
  std::size_t contextOrdinal(const ContextMetadataRef &context) const;

  const std::vector<ContextMetadataRef> &contexts() const noexcept {
    return contexts_;
  }
  const std::vector<Node> &nodes() const noexcept { return nodes_; }
  const std::vector<Edge> &edges() const noexcept { return edges_; }
  const std::vector<ResourceGraph> &resourceGraphs() const noexcept {
    return resourceGraphs_;
  }
  auto &resourceGraphs() noexcept { return resourceGraphs_; }
  const std::vector<ResourceContextTemplate> &resourceContextTemplates()
      const noexcept {
    return resourceContextTemplates_;
  }
  auto &resourceContextTemplates() noexcept { return resourceContextTemplates_; }
  const std::vector<KernelGraph> &kernelGraphs() const noexcept {
    return kernelGraphs_;
  }
  auto &kernelGraphs() noexcept { return kernelGraphs_; }
  const std::vector<ProcedureGraph> &procedureGraphs() const noexcept {
    return procedureGraphs_;
  }
  auto &procedureGraphs() noexcept { return procedureGraphs_; }
  const auto &stageGraphs() const noexcept { return stageGraphs_; }
  auto &stageGraphs() noexcept { return stageGraphs_; }
  const auto &compositionDefinitions() const noexcept { return compositionDefinitions_; }
  auto &compositionDefinitions() noexcept { return compositionDefinitions_; }
  const auto &protocolGraphs() const noexcept { return protocolGraphs_; }
  auto &protocolGraphs() noexcept { return protocolGraphs_; }
  const auto &pipelineGraphs() const noexcept { return pipelineGraphs_; }
  const std::string &sourceName() const noexcept { return sourceName_; }

  std::string print() const;

private:
  bool ownsContext(const ContextMetadataRef &context) const noexcept;

  std::string sourceName_;
  std::uint64_t moduleIdentity_ = 0;
  std::vector<ContextMetadataRef> contexts_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<ResourceGraph> resourceGraphs_;
  std::vector<ResourceContextTemplate> resourceContextTemplates_;
  std::vector<KernelGraph> kernelGraphs_;
  std::vector<ProcedureGraph> procedureGraphs_;
  std::vector<StageGraph> stageGraphs_;
  std::vector<CompositionDefinition> compositionDefinitions_;
  std::vector<ProtocolGraph> protocolGraphs_;
  std::vector<PipelineGraph> pipelineGraphs_;
};

const char *definitionKindName(DefinitionKind kind);

const char *nodeKindName(NodeKind kind);
const char *edgeKindName(EdgeKind kind);
const char *contextResolutionName(ContextResolution resolution);
const char *contextUseName(ContextUse use);

} // namespace metreon::graphir
