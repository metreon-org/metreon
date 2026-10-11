# Protocol and pipeline frontend design

Approved in chat on 2026-10-11. Implement the structural language used by
`tmp/protocols_and_pipelines.mtr`, preserving the existing frontend and GraphIR.

## Scope and representation

Parse named protocols with roles, message-bearing states, named payload fields,
successor states, and final states. Parse named pipelines with input/output
parameters, scalar state properties, bounded channel edges, stages using the
existing callable body grammar, topology connections, and schedules containing
fused workers and indexed warp placements.

Protocol and pipeline declarations are typed AST objects. Properties hold either
typed literals or type references; connections and placements have structured
fields. This slice supports the concrete, nongeneric declarations in the saved
example. Tensor shapes, const generics, asynchronous channel operations, and GPU
execution remain outside this slice.

Emit `graphir.protocol_state`, `graphir.pipeline_state`, `graphir.pipeline_edge`,
`graphir.stage`, `graphir.topology`, and `graphir.schedule` definitions. Stage
bodies use existing parameter, variable, call, return, and CFG operations.
Each protocol/pipeline owns a `graphir.resource_graph` whose member references
have stable, module-local IDs and typed definition targets. `graphir.protocol`
and `graphir.pipeline` sections list those resource-graph references, with roles
and input/output parameters respectively. Protocol messages preserve role
direction, payloads, and successor references; topology and schedule records
preserve their resolved member references. Existing resource-state/transition
GraphIR remains compatible.

## Validation

Reject duplicate declarations/members, unknown roles/states/types, invalid final
states, duplicate payload fields and properties, invalid scalar initializers,
unknown channel protocols, invalid or zero capacities, unknown topology endpoints,
incorrect input/output directions, incompatible scalar/endpoint types, repeated
destination bindings, unknown workers/stages, duplicate fusion/placement, and
placements outside their worker's fused stages. Validate stage bodies with the
existing context, effects, ownership, lexical-scope, call, and return checks.
Resolve forward references before lowering and preserve source locations.

References use indices rather than addresses into growable vectors. Printing
validates targets; moving a module preserves definitions, references, and CFGs.
No resource/protocol safety claim implies runtime execution or deadlock freedom.

## Acceptance and constraints

- Compile the saved sample to `tmp/graphir/protocols_and_pipelines.graphir`.
- Preserve all semantic fields and stage operations without raw source capture.
- Add a tracked example and focused positive/negative tests, including CLI use.
- Build with existing C++20 warnings-as-errors settings; run the full CTest suite.
- Keep `TACHYON_MASTER_PLAN.md` unchanged. Do not commit, push, or publish.
- Implement in the current checkout on `codex/protocol-pipeline-frontend` so the
  requested ignored input and output remain available to the user.
