#include "metreon/GraphIR/Graph.h"
#include "metreon/GraphIR/ControlFlow.h"

#include <atomic>
#include <functional>
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
      kernelGraphs_(std::move(other.kernelGraphs_)),
      procedureGraphs_(std::move(other.procedureGraphs_)) {
  // Keep a moved-from module valid without letting it mint duplicate keys.
  other.sourceName_.clear();
  other.contexts_.clear();
  other.nodes_.clear();
  other.edges_.clear();
  other.resourceGraphs_.clear();
  other.resourceContextTemplates_.clear();
  other.kernelGraphs_.clear();
  other.procedureGraphs_.clear();
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
  procedureGraphs_ = std::move(other.procedureGraphs_);
  other.sourceName_.clear();
  other.contexts_.clear();
  other.nodes_.clear();
  other.edges_.clear();
  other.resourceGraphs_.clear();
  other.resourceContextTemplates_.clear();
  other.kernelGraphs_.clear();
  other.procedureGraphs_.clear();
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

std::optional<CallableValueId> CallableGraph::addParameter(
    std::string name, std::string type, ContextMetadataRef context,
    SourceLocation location) {
  std::optional<CallableValueId> id;
  if (!context) {
    id = nextValue_++;
  }
  parameters_.push_back({id, std::move(name), std::move(type),
                         std::move(context), location});
  return id;
}

CallableOperation CallableGraph::makeOperation(CallableOperationKind kind,
                                                std::string name,
                                                SourceLocation location) {
  CallableOperation operation;
  operation.id = nextValue_++;
  operation.kind = kind;
  operation.name = std::move(name);
  operation.location = location;
  return operation;
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

ProcedureGraph &Module::addProcedureGraph(std::string name,
                                           SourceLocation location) {
  procedureGraphs_.emplace_back(std::move(name), location);
  return procedureGraphs_.back();
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

  auto printContext = [&](const ContextMetadataRef &context) {
    if (context) {
      requireContextUse(context, ContextUse::MetadataAttachment);
      output << " context(#ctx" << context->key_.ordinal_ << ')';
    }
  };
  auto printLocation = [&](SourceLocation location) {
    output << " loc(\"" << escapeString(sourceName_) << "\":"
           << location.line << ':' << location.column << ')';
  };
  auto printCallables = [&](const std::vector<CallableGraph> &callables,
                            const std::string &kind, const std::string &prefix) {
    for (std::size_t index = 0; index < callables.size(); ++index) {
      const auto &callable = callables[index];
      const std::string valuePrefix = "%" + prefix + std::to_string(index) + "v";
      output << "\n  graphir." << kind << " @\"" << escapeString(callable.name()) << '\"';
      printAttributes(output, callable.attributes());
      printContext(callable.context());
      printLocation(callable.location());
      output << " {\n";
      for (const auto &parameter : callable.parameters()) {
        output << "    ";
        if (parameter.id) {
          output << valuePrefix << *parameter.id << " = graphir.parameter ";
        } else {
          output << "graphir.context_parameter ";
        }
        output << '\"' << escapeString(parameter.name) << '\"';
        printAttributes(output, {{"type", parameter.type}});
        printContext(parameter.context);
        printLocation(parameter.location);
        output << '\n';
      }
      std::function<void(const std::vector<CallableOperation> &, std::size_t)> printBody;
      printBody = [&](const auto &body, std::size_t indent) {
        for (const auto &operation : body) {
          output << std::string(indent, ' ');
          const bool isBlock = operation.kind == CallableOperationKind::Block;
          const bool isReturn = operation.kind == CallableOperationKind::Return;
          const auto type = operation.attributes.find("type");
          const bool isVoid = type != operation.attributes.end() && type->second == "void";
          if (!isBlock && !isReturn && !isVoid) {
            output << valuePrefix << operation.id << " = ";
          }
          const char *operationName = "";
          switch (operation.kind) {
          case CallableOperationKind::Literal: operationName = "literal"; break;
          case CallableOperationKind::Variable: operationName = "variable_decl"; break;
          case CallableOperationKind::Call: operationName = "call"; break;
          case CallableOperationKind::Block: operationName = "block"; break;
          case CallableOperationKind::Return: operationName = "return"; break;
          }
          output << "graphir." << operationName;
          if (!operation.name.empty()) {
            output << " \"" << escapeString(operation.name) << '\"';
          }
          if (!operation.operands.empty()) {
            output << " operands(";
            for (std::size_t operand = 0; operand < operation.operands.size(); ++operand) {
              if (operand != 0) {
                output << ", ";
              }
              output << valuePrefix << operation.operands[operand];
            }
            output << ')';
          }
          if (!operation.attributes.empty()) {
            printAttributes(output, operation.attributes);
          }
          // All operations need an identity, including void calls, scopes,
          // and returns that do not define a runtime value.
          output << " id(#" << prefix << index << "op" << operation.id << ')';
          printContext(operation.context);
          printLocation(operation.location);
          if (isBlock) {
            output << " {\n";
            printBody(operation.body, indent + 2);
            output << std::string(indent, ' ') << "}\n";
          } else {
            output << '\n';
          }
        }
      };
      printBody(callable.body(), 4);
      output << "  }\n";
    }
  };
  printCallables(kernelGraphs_, "kernel", "k");
  printCallables(procedureGraphs_, "procedure", "p");

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

  output << "\n  graphir.cfg {\n";
  auto printControlFlow = [&](const std::vector<CallableGraph> &callables,
                              const std::string &kind,
                              const std::string &prefix) {
    for (std::size_t index = 0; index < callables.size(); ++index) {
      const auto &callable = callables[index];
      const auto cfg = buildControlFlow(callable);
      const auto callablePrefix = prefix + std::to_string(index);
      const auto blockPrefix = "^" + callablePrefix + "bb";
      const auto operationPrefix = "#" + callablePrefix + "op";
      const auto valuePrefix = "%" + callablePrefix + "v";
      output << "    graphir.cfg." << kind << " @\""
             << escapeString(callable.name()) << "\" entry("
             << blockPrefix << cfg.entry << ") parameters(";
      bool firstParameter = true;
      for (const auto &parameter : callable.parameters()) {
        if (!parameter.id) {
          continue;
        }
        if (!firstParameter) {
          output << ", ";
        }
        firstParameter = false;
        output << valuePrefix << *parameter.id;
      }
      output << ')';
      printLocation(callable.location());
      output << " {\n";
      for (const auto &block : cfg.blocks) {
        output << "      graphir.cfg.block " << blockPrefix << block.id
               << " scopes(";
        for (std::size_t scope = 0; scope < block.scopes.size(); ++scope) {
          if (scope != 0) {
            output << ", ";
          }
          output << operationPrefix << block.scopes[scope];
        }
        output << ')';
        printLocation(block.location);
        output << " {\n";
        for (const auto operation : block.operations) {
          output << "        graphir.cfg.op " << operationPrefix << operation << '\n';
        }
        const auto &terminator = block.terminator;
        if (terminator.kind == ControlFlowTerminatorKind::Branch) {
          output << "        graphir.cfg.br " << blockPrefix
                 << terminator.successor.value();
        } else {
          output << "        graphir.cfg.return";
          if (terminator.operation) {
            output << ' ' << operationPrefix << *terminator.operation;
          } else {
            printAttributes(output, {{"implicit", "true"}});
          }
          if (!terminator.operands.empty()) {
            output << " operands(";
            for (std::size_t operand = 0; operand < terminator.operands.size(); ++operand) {
              if (operand != 0) {
                output << ", ";
              }
              output << valuePrefix << terminator.operands[operand];
            }
            output << ')';
          }
        }
        printLocation(terminator.location);
        output << "\n      }\n";
      }
      output << "    }\n";
    }
  };
  printControlFlow(kernelGraphs_, "kernel", "k");
  printControlFlow(procedureGraphs_, "procedure", "p");
  output << "  }\n}\n";
  return output.str();
}

} // namespace metreon::graphir
