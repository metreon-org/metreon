#pragma once

#include "metreon/Basic/SourceLocation.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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

struct ResourceStateNode {
  ResourceNodeId id = 0;
  std::string name;
  std::map<std::string, std::string> attributes;
  SourceLocation location;
};

struct ResourceTransitionEdge {
  ResourceNodeId source = 0;
  ResourceNodeId target = 0;
  std::map<std::string, std::string> attributes;
  SourceLocation location;
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

  const std::string &name() const noexcept { return name_; }
  const std::map<std::string, std::string> &attributes() const noexcept {
    return attributes_;
  }
  const SourceLocation &location() const noexcept { return location_; }
  const std::vector<ResourceStateNode> &states() const noexcept {
    return states_;
  }
  const std::vector<ResourceTransitionEdge> &transitions() const noexcept {
    return transitions_;
  }

private:
  std::string name_;
  std::map<std::string, std::string> attributes_;
  SourceLocation location_;
  std::vector<ResourceStateNode> states_;
  std::vector<ResourceTransitionEdge> transitions_;
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

private:
  std::string resourceName_;
  std::string contextParameter_;
  SourceLocation location_;
  std::vector<ContextTemplateSpecialization> specializations_;
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

  // This is the enforcement point future runtime lowerings must use before
  // consuming context evidence. Every runtime-oriented use is rejected.
  void requireContextUse(const ContextMetadataRef &context,
                         ContextUse use) const;

  const std::vector<ContextMetadataRef> &contexts() const noexcept {
    return contexts_;
  }
  const std::vector<Node> &nodes() const noexcept { return nodes_; }
  const std::vector<Edge> &edges() const noexcept { return edges_; }
  const std::vector<ResourceGraph> &resourceGraphs() const noexcept {
    return resourceGraphs_;
  }
  const std::vector<ResourceContextTemplate> &resourceContextTemplates()
      const noexcept {
    return resourceContextTemplates_;
  }
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
};

const char *nodeKindName(NodeKind kind);
const char *edgeKindName(EdgeKind kind);
const char *contextResolutionName(ContextResolution resolution);
const char *contextUseName(ContextUse use);

} // namespace metreon::graphir
