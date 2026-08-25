#include "metreon/Basic/Diagnostic.h"
#include "metreon/GraphIR/Lowering.h"
#include "metreon/Lexer/Lexer.h"
#include "metreon/Parser/Parser.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using metreon::graphir::ContextKey;
using metreon::graphir::ContextMetadata;
using metreon::graphir::ContextMetadataRef;
using metreon::graphir::ContextResolution;
using metreon::graphir::ContextUse;
using metreon::graphir::Edge;
using metreon::graphir::EdgeKind;
using metreon::graphir::Module;
using metreon::graphir::Node;
using metreon::graphir::NodeId;
using metreon::graphir::NodeKind;
using metreon::graphir::ResourceGraph;
using metreon::graphir::ResourceContextTemplate;
using metreon::graphir::ResourceStateNode;
using metreon::graphir::ResourceTransitionEdge;
using metreon::graphir::ResourceTypeCheck;
using metreon::graphir::ContextTemplateSpecialization;

static_assert(!std::is_default_constructible_v<ContextKey>);
static_assert(!std::is_copy_constructible_v<ContextKey>);
static_assert(!std::is_move_constructible_v<ContextKey>);
static_assert(!std::is_copy_constructible_v<ContextMetadata>);
static_assert(!std::is_move_constructible_v<ContextMetadata>);

void check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

metreon::ast::Module parse(const std::string &source) {
  metreon::parser::Parser parser(source);
  return parser.parseModule();
}

ContextMetadataRef findContext(
    const Module &graph, const std::string &name,
    ContextResolution resolution = ContextResolution::Declared,
    const std::string &identifier = "") {
  for (const ContextMetadataRef &context : graph.contexts()) {
    if (context->name() == name && context->resolution() == resolution &&
        (identifier.empty() || context->identifier() == identifier)) {
      return context;
    }
  }
  return {};
}

const Node *findNode(const Module &graph, NodeKind kind, const std::string &name,
                     const std::string &contextName = "") {
  for (const Node &node : graph.nodes()) {
    if (node.kind != kind || node.name != name) {
      continue;
    }
    if (contextName.empty() ||
        (node.context && node.context->name() == contextName)) {
      return &node;
    }
  }
  return nullptr;
}

const ResourceStateNode *findResourceState(const ResourceGraph &graph,
                                           const std::string &name) {
  for (const ResourceStateNode &state : graph.states()) {
    if (state.name == name) {
      return &state;
    }
  }
  return nullptr;
}

const ResourceTransitionEdge *
findResourceTransition(const ResourceGraph &graph, const std::string &name) {
  for (const ResourceTransitionEdge &transition : graph.transitions()) {
    const auto transitionName = transition.attributes.find("name");
    if (transitionName != transition.attributes.end() &&
        transitionName->second == name) {
      return &transition;
    }
  }
  return nullptr;
}

const ResourceContextTemplate *
findResourceTemplate(const Module &graph, const std::string &resourceName,
                     const std::string &contextParameter) {
  for (const ResourceContextTemplate &resourceTemplate :
       graph.resourceContextTemplates()) {
    if (resourceTemplate.resourceName() == resourceName &&
        resourceTemplate.contextParameter() == contextParameter) {
      return &resourceTemplate;
    }
  }
  return nullptr;
}

const ContextTemplateSpecialization *findSpecialization(
    const ResourceContextTemplate &resourceTemplate,
    const std::string &contextIdentifier) {
  for (const ContextTemplateSpecialization &specialization :
       resourceTemplate.specializations()) {
    if (specialization.context &&
        specialization.context->identifier() == contextIdentifier) {
      return &specialization;
    }
  }
  return nullptr;
}

const ResourceTypeCheck *
findTypeCheck(const ContextTemplateSpecialization &specialization,
              const std::string &transitionName) {
  for (const ResourceTypeCheck &check : specialization.checks) {
    if (check.transitionName == transitionName) {
      return &check;
    }
  }
  return nullptr;
}

bool hasNodeEdge(const Module &graph, NodeId source, NodeId target,
                 EdgeKind kind, const std::string &path = "") {
  for (const Edge &edge : graph.edges()) {
    const NodeId *targetNode = std::get_if<NodeId>(&edge.target);
    if (edge.source != source || targetNode == nullptr ||
        *targetNode != target || edge.kind != kind) {
      continue;
    }
    if (path.empty()) {
      return true;
    }
    const auto pathAttribute = edge.attributes.find("path");
    if (pathAttribute != edge.attributes.end() &&
        pathAttribute->second == path) {
      return true;
    }
  }
  return false;
}

bool hasContextEdge(const Module &graph, NodeId source,
                    const ContextMetadataRef &target, EdgeKind kind) {
  for (const Edge &edge : graph.edges()) {
    const ContextMetadataRef *targetContext =
        std::get_if<ContextMetadataRef>(&edge.target);
    if (edge.source == source && targetContext != nullptr && *targetContext &&
        (*targetContext)->hasSameIdentity(*target) && edge.kind == kind) {
      return true;
    }
  }
  return false;
}

void expectDiagnostic(const std::string &expectedCode,
                      const std::function<void()> &operation) {
  try {
    operation();
  } catch (const metreon::DiagnosticError &error) {
    check(error.diagnostic().code == expectedCode,
          "expected diagnostic `" + expectedCode + "`, got `" +
              error.diagnostic().code + "`");
    return;
  }
  throw std::runtime_error("expected diagnostic `" + expectedCode + "`");
}

void expectLogicError(const std::string &expectedText,
                      const std::function<void()> &operation) {
  try {
    operation();
  } catch (const std::logic_error &error) {
    check(std::string(error.what()).find(expectedText) != std::string::npos,
          "expected logic error containing `" + expectedText + "`, got `" +
              error.what() + "`");
    return;
  }
  throw std::runtime_error("expected logic error containing `" + expectedText +
                           "`");
}

const char *contextSource = R"(
context Host::Process grants {
  allocate,
  sleep,
  block,
  host_io
} process_ctx;

context Host::SoftIrq grants {
  atomic,
  mmio,
  defer<Host::Process>
} softirq_ctx;

context Host::HardIrq<I: Irq> grants {
  atomic,
  mmio,
  irq_ack<I>,
  defer<Host::SoftIrq>
} harirq_ctx;

context Host::Callback<S: StreamId> grants {
  atomic,
  defer<Host::Process>
} host_callback_ctx;
)";

const char *resourceSource = R"(
context Gpu::Thread<D: Device, U: Uniformity> grants {
  gpu_load,
  gpu_store
} thread1;

context Gpu::Thread<D: Device, U: Uniformity> grants {
  gpu_async_copy,
  gpu_await,
  gpu_atomic
} thread2;

resource CopySlot<S: AddressSpace> {
  state Vacant(storage: own buffer<S>);
  state InFlight(
    storage: own buffer<S>,
    ticket: own event<Copy>
  );
  state Ready(storage: own buffer<S>);

  transition begin<C: Context>(
    @cx: C;
    self: own Vacant,
    src: view<T, gpu::global>
  ) -> InFlight
  !{gpu_async_copy}
  where C allows {gpu_async_copy};

  await transition complete<C: Context>(
    @cx: C;
    self: own InFlight
  ) -> Ready
  !{gpu_await}
  where C allows {gpu_await};
}
)";

const char *accumulatorSource = R"(
context Gpu::PersistentWorker<
  D: Device,
  U: Uniform<Cluster>
> grants {
  gpu_async_copy
} temp_ctx_identifier;

resource TemporaryResource<S: AddressSpace> {
  state TempState1(storage: own Buffer<S>);
  state TempState2(storage: own Buffer<S>);

  accumulator TempState1(device);
  accumulator TempState2(cluster);

  transition change(
    @cx: C;
    self: own TempState1
  ) -> TempState2 !{gpu_async_copy}
  where C allows {gpu_async_copy};
}
)";

void tokenizesResourceDeclarations() {
  metreon::lexer::Lexer lexer(resourceSource);
  bool sawResource = false;
  bool sawState = false;
  bool sawTransition = false;
  bool sawAwait = false;
  bool sawWhere = false;
  bool sawAllows = false;
  bool sawOwn = false;
  bool sawAt = false;
  bool sawBang = false;
  bool sawArrow = false;

  while (true) {
    const metreon::lexer::Token token = lexer.next();
    switch (token.kind) {
    case metreon::lexer::TokenKind::KeywordResource:
      sawResource = true;
      break;
    case metreon::lexer::TokenKind::KeywordState:
      sawState = true;
      break;
    case metreon::lexer::TokenKind::KeywordTransition:
      sawTransition = true;
      break;
    case metreon::lexer::TokenKind::KeywordAwait:
      sawAwait = true;
      break;
    case metreon::lexer::TokenKind::KeywordWhere:
      sawWhere = true;
      break;
    case metreon::lexer::TokenKind::KeywordAllows:
      sawAllows = true;
      break;
    case metreon::lexer::TokenKind::KeywordOwn:
      sawOwn = true;
      break;
    case metreon::lexer::TokenKind::At:
      sawAt = true;
      break;
    case metreon::lexer::TokenKind::Bang:
      sawBang = true;
      break;
    case metreon::lexer::TokenKind::Arrow:
      sawArrow = true;
      break;
    default:
      break;
    }
    if (token.kind == metreon::lexer::TokenKind::EndOfFile) {
      break;
    }
  }

  check(sawResource && sawState && sawTransition && sawAwait && sawWhere &&
            sawAllows && sawOwn && sawAt && sawBang && sawArrow,
        "resource syntax was not fully tokenized");
}

void tokenizesAccumulatorDeclarations() {
  metreon::lexer::Lexer lexer(accumulatorSource);
  std::size_t accumulatorCount = 0;
  while (true) {
    const metreon::lexer::Token token = lexer.next();
    if (token.kind == metreon::lexer::TokenKind::KeywordAccumulator) {
      ++accumulatorCount;
    }
    if (token.kind == metreon::lexer::TokenKind::EndOfFile) {
      break;
    }
  }
  check(accumulatorCount == 2,
        "accumulator declarations were not tokenized as keywords");
}

void parsesResourceDeclarations() {
  const metreon::ast::Module module = parse(resourceSource);
  check(module.contexts.size() == 2,
        "resource example lost its context declarations");
  check(module.contexts[0].name.str() == "Gpu::Thread" &&
            module.contexts[1].name.str() == "Gpu::Thread" &&
            module.contexts[0].identifier == "thread1" &&
            module.contexts[1].identifier == "thread2",
        "same-type contexts were not distinguished by identifier");
  check(module.resources.size() == 1,
        "expected one resource declaration");

  const metreon::ast::ResourceDeclaration &resource =
      module.resources.front();
  check(resource.name.str() == "CopySlot", "lost resource name");
  check(resource.parameters.size() == 1 &&
            resource.parameters.front().name == "S" &&
            resource.parameters.front().constraint.str() == "AddressSpace",
        "lost resource generic parameter");
  check(resource.states.size() == 3, "expected three resource states");
  check(resource.states[1].name == "InFlight" &&
            resource.states[1].fields.size() == 2 &&
            resource.states[1].fields[1].type.str() == "own event<Copy>",
        "lost resource state payload fields");
  check(resource.transitions.size() == 2,
        "expected two resource transitions");

  const metreon::ast::ResourceTransitionDeclaration &begin =
      resource.transitions[0];
  const metreon::ast::ResourceTransitionDeclaration &complete =
      resource.transitions[1];
  check(!begin.isAwait && begin.parameters.size() == 3 &&
            begin.parameters.front().isContextEvidence &&
            begin.parameters[1].type.str() == "own Vacant" &&
            begin.resultState.str() == "InFlight" &&
            begin.effects.size() == 1 &&
            begin.effects.front().str() == "gpu_async_copy",
        "lost begin transition syntax");
  check(complete.isAwait && complete.resultState.str() == "Ready" &&
            complete.allowsClauses.size() == 1 &&
            complete.allowsClauses.front().str() ==
                "C allows {gpu_await}",
        "lost await transition conditions");
}

void parsesAndLowersAccumulators() {
  const metreon::ast::Module module = parse(accumulatorSource);
  check(module.resources.size() == 1,
        "accumulator example lost its resource declaration");
  const metreon::ast::ResourceDeclaration &resource = module.resources.front();
  check(resource.parameters.size() == 1 &&
            resource.parameters[0].name == "S" &&
            resource.parameters[0].constraint.str() == "AddressSpace",
        "accumulator resource lost its constrained generic parameter");
  check(resource.states.size() == 2 && resource.accumulators.size() == 2,
        "accumulator declarations were not retained in the AST");
  check(resource.accumulators[0].stateName == "TempState1" &&
            resource.accumulators[0].scope.str() == "device" &&
            resource.accumulators[1].stateName == "TempState2" &&
            resource.accumulators[1].scope.str() == "cluster",
        "accumulator state or persistence scope was lost");

  const Module graph = metreon::graphir::lowerModule(
      module, "accumulators.mtr");
  check(graph.resourceGraphs().size() == 1,
        "accumulator resource graph was not emitted");
  const ResourceGraph &resourceGraph = graph.resourceGraphs().front();
  const ResourceStateNode *first =
      findResourceState(resourceGraph, "TempState1");
  const ResourceStateNode *second =
      findResourceState(resourceGraph, "TempState2");
  check(first != nullptr && second != nullptr,
        "accumulator states were not lowered");
  const auto firstAccumulator = first->attributes.find("accumulator");
  const auto secondAccumulator = second->attributes.find("accumulator");
  check(firstAccumulator != first->attributes.end() &&
            firstAccumulator->second == "device" &&
            secondAccumulator != second->attributes.end() &&
            secondAccumulator->second == "cluster",
        "persistent scopes were not attached to GraphIR resource states");

  const std::string printed = graph.print();
  check(printed.find("graphir.resource_state \"TempState1\" "
                     "{accumulator = \"device\"") != std::string::npos &&
            printed.find("graphir.resource_state \"TempState2\" "
                         "{accumulator = \"cluster\"") != std::string::npos,
        "textual GraphIR omitted accumulator fields");
}

void acceptsAllAccumulatorScopes() {
  const std::string source = R"(
resource ScopeResource<S: AddressSpace> {
  state ThreadState();
  state WarpState();
  state BlockState();
  state DeviceState();
  state ClusterState();
  state HostPinnedState();

  accumulator ThreadState(thread);
  accumulator WarpState(warp);
  accumulator BlockState(block);
  accumulator DeviceState(device);
  accumulator ClusterState(cluster);
  accumulator HostPinnedState(host::pinned);
}
)";
  const Module graph = metreon::graphir::lowerModule(
      parse(source), "accumulator-scopes.mtr");
  const ResourceGraph &resource = graph.resourceGraphs().front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"ThreadState", "thread"},       {"WarpState", "warp"},
      {"BlockState", "block"},         {"DeviceState", "device"},
      {"ClusterState", "cluster"},     {"HostPinnedState", "host::pinned"},
  };
  for (const auto &[stateName, scope] : expected) {
    const ResourceStateNode *state = findResourceState(resource, stateName);
    check(state != nullptr, "allowed accumulator scope lost its state");
    const auto accumulator = state->attributes.find("accumulator");
    check(accumulator != state->attributes.end() &&
              accumulator->second == scope,
          "allowed accumulator scope was not preserved");
  }
}

void lowersResourcesAsSeparateGraphs() {
  const Module graph = metreon::graphir::lowerModule(
      parse(resourceSource), "resources.mtr");
  check(graph.contexts().size() == 2 && graph.nodes().size() == 9,
        "resource lowering changed the context/grant graph");
  check(graph.resourceGraphs().size() == 1,
        "resource lowering did not create a separate graph");
  const ContextMetadataRef thread1 = findContext(
      graph, "Gpu::Thread", ContextResolution::Declared, "thread1");
  const ContextMetadataRef thread2 = findContext(
      graph, "Gpu::Thread", ContextResolution::Declared, "thread2");
  check(thread1 && thread2 && !thread1->hasSameIdentity(*thread2),
        "same-type contexts did not retain distinct identities");

  const ResourceGraph &resource = graph.resourceGraphs().front();
  const ResourceStateNode *vacant = findResourceState(resource, "Vacant");
  const ResourceStateNode *inFlight =
      findResourceState(resource, "InFlight");
  const ResourceStateNode *ready = findResourceState(resource, "Ready");
  const ResourceTransitionEdge *begin =
      findResourceTransition(resource, "begin");
  const ResourceTransitionEdge *complete =
      findResourceTransition(resource, "complete");
  check(resource.name() == "CopySlot" && resource.states().size() == 3,
        "resource graph omitted state nodes");
  check(vacant != nullptr && inFlight != nullptr && ready != nullptr,
        "resource state nodes are incomplete");
  check(begin != nullptr && begin->source == vacant->id &&
            begin->target == inFlight->id,
        "begin was not lowered as Vacant -> InFlight");
  check(complete != nullptr && complete->source == inFlight->id &&
            complete->target == ready->id,
        "complete was not lowered as InFlight -> Ready");

  const auto awaitCondition = complete->attributes.find("await_condition");
  check(awaitCondition != complete->attributes.end() &&
            awaitCondition->second == "C allows {gpu_await}",
        "await condition was not attached to transition metadata");
  check(begin->attributes.find("await_condition") == begin->attributes.end(),
        "non-await transition gained await-condition metadata");

  const std::string printed = graph.print();
  const std::size_t resourceGraphPosition =
      printed.find("graphir.resource_graph \"CopySlot\"");
  const std::size_t templateSectionPosition =
      printed.find("graphir.template_section");
  check(printed.find("graphir.graph @contexts") != std::string::npos &&
            resourceGraphPosition != std::string::npos &&
            printed.find("graphir.resource_state \"InFlight\"") !=
                std::string::npos &&
            printed.find("await_condition = \"C allows {gpu_await}\"") !=
                std::string::npos,
        "textual GraphIR omitted the separate resource graph");
  check(templateSectionPosition != std::string::npos &&
            templateSectionPosition > resourceGraphPosition,
        "template section must be emitted after the resource graph");
  check(printed.find("graphir.context_identifier \"thread1\" -> #ctx0") !=
                std::string::npos &&
            printed.find(
                "graphir.context_identifier \"thread2\" -> #ctx1") !=
                std::string::npos &&
            printed.find("#ctx0 identifier(\"thread1\")") !=
                std::string::npos &&
            printed.find("#ctx1 identifier(\"thread2\")") !=
                std::string::npos,
        "GraphIR did not map context identifiers to their #ctx metadata");

  const ResourceContextTemplate *resourceTemplate =
      findResourceTemplate(graph, "CopySlot", "C");
  check(resourceTemplate != nullptr,
        "same-type context template was not lowered");
  const ContextTemplateSpecialization *thread1Specialization =
      findSpecialization(*resourceTemplate, "thread1");
  const ContextTemplateSpecialization *thread2Specialization =
      findSpecialization(*resourceTemplate, "thread2");
  check(thread1Specialization != nullptr && thread2Specialization != nullptr,
        "template did not specialize both context identifiers");
  const ResourceTypeCheck *thread1Begin =
      findTypeCheck(*thread1Specialization, "begin");
  const ResourceTypeCheck *thread2Begin =
      findTypeCheck(*thread2Specialization, "begin");
  check(thread1Begin != nullptr && !thread1Begin->isValid() &&
            thread2Begin != nullptr && thread2Begin->isValid(),
        "grant checks did not distinguish same-type context identifiers");
}

void emitsNonFatalContextTemplateChecks() {
  const std::string source = R"(
context Gpu::Exact grants {
  copy<Scope<Warp>>,
  gpu_await,
  defer<Remote::Context>
} exact;

context Gpu::WrongGeneric grants {
  copy<Scope<Block>>
} wrong_generic;

context Host::None grants {} none;

resource Slot {
  state Ready();

  transition use<C: Context>(
    @cx: C;
    self: own Ready
  ) -> Ready
  !{not_checked}
  where C allows {copy<Scope<Warp>>};

  await transition complete<C: Context>(
    @cx: C;
    self: own Ready
  ) -> Ready
  !{also_not_checked}
  where C allows {gpu_await};
}
)";

  const Module graph =
      metreon::graphir::lowerModule(parse(source), "templates.mtr");
  const ResourceContextTemplate *resourceTemplate =
      findResourceTemplate(graph, "Slot", "C");
  check(resourceTemplate != nullptr,
        "resource context template was not lowered");
  check(graph.contexts().size() == 4,
        "expected three declared contexts and one external context");
  check(resourceTemplate->specializations().size() == 3,
        "template must map only locally declared contexts");

  const ContextTemplateSpecialization *exact =
      findSpecialization(*resourceTemplate, "exact");
  const ContextTemplateSpecialization *wrongGeneric =
      findSpecialization(*resourceTemplate, "wrong_generic");
  const ContextTemplateSpecialization *none =
      findSpecialization(*resourceTemplate, "none");
  check(exact != nullptr && wrongGeneric != nullptr && none != nullptr,
        "template omitted a declared context mapping");

  const ResourceTypeCheck *exactUse = findTypeCheck(*exact, "use");
  const ResourceTypeCheck *exactComplete =
      findTypeCheck(*exact, "complete");
  const ResourceTypeCheck *wrongUse =
      findTypeCheck(*wrongGeneric, "use");
  const ResourceTypeCheck *wrongComplete =
      findTypeCheck(*wrongGeneric, "complete");
  check(exactUse != nullptr && exactUse->isValid() &&
            exactComplete != nullptr && exactComplete->isValid(),
        "matching where capabilities should validate independently of effects");
  check(wrongUse != nullptr && !wrongUse->isValid() &&
            wrongUse->missingCapabilities.size() == 1 &&
            wrongUse->missingCapabilities.front() ==
                "copy<Scope<Warp>>",
        "generic capability arguments were not matched exactly");
  check(wrongComplete != nullptr && !wrongComplete->isValid(),
        "missing plain capability should produce an invalid result");

  const std::string printed = graph.print();
  check(printed.find("graphir.template_section") != std::string::npos &&
            printed.find("graphir.context_mapping \"C\" -> #ctx0") !=
                std::string::npos &&
            printed.find("result = \"invalid\"") != std::string::npos &&
            printed.find("context #ctx1 identifier `wrong_generic` of type "
                         "`Gpu::WrongGeneric` does not grant") !=
                std::string::npos &&
            printed.find("missing = \"{copy<Scope<Warp>>}\"") !=
                std::string::npos,
        "textual GraphIR omitted non-fatal template validation diagnostics");
  check(printed.find("graphir.context_mapping \"C\" -> #ctx3") ==
            std::string::npos,
        "external context metadata became a template candidate");
}

void parsesContextDeclarations() {
  const metreon::ast::Module module = parse(contextSource);
  check(module.contexts.size() == 4, "expected four context declarations");

  const metreon::ast::ContextDeclaration &process = module.contexts[0];
  const metreon::ast::ContextDeclaration &softIrq = module.contexts[1];
  const metreon::ast::ContextDeclaration &hardIrq = module.contexts[2];
  const metreon::ast::ContextDeclaration &callback = module.contexts[3];
  check(process.identifier == "process_ctx" && process.grants.size() == 4 &&
            process.grants[0].capability.str() == "allocate" &&
            process.grants[1].capability.str() == "sleep" &&
            process.grants[2].capability.str() == "block" &&
            process.grants[3].capability.str() == "host_io",
        "Host::Process does not match tmp/example1.mtr");
  check(softIrq.identifier == "softirq_ctx" && softIrq.grants.size() == 3 &&
            softIrq.grants[2].capability.str() == "defer<Host::Process>",
        "Host::SoftIrq does not match tmp/example1.mtr");
  check(hardIrq.name.str() == "Host::HardIrq", "lost qualified context name");
  check(hardIrq.identifier == "harirq_ctx", "lost context identifier");
  check(hardIrq.parameters.size() == 1, "lost HardIrq context variable");
  check(hardIrq.parameters[0].name == "I", "wrong context variable name");
  check(hardIrq.parameters[0].constraint.str() == "Irq",
        "wrong context variable constraint");
  check(hardIrq.grants.size() == 4, "wrong HardIrq grant count");
  check(hardIrq.grants[2].capability.str() == "irq_ack<I>",
        "lost parameterized grant");
  check(callback.identifier == "host_callback_ctx" &&
            callback.parameters.size() == 1 && callback.grants.size() == 2 &&
            callback.grants[1].capability.str() == "defer<Host::Process>",
        "Host::Callback does not match tmp/example1.mtr");

  const metreon::ast::Module empty =
      parse("context Host::Callback<S: StreamId> grants {} callback;");
  check(empty.contexts.front().grants.empty(),
        "empty grant set should be accepted");
}

void parsesPlatformContextsAndLowersMetadata() {
  const std::string source = R"(
context Gpu::Thread<D, U> grants {
} gpu_thread;

context Gpu::PersistentWorker<D, Uniform<Block>> grants {
  gpu_persistent_work
} persistent_worker;

context Gpu::PersistentWorker<D, Uniform<Cluster>> grants {
  gpu_persistent_work
} cluster_worker;

context Transport::TxQueue<N: Nic, Q: Queue> grants {
  net_dma_submit,
  net_completion,
  transport_cancel
} transport_ctx;
)";

  const metreon::ast::Module module = parse(source);
  check(module.contexts.size() == 4,
        "expected GPU thread, worker specializations, and transport context");

  const metreon::ast::ContextDeclaration &gpuThread = module.contexts[0];
  const metreon::ast::ContextDeclaration &blockWorker = module.contexts[1];
  const metreon::ast::ContextDeclaration &clusterWorker = module.contexts[2];
  const metreon::ast::ContextDeclaration &transport = module.contexts[3];
  check(gpuThread.name.str() == "Gpu::Thread" &&
            gpuThread.parameters.empty() && gpuThread.arguments.size() == 2 &&
            gpuThread.arguments[0].str() == "D" &&
            gpuThread.arguments[1].str() == "U" &&
            gpuThread.typeName() == "Gpu::Thread<D, U>" &&
            gpuThread.grants.empty() && gpuThread.identifier == "gpu_thread",
        "lost concrete Gpu::Thread context arguments or identifier");
  check(blockWorker.name.str() == "Gpu::PersistentWorker" &&
            blockWorker.parameters.empty() &&
            blockWorker.arguments.size() == 2 &&
            blockWorker.arguments[0].str() == "D" &&
            blockWorker.arguments[1].str() == "Uniform<Block>" &&
            blockWorker.typeName() ==
                "Gpu::PersistentWorker<D, Uniform<Block>>",
        "lost concrete nested PersistentWorker arguments");
  check(clusterWorker.arguments.size() == 2 &&
            clusterWorker.arguments[1].str() == "Uniform<Cluster>" &&
            clusterWorker.typeName() ==
                "Gpu::PersistentWorker<D, Uniform<Cluster>>",
        "Uniform<Cluster> was not accepted as a context argument");
  check(transport.typeName() == "Transport::TxQueue" &&
            transport.arguments.empty() && transport.parameters.size() == 2 &&
            transport.parameters[0].name == "N" &&
            transport.parameters[0].constraint.str() == "Nic" &&
            transport.parameters[1].name == "Q" &&
            transport.parameters[1].constraint.str() == "Queue" &&
            transport.grants.size() == 3,
        "lost Transport::TxQueue parameters or grants");

  const Module graph =
      metreon::graphir::lowerContexts(module, "platform-contexts.mtr");
  const ContextMetadataRef gpuThreadMetadata =
      findContext(graph, "Gpu::Thread<D, U>", ContextResolution::Declared,
                  "gpu_thread");
  const ContextMetadataRef blockMetadata = findContext(
      graph, "Gpu::PersistentWorker<D, Uniform<Block>>",
      ContextResolution::Declared, "persistent_worker");
  const ContextMetadataRef clusterMetadata = findContext(
      graph, "Gpu::PersistentWorker<D, Uniform<Cluster>>",
      ContextResolution::Declared, "cluster_worker");
  const ContextMetadataRef transportMetadata = findContext(
      graph, "Transport::TxQueue", ContextResolution::Declared,
      "transport_ctx");
  check(gpuThreadMetadata && blockMetadata && clusterMetadata &&
            transportMetadata && gpuThreadMetadata->genericArity() == 2 &&
            blockMetadata->genericArity() == 2 &&
            clusterMetadata->genericArity() == 2 &&
            transportMetadata->genericArity() == 2,
        "platform contexts did not receive distinct GraphIR metadata");

  const Node *persistentGrant =
      findNode(graph, NodeKind::Grant, "gpu_persistent_work",
               "Gpu::PersistentWorker<D, Uniform<Block>>");
  const Node *transportSubmit = findNode(
      graph, NodeKind::Grant, "net_dma_submit", "Transport::TxQueue");
  const Node *transportCompletion = findNode(
      graph, NodeKind::Grant, "net_completion", "Transport::TxQueue");
  const Node *transportCancel = findNode(
      graph, NodeKind::Grant, "transport_cancel", "Transport::TxQueue");
  check(persistentGrant != nullptr && transportSubmit != nullptr &&
            transportCompletion != nullptr && transportCancel != nullptr &&
            transportSubmit->context->hasSameIdentity(*transportMetadata),
        "platform context grants were not attached to their metadata");

  const std::string printed = graph.print();
  check(printed.find("graphir.context_metadata \"Gpu::Thread<D, U>\"") !=
                std::string::npos &&
            printed.find(
                "graphir.context_metadata \"Gpu::PersistentWorker<D, "
                "Uniform<Block>>\"") != std::string::npos &&
            printed.find(
                "graphir.context_metadata \"Transport::TxQueue\"") !=
                std::string::npos &&
            printed.find(
                "graphir.context_identifier \"transport_ctx\" -> #ctx3") !=
                std::string::npos,
        "textual GraphIR omitted platform context assignments");
}

void resolvesConcreteContextTypes() {
  const std::string source = R"(
context Gpu::PersistentWorker<D, Uniform<Block>> grants {
  gpu_persistent_work
} persistent_worker;

context Host::Dispatcher grants {
  defer<Gpu::PersistentWorker<D, Uniform<Block>>>
} dispatcher;
)";
  const Module graph =
      metreon::graphir::lowerContexts(parse(source), "concrete-target.mtr");
  const ContextMetadataRef worker = findContext(
      graph, "Gpu::PersistentWorker<D, Uniform<Block>>",
      ContextResolution::Declared, "persistent_worker");
  const Node *defer = findNode(
      graph, NodeKind::Grant,
      "defer<Gpu::PersistentWorker<D, Uniform<Block>>>", "Host::Dispatcher");
  check(worker && defer != nullptr &&
            hasContextEdge(graph, defer->id, worker, EdgeKind::Transition),
        "concrete context type did not resolve to its declared metadata");
  check(findContext(graph, "Gpu::PersistentWorker<D, Uniform<Block>>",
                    ContextResolution::External) == nullptr,
        "concrete context target was duplicated as external metadata");
}

void lowersContextsAsOpaqueMetadata() {
  const Module graph = metreon::graphir::lowerContexts(parse(contextSource),
                                                        "contexts.mtr");

  const ContextMetadataRef process = findContext(graph, "Host::Process");
  const ContextMetadataRef softIrq = findContext(graph, "Host::SoftIrq");
  const ContextMetadataRef hardIrq = findContext(graph, "Host::HardIrq");
  const ContextMetadataRef callback = findContext(graph, "Host::Callback");
  const Node *irqVariable =
      findNode(graph, NodeKind::ContextVariable, "I", "Host::HardIrq");
  const Node *streamVariable =
      findNode(graph, NodeKind::ContextVariable, "S", "Host::Callback");
  const Node *irqAck =
      findNode(graph, NodeKind::Grant, "irq_ack<I>", "Host::HardIrq");
  const Node *hardIrqDefer = findNode(graph, NodeKind::Grant,
                                     "defer<Host::SoftIrq>",
                                     "Host::HardIrq");
  const Node *callbackDefer = findNode(graph, NodeKind::Grant,
                                      "defer<Host::Process>",
                                      "Host::Callback");

  check(process && softIrq && hardIrq && callback,
        "missing context metadata");
  check(process->identifier() == "process_ctx" &&
            softIrq->identifier() == "softirq_ctx" &&
            hardIrq->identifier() == "harirq_ctx" &&
            callback->identifier() == "host_callback_ctx",
        "context metadata lost user-facing identifiers");
  check(graph.contexts().size() == 4,
        "declared contexts must exist only in the metadata table");
  check(graph.nodes().size() == 15,
        "context declarations must not consume graph node IDs");
  check(irqVariable != nullptr && streamVariable != nullptr,
        "missing context-variable node");
  check(irqAck != nullptr && hardIrqDefer != nullptr &&
            callbackDefer != nullptr,
        "missing grant node");

  check(irqVariable->context->hasSameIdentity(*hardIrq),
        "HardIrq variable lost its context metadata");
  check(streamVariable->context->hasSameIdentity(*callback),
        "Callback variable lost its context metadata");
  check(irqAck->context->hasSameIdentity(*hardIrq),
        "irq_ack<I> lost its context metadata");
  check(hasNodeEdge(graph, irqAck->id, irqVariable->id, EdgeKind::Argument,
                    "0"),
        "irq_ack<I> must reference I through argument edge 0");
  check(hasContextEdge(graph, hardIrqDefer->id, softIrq,
                       EdgeKind::Transition),
        "HardIrq defer must target SoftIrq metadata");
  check(hasContextEdge(graph, callbackDefer->id, process,
                       EdgeKind::Transition),
        "Callback defer must target Process metadata");

  for (std::size_t left = 0; left < graph.contexts().size(); ++left) {
    for (std::size_t right = left + 1; right < graph.contexts().size();
         ++right) {
      check(!graph.contexts()[left]->hasSameIdentity(*graph.contexts()[right]),
            "context keys must be unique within a module");
    }
  }

  check(hardIrq->permits(ContextUse::MetadataAttachment),
        "metadata attachment must remain legal");
  check(!hardIrq->isRuntimeMaterializable(),
        "context keys must not have a runtime representation");
  const std::vector<ContextUse> forbiddenUses = {
      ContextUse::RuntimeStore, ContextUse::Transmute, ContextUse::Cast,
      ContextUse::CrossThreadSend, ContextUse::RuntimeCapture};
  for (const ContextUse use : forbiddenUses) {
    check(!hardIrq->permits(use), "context policy admitted a runtime use");
    expectLogicError("forbids", [&] { graph.requireContextUse(hardIrq, use); });
  }

  const std::string firstPrint = graph.print();
  const std::string secondPrint = graph.print();
  check(firstPrint == secondPrint, "GraphIR printing must be deterministic");
  const Module rebuiltGraph = metreon::graphir::lowerContexts(
      parse(contextSource), "contexts.mtr");
  check(firstPrint == rebuiltGraph.print(),
        "opaque keys must preserve reproducible textual GraphIR");
  check(firstPrint.find("graphir.context_metadata \"Host::HardIrq\"") !=
            std::string::npos,
        "textual GraphIR omitted HardIrq metadata");
  check(firstPrint.find("identifier = \"harirq_ctx\"") !=
            std::string::npos &&
            firstPrint.find(
                "graphir.context_identifier \"harirq_ctx\" -> #ctx2") !=
                std::string::npos,
        "textual GraphIR omitted the context identifier mapping");
  check(firstPrint.find("!graphir.context_key<") != std::string::npos,
        "textual GraphIR omitted the opaque compiler key");
  check(firstPrint.find("runtime_materializable = \"false\"") !=
            std::string::npos &&
            firstPrint.find("runtime_capturable = \"false\"") !=
                std::string::npos &&
            firstPrint.find("sendable = \"false\"") != std::string::npos &&
            firstPrint.find("storable = \"false\"") != std::string::npos &&
            firstPrint.find("transmutable = \"false\"") !=
                std::string::npos &&
            firstPrint.find("castable = \"false\"") != std::string::npos,
        "textual GraphIR omitted context-use restrictions");
  check(firstPrint.find(" = graphir.context \"") == std::string::npos,
        "a context was still lowered as a graph node");
  check(firstPrint.find("graphir.external_context") == std::string::npos,
        "an external context was still lowered as a graph node");
  check(firstPrint.find("context(#ctx") != std::string::npos,
        "nodes must reference their context metadata");
  check(firstPrint.find("loc(\"contexts.mtr\":") != std::string::npos,
        "textual GraphIR omitted source provenance");
}

void enforcesContextKeyProvenance() {
  Module first = metreon::graphir::lowerContexts(
      parse("context Host::Process grants { atomic } process;"), "first.mtr");
  Module second = metreon::graphir::lowerContexts(
      parse("context Host::Process grants { atomic } process;"), "second.mtr");
  const ContextMetadataRef firstContext =
      findContext(first, "Host::Process");
  const ContextMetadataRef secondContext =
      findContext(second, "Host::Process");

  check(firstContext && secondContext, "missing provenance test contexts");
  check(!firstContext->hasSameIdentity(*secondContext),
        "separate modules must mint different context keys");

  Module moved = std::move(first);
  const ContextMetadataRef movedContext = findContext(moved, "Host::Process");
  check(movedContext && movedContext->hasSameIdentity(*firstContext),
        "moving a module must preserve its existing context keys");
  const ContextMetadataRef movedFromContext = first.addContextMetadata(
      "Host::MovedFrom", "moved_from", 0, ContextResolution::Declared,
      metreon::SourceLocation{});
  check(!movedFromContext->hasSameIdentity(*movedContext),
        "a moved-from module must not mint a duplicate context key");

  expectLogicError("not minted by this module", [&] {
    second.addNode(NodeKind::Grant, "forged", {}, firstContext,
                   metreon::SourceLocation{});
  });
}

void preservesEmptyContextsAsMetadata() {
  const Module graph = metreon::graphir::lowerContexts(
      parse("context Host::Idle grants {} idle;"), "empty.mtr");
  check(graph.contexts().size() == 1,
        "empty context declaration metadata was lost");
  check(graph.nodes().empty(),
        "empty context declaration must not create an anchor node");
  check(graph.print().find("graphir.context_metadata \"Host::Idle\"") !=
            std::string::npos,
        "empty context metadata was omitted from textual GraphIR");
}

void preservesNestedVariableReferences() {
  const std::string source = R"(
/* nested generic argument */
context Gpu::Thread<D: Device, U: Uniformity> grants {
  gpu_barrier<Scope<U>>, // trailing grant comma is accepted
} thread;
)";
  const Module graph =
      metreon::graphir::lowerContexts(parse(source), "nested.mtr");
  const Node *grant = findNode(graph, NodeKind::Grant,
                               "gpu_barrier<Scope<U>>", "Gpu::Thread");
  const Node *uniformity =
      findNode(graph, NodeKind::ContextVariable, "U", "Gpu::Thread");
  check(grant != nullptr && uniformity != nullptr,
        "nested generic nodes were not lowered");
  check(hasNodeEdge(graph, grant->id, uniformity->id, EdgeKind::Argument,
                    "0.0"),
        "nested context-variable reference must retain argument path 0.0");
}

void preservesExternalTransitionTargets() {
  const std::string source = R"(
context Host::HardIrq<I: Irq> grants {
  defer<Host::SoftIrq>
} hard_irq;
)";
  const Module graph =
      metreon::graphir::lowerContexts(parse(source), "partial.mtr");
  const Node *grant = findNode(graph, NodeKind::Grant,
                               "defer<Host::SoftIrq>", "Host::HardIrq");
  const ContextMetadataRef target = findContext(
      graph, "Host::SoftIrq", ContextResolution::External);
  check(grant != nullptr && target,
        "unresolved defer target must remain explicit metadata");
  check(hasContextEdge(graph, grant->id, target, EdgeKind::Transition),
        "unresolved defer target metadata must remain connected");
  check(graph.print().find("resolution = \"external\"") != std::string::npos,
        "external context resolution was not printed");

  const std::string genericSource =
      "context Host::Any<C: Context> grants { defer<C> } any;";
  const Module genericGraph = metreon::graphir::lowerContexts(
      parse(genericSource), "generic-transition.mtr");
  const Node *genericGrant =
      findNode(genericGraph, NodeKind::Grant, "defer<C>", "Host::Any");
  const Node *genericTarget = findNode(genericGraph, NodeKind::ContextVariable,
                                       "C", "Host::Any");
  check(genericGrant != nullptr && genericTarget != nullptr,
        "generic defer target nodes were not lowered");
  check(hasNodeEdge(genericGraph, genericGrant->id, genericTarget->id,
                    EdgeKind::Transition),
        "defer<C> must transition through its context-variable node");
}

void resolvesRepeatedContextTypesByIdentifier() {
  const std::string source =
      "context Host::Process grants {} process1; "
      "context Host::Process grants {} process2; "
      "context Host::Source grants { defer<process2> } source;";
  const Module graph =
      metreon::graphir::lowerModule(parse(source), "context-identifiers.mtr");
  const ContextMetadataRef process1 = findContext(
      graph, "Host::Process", ContextResolution::Declared, "process1");
  const ContextMetadataRef process2 = findContext(
      graph, "Host::Process", ContextResolution::Declared, "process2");
  const Node *defer =
      findNode(graph, NodeKind::Grant, "defer<process2>", "Host::Source");
  check(process1 && process2 && defer != nullptr,
        "identifier-target context graph was not lowered");
  check(hasContextEdge(graph, defer->id, process2, EdgeKind::Transition) &&
            !hasContextEdge(graph, defer->id, process1, EdgeKind::Transition),
        "context identifier did not select the intended same-type target");
}

void diagnosesInvalidInput() {
  expectDiagnostic("parse.expected_context_identifier", [] {
    static_cast<void>(parse("context Host::Process grants {};"));
  });

  expectDiagnostic("sema.duplicate_context_identifier", [] {
    const auto module = parse(
        "context Host::Process grants {} duplicate; "
        "context Gpu::Thread grants {} duplicate;");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-identifier.mtr"));
  });

  expectDiagnostic("sema.duplicate_context_variable", [] {
    const auto module = parse(
        "context Gpu::Thread<D: Device, D: Device> grants {} thread;");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-variable.mtr"));
  });

  expectDiagnostic("sema.duplicate_grant", [] {
    const auto module =
        parse("context Host::Process grants { atomic, atomic } process;");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "duplicate-grant.mtr"));
  });

  expectDiagnostic("sema.defer_arity", [] {
    const auto module =
        parse("context Host::Process grants { defer } process;");
    static_cast<void>(
        metreon::graphir::lowerContexts(module, "bad-defer.mtr"));
  });

  expectDiagnostic("parse.expected_context", [] {
    static_cast<void>(parse("fn acknowledge() {}"));
  });

  expectDiagnostic("lex.unexpected_character", [] {
    static_cast<void>(
        parse("context Host::Process grants { $ } process;"));
  });

  expectDiagnostic("lex.unterminated_block_comment", [] {
    static_cast<void>(parse("/* never closed"));
  });

  expectDiagnostic("parse.unexpected_token", [] {
    static_cast<void>(parse("resource Slot<S> {}"));
  });

  expectDiagnostic("sema.unknown_accumulator_state", [] {
    const auto module =
        parse("resource Slot<S: AddressSpace> { "
              "accumulator Missing(device); }");
    static_cast<void>(
        metreon::graphir::lowerModule(module, "unknown-accumulator-state.mtr"));
  });

  expectDiagnostic("sema.accumulator_before_state", [] {
    const auto module =
        parse("resource Slot<S: AddressSpace> { "
              "accumulator Ready(device); state Ready(); }");
    static_cast<void>(
        metreon::graphir::lowerModule(module, "early-accumulator.mtr"));
  });

  expectDiagnostic("sema.duplicate_state_accumulator", [] {
    const auto module =
        parse("resource Slot<S: AddressSpace> { state Ready(); "
              "accumulator Ready(device); accumulator Ready(cluster); }");
    static_cast<void>(metreon::graphir::lowerModule(
        module, "duplicate-accumulator.mtr"));
  });

  expectDiagnostic("sema.invalid_accumulator_scope", [] {
    const auto module =
        parse("resource Slot<S: AddressSpace> { state Ready(); "
              "accumulator Ready(grid); }");
    static_cast<void>(
        metreon::graphir::lowerModule(module, "bad-accumulator-scope.mtr"));
  });

  expectDiagnostic("sema.unknown_transition_target", [] {
    const auto module = parse(R"(
resource Slot {
  state Vacant();
  transition begin(self: own Vacant) -> Missing;
}
)");
    static_cast<void>(
        metreon::graphir::lowerModule(module, "unknown-state.mtr"));
  });

  expectDiagnostic("sema.ambiguous_context_target", [] {
    const auto module = parse(
        "context Host::Process grants {} process1; "
        "context Host::Process grants {} process2; "
        "context Host::Source grants { defer<Host::Process> } source;");
    static_cast<void>(
        metreon::graphir::lowerModule(module, "ambiguous-context.mtr"));
  });
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"tokenizes resource declarations", tokenizesResourceDeclarations},
      {"tokenizes accumulator declarations", tokenizesAccumulatorDeclarations},
      {"parses resource declarations", parsesResourceDeclarations},
      {"parses and lowers accumulators", parsesAndLowersAccumulators},
      {"accepts all accumulator scopes", acceptsAllAccumulatorScopes},
      {"lowers resources as separate graphs", lowersResourcesAsSeparateGraphs},
      {"emits non-fatal context template checks",
       emitsNonFatalContextTemplateChecks},
      {"parses context declarations", parsesContextDeclarations},
      {"parses platform contexts and lowers metadata",
       parsesPlatformContextsAndLowersMetadata},
      {"resolves concrete context types", resolvesConcreteContextTypes},
      {"lowers contexts as opaque metadata", lowersContextsAsOpaqueMetadata},
      {"enforces context key provenance", enforcesContextKeyProvenance},
      {"preserves empty context metadata", preservesEmptyContextsAsMetadata},
      {"preserves nested variable references", preservesNestedVariableReferences},
      {"preserves external transition targets", preservesExternalTransitionTargets},
      {"resolves repeated context types by identifier",
       resolvesRepeatedContextTypesByIdentifier},
      {"diagnoses invalid input", diagnosesInvalidInput},
  };

  std::size_t failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}
