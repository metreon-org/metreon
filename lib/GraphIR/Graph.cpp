#include "metreon/GraphIR/Graph.h"

#include <atomic>
#include <sstream>
#include <stdexcept>
#include <string>

namespace metreon::graphir {

namespace {

std::atomic<std::uint64_t> nextModuleIdentity{0};

std::uint64_t mintModuleIdentity() noexcept {
  return nextModuleIdentity.fetch_add(1, std::memory_order_relaxed);
}

std::string escapeString(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }
  return escaped;
}

void printAttributes(std::ostringstream &output,
                     const std::map<std::string, std::string> &attributes) {
  output << " {";
  bool first = true;
  for (const auto &[key, value] : attributes) {
    if (!first) {
      output << ", ";
    }
    first = false;
    output << key << " = \"" << escapeString(value) << '"';
  }
  output << '}';
}

std::string formatCapabilitySet(const std::vector<std::string> &capabilities) {
  std::ostringstream output;
  output << '{';
  for (std::size_t index = 0; index < capabilities.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << capabilities[index];
  }
  output << '}';
  return output.str();
}

} // namespace

bool ContextKey::operator==(const ContextKey &other) const noexcept {
  return moduleIdentity_ == other.moduleIdentity_ && ordinal_ == other.ordinal_;
}

const char *nodeKindName(NodeKind kind) {
  switch (kind) {
  case NodeKind::ContextVariable:
    return "context_var";
  case NodeKind::Grant:
    return "grant";
  }
  return "unknown";
}

const char *edgeKindName(EdgeKind kind) {
  switch (kind) {
  case EdgeKind::Argument:
    return "argument";
  case EdgeKind::Transition:
    return "transition";
  }
  return "unknown";
}

const char *contextResolutionName(ContextResolution resolution) {
  switch (resolution) {
  case ContextResolution::Declared:
    return "declared";
  case ContextResolution::External:
    return "external";
  }
  return "unknown";
}

const char *contextUseName(ContextUse use) {
  switch (use) {
  case ContextUse::MetadataAttachment:
    return "metadata attachment";
  case ContextUse::RuntimeStore:
    return "runtime storage";
  case ContextUse::Transmute:
    return "transmute";
  case ContextUse::Cast:
    return "cast";
  case ContextUse::CrossThreadSend:
    return "cross-thread send";
  case ContextUse::RuntimeCapture:
    return "runtime capture";
  }
  return "unknown use";
}

Module::Module(std::string sourceName)
    : sourceName_(std::move(sourceName)),
      moduleIdentity_(mintModuleIdentity()) {
}

Module::Module(Module &&other) noexcept
    : sourceName_(std::move(other.sourceName_)),
      moduleIdentity_(other.moduleIdentity_),
      contexts_(std::move(other.contexts_)), nodes_(std::move(other.nodes_)),
      edges_(std::move(other.edges_)),
      resourceGraphs_(std::move(other.resourceGraphs_)),
      resourceContextTemplates_(
          std::move(other.resourceContextTemplates_)),
      kernelGraphs_(std::move(other.kernelGraphs_)) {
  // Keep a moved-from module valid without letting it mint duplicate keys.
  other.sourceName_.clear();
  other.contexts_.clear();
  other.nodes_.clear();
  other.edges_.clear();
  other.resourceGraphs_.clear();
  other.resourceContextTemplates_.clear();
  other.kernelGraphs_.clear();
  other.moduleIdentity_ = mintModuleIdentity();
}

Module &Module::operator=(Module &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  sourceName_ = std::move(other.sourceName_);
  moduleIdentity_ = other.moduleIdentity_;
  contexts_ = std::move(other.contexts_);
  nodes_ = std::move(other.nodes_);
  edges_ = std::move(other.edges_);
  resourceGraphs_ = std::move(other.resourceGraphs_);
  resourceContextTemplates_ = std::move(other.resourceContextTemplates_);
  kernelGraphs_ = std::move(other.kernelGraphs_);
  other.sourceName_.clear();
  other.contexts_.clear();
  other.nodes_.clear();
  other.edges_.clear();
  other.resourceGraphs_.clear();
  other.resourceContextTemplates_.clear();
  other.kernelGraphs_.clear();
  other.moduleIdentity_ = mintModuleIdentity();
  return *this;
}

ResourceNodeId ResourceGraph::addState(
    std::string name, std::map<std::string, std::string> attributes,
    SourceLocation location) {
  const ResourceNodeId id = states_.size();
  states_.push_back(ResourceStateNode{id, std::move(name),
                                      std::move(attributes), location});
  return id;
}

KernelVariableId KernelGraph::addVariable(
    std::string name, std::map<std::string, std::string> attributes,
    SourceLocation location) {
  const KernelVariableId id = variables_.size();
  variables_.push_back(KernelVariableNode{
      id, std::move(name), std::move(attributes), location});
  return id;
}

void ResourceGraph::addTransition(
    ResourceNodeId source, ResourceNodeId target,
    std::map<std::string, std::string> attributes, SourceLocation location) {
  if (source >= states_.size() || target >= states_.size()) {
    throw std::logic_error(
        "GraphIR resource transition refers to an unknown state node");
  }
  transitions_.push_back(ResourceTransitionEdge{
      source, target, std::move(attributes), location});
}

ContextTemplateSpecialization &
ResourceContextTemplate::addSpecialization(ContextMetadataRef context) {
  specializations_.push_back(
      ContextTemplateSpecialization{std::move(context), {}});
  return specializations_.back();
}

ContextMetadataRef Module::addContextMetadata(
    std::string name, std::string identifier, std::size_t genericArity,
    ContextResolution resolution, SourceLocation location) {
  const std::size_t ordinal = contexts_.size();
  ContextMetadataRef context(new ContextMetadata(
      moduleIdentity_, ordinal, std::move(name), std::move(identifier),
      genericArity, resolution, location));
  contexts_.push_back(context);
  return context;
}

bool Module::ownsContext(const ContextMetadataRef &context) const noexcept {
  if (!context || context->key_.moduleIdentity_ != moduleIdentity_) {
    return false;
  }

  const std::size_t ordinal = context->key_.ordinal_;
  return ordinal < contexts_.size() &&
         contexts_[ordinal].get() == context.get();
}

void Module::requireContextUse(const ContextMetadataRef &context,
                               ContextUse use) const {
  if (!ownsContext(context)) {
    throw std::logic_error(
        "GraphIR context metadata was not minted by this module");
  }
  if (!context->permits(use)) {
    throw std::logic_error("GraphIR context key forbids " +
                           std::string(contextUseName(use)));
  }
}

NodeId Module::addNode(NodeKind kind, std::string name,
                       std::map<std::string, std::string> attributes,
                       ContextMetadataRef context, SourceLocation location) {
  requireContextUse(context, ContextUse::MetadataAttachment);
  const NodeId id = nodes_.size();
  nodes_.push_back(Node{id, kind, std::move(name), std::move(attributes),
                        std::move(context), location});
  return id;
}

void Module::addEdge(NodeId source, NodeId target, EdgeKind kind,
                     std::map<std::string, std::string> attributes) {
  if (source >= nodes_.size() || target >= nodes_.size()) {
    throw std::logic_error("GraphIR edge refers to an unknown node");
  }
  edges_.push_back(
      Edge{source, EdgeTarget{target}, kind, std::move(attributes)});
}

void Module::addContextEdge(
    NodeId source, ContextMetadataRef target, EdgeKind kind,
    std::map<std::string, std::string> attributes) {
  if (source >= nodes_.size()) {
    throw std::logic_error("GraphIR edge refers to an unknown source node");
  }
  if (kind != EdgeKind::Transition) {
    throw std::logic_error(
        "GraphIR context metadata may only be an edge transition target");
  }
  requireContextUse(target, ContextUse::MetadataAttachment);
  edges_.push_back(Edge{source, EdgeTarget{std::move(target)}, kind,
                        std::move(attributes)});
}

ResourceGraph &Module::addResourceGraph(
    std::string name, std::map<std::string, std::string> attributes,
    SourceLocation location) {
  resourceGraphs_.emplace_back(std::move(name), std::move(attributes),
                               location);
  return resourceGraphs_.back();
}

ResourceContextTemplate &Module::addResourceContextTemplate(
    std::string resourceName, std::string contextParameter,
    SourceLocation location) {
  resourceContextTemplates_.emplace_back(
      std::move(resourceName), std::move(contextParameter), location);
  return resourceContextTemplates_.back();
}

KernelGraph &Module::addKernelGraph(std::string name,
                                    SourceLocation location) {
  kernelGraphs_.emplace_back(std::move(name), location);
  return kernelGraphs_.back();
}

std::string Module::print() const {
  std::ostringstream output;
  output << "graphir.module {\n";
  output << "  graphir.graph @contexts {\n";

  for (const ContextMetadataRef &context : contexts_) {
    const std::size_t ordinal = context->key_.ordinal_;
    std::ostringstream key;
    // The textual key is module-scoped and reproducible. The private module
    // identity still participates in in-memory equality and provenance checks.
    key << "!graphir.context_key<" << ordinal << '>';

    output << "    #ctx" << ordinal << " = graphir.context_metadata \""
           << escapeString(context->name_) << '"';
    std::map<std::string, std::string> attributes = {
        {"castable", "false"},
        {"generic_arity", std::to_string(context->genericArity_)},
        {"key", key.str()},
        {"provenance", "compiler_minted"},
        {"resolution", contextResolutionName(context->resolution_)},
        {"runtime_capturable", "false"},
        {"runtime_materializable", "false"},
        {"sendable", "false"},
        {"storable", "false"},
        {"transmutable", "false"},
    };
    if (context->hasIdentifier()) {
      attributes.emplace("identifier", context->identifier_);
    }
    printAttributes(output, attributes);
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << context->location_.line << ':' << context->location_.column
           << ")\n";
    if (context->hasIdentifier()) {
      output << "    graphir.context_identifier \""
             << escapeString(context->identifier_) << "\" -> #ctx" << ordinal
             << '\n';
    }
  }

  if (!contexts_.empty() && (!nodes_.empty() || !edges_.empty())) {
    output << '\n';
  }

  for (const Node &node : nodes_) {
    output << "    %n" << node.id << " = graphir."
           << nodeKindName(node.kind) << " \"" << escapeString(node.name)
           << '"';
    printAttributes(output, node.attributes);
    output << " context(#ctx" << node.context->key_.ordinal_ << ')';
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << node.location.line << ':' << node.location.column << ")\n";
  }

  if (!nodes_.empty() && !edges_.empty()) {
    output << '\n';
  }

  for (const Edge &edge : edges_) {
    output << "    graphir.edge %n" << edge.source << " -> ";
    if (const NodeId *targetNode = std::get_if<NodeId>(&edge.target)) {
      output << "%n" << *targetNode;
    } else {
      const ContextMetadataRef &targetContext =
          std::get<ContextMetadataRef>(edge.target);
      output << "#ctx" << targetContext->key_.ordinal_;
    }
    std::map<std::string, std::string> attributes = edge.attributes;
    attributes.emplace("kind", edgeKindName(edge.kind));
    printAttributes(output, attributes);
    output << '\n';
  }

  output << "  }\n";

  for (std::size_t kernelIndex = 0; kernelIndex < kernelGraphs_.size();
       ++kernelIndex) {
    const KernelGraph &kernel = kernelGraphs_[kernelIndex];
    output << '\n';
    output << "  graphir.kernel @\"" << escapeString(kernel.name()) << "\"";
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << kernel.location().line << ':' << kernel.location().column
           << ") {\n";

    for (const KernelVariableNode &variable : kernel.variables()) {
      output << "    %k" << kernelIndex << "v" << variable.id
             << " = graphir.variable_decl \""
             << escapeString(variable.name) << '\"';
      printAttributes(output, variable.attributes);
      output << " loc(\"" << escapeString(sourceName_) << "\":"
             << variable.location.line << ':' << variable.location.column
             << ")\n";
    }

    output << "  }\n";
  }

  for (const ResourceGraph &resource : resourceGraphs_) {
    output << '\n';
    output << "  graphir.resource_graph \"" << escapeString(resource.name())
           << '\"';
    printAttributes(output, resource.attributes());
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << resource.location().line << ':' << resource.location().column
           << ") {\n";

    for (const ResourceStateNode &state : resource.states()) {
      output << "    %s" << state.id << " = graphir.resource_state \""
             << escapeString(state.name) << '\"';
      printAttributes(output, state.attributes);
      output << " loc(\"" << escapeString(sourceName_) << "\":"
             << state.location.line << ':' << state.location.column << ")\n";
    }

    if (!resource.states().empty() && !resource.transitions().empty()) {
      output << '\n';
    }

    for (const ResourceTransitionEdge &transition :
         resource.transitions()) {
      output << "    graphir.resource_edge %s" << transition.source
             << " -> %s" << transition.target;
      std::map<std::string, std::string> attributes = transition.attributes;
      attributes.emplace("kind", "transition");
      printAttributes(output, attributes);
      output << " loc(\"" << escapeString(sourceName_) << "\":"
             << transition.location.line << ':' << transition.location.column
             << ")\n";
    }

    output << "  }\n";
  }

  if (!resourceContextTemplates_.empty()) {
    output << '\n';
    output << "  graphir.template_section {\n";
    for (const ResourceContextTemplate &resourceTemplate :
         resourceContextTemplates_) {
      output << "    graphir.resource_template \""
             << escapeString(resourceTemplate.resourceName())
             << "\" context(\""
             << escapeString(resourceTemplate.contextParameter()) << "\")";
      output << " loc(\"" << escapeString(sourceName_) << "\":"
             << resourceTemplate.location().line << ':'
             << resourceTemplate.location().column << ") {\n";

      for (const ContextTemplateSpecialization &specialization :
           resourceTemplate.specializations()) {
        if (!ownsContext(specialization.context)) {
          throw std::logic_error(
              "GraphIR template refers to context metadata not minted by "
              "this module");
        }
        const std::size_t contextOrdinal =
            specialization.context->key_.ordinal_;
        output << "      graphir.context_mapping \""
               << escapeString(resourceTemplate.contextParameter())
               << "\" -> #ctx" << contextOrdinal << " identifier(\""
               << escapeString(specialization.context->identifier_)
               << "\") {\n";

        for (const ResourceTypeCheck &check : specialization.checks) {
          std::map<std::string, std::string> attributes = {
              {"required",
               formatCapabilitySet(check.requiredCapabilities)},
              {"result", check.isValid() ? "valid" : "invalid"},
          };
          if (!check.isValid()) {
            const std::string missing =
                formatCapabilitySet(check.missingCapabilities);
            attributes.emplace("missing", missing);
            attributes.emplace(
                "message",
                "resource type check failed: context #ctx" +
                    std::to_string(contextOrdinal) + " identifier `" +
                    specialization.context->identifier_ + "` of type `" +
                    specialization.context->name_ +
                    "` does not grant required capabilities " + missing +
                    " for `" + resourceTemplate.resourceName() + "::" +
                    check.transitionName + "`");
          }

          output << "        graphir.type_check \""
                 << escapeString(check.transitionName) << '\"';
          printAttributes(output, attributes);
          output << " loc(\"" << escapeString(sourceName_) << "\":"
                 << check.location.line << ':' << check.location.column
                 << ")\n";
        }

        output << "      }\n";
      }

      output << "    }\n";
    }
    output << "  }\n";
  }

  output << "}\n";
  return output.str();
}

} // namespace metreon::graphir
