# Protocol and Pipeline Frontend Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Compile the approved minimal protocol/pipeline example to validated GraphIR with resource-graph member references.

**Architecture:** Add structured AST declarations and a dedicated composition parser/validator. Reuse callable validation and lowering for stages; add typed module-local definition references and protocol/pipeline sections to GraphIR.

**Tech Stack:** C++20, CMake, CTest; no new dependencies.

**Spec:** `docs/superpowers/specs/2026-10-11-protocol-pipeline-design.md`

## Global Constraints

- Keep `TACHYON_MASTER_PLAN.md` unchanged. Do not commit, push, or publish.
- Preserve existing context/resource/callable behavior and output conventions.
- This slice supports the concrete, nongeneric declarations in the saved example.
- Implement in the current checkout on `codex/protocol-pipeline-frontend`.

## Review Focus

- Multiple declarations with repeated local stage/state names must remain scoped.
- Forward references must resolve deterministically before lowering.
- Zero/overflow capacities and malformed placements must produce diagnostics.
- Scalar topology and endpoint protocol/state mismatches must be rejected.
- Module moves must preserve reference targets, source locations, and stage CFGs.

### Task 1: Acceptance tests and parsing

Files: `tests/CompositionTests.cpp`, `examples/protocols_and_pipelines.mtr`,
`CMakeLists.txt`, `include/metreon/AST/Context.h`,
`include/metreon/Lexer/Token.h`, `lib/Lexer/Lexer.cpp`,
`include/metreon/Parser/Parser.h`, `lib/Parser/Parser.cpp`,
`lib/Parser/CompositionParser.cpp`.

- [x] Write acceptance and invalid-input tests using the real parser/lowering.
- [x] Build the test and observe rejection at the unsupported protocol declaration.
- [x] Add typed protocol/pipeline/property/topology/schedule AST and punctuation.
- [x] Implement dedicated parser methods, reusing callable signatures and bodies.
- [x] Verify complete AST/source-location preservation and malformed-input errors.

### Task 2: Composition and stage validation

Files: `include/metreon/Sema/CompositionValidator.h`,
`lib/Sema/CompositionValidator.cpp`, `lib/Sema/CallableValidator.cpp`,
`include/metreon/Sema/CallableValidator.h`, `lib/GraphIR/Lowering.cpp`.

Interfaces: `validateValueType(module, type, allowVoid)` shares existing type
checks; `validateCompositions(module)` resolves structural constraints;
`validateCallables(module)` additionally analyzes pipeline-scoped stages.

- [x] Observe rejection tests failing with missing/incorrect diagnostics.
- [x] Validate role/state/type/property/topology/schedule contracts from the spec.
- [x] Register and validate scoped stages with normal calls/context/effects checks.
- [x] Verify unknown references, duplicate names, mismatches, and forward references.

### Task 3: GraphIR definitions and references

Files: `include/metreon/GraphIR/Graph.h`, `lib/GraphIR/Graph.cpp`,
`include/metreon/GraphIR/Composition.h`, `lib/GraphIR/Composition.cpp`,
`lib/GraphIR/Lowering.cpp`, `tests/CompositionTests.cpp`.

Interfaces: `DefinitionReference {kind, index}` resolves through the module;
`ResourceMemberReference {graph, member}` addresses a graph's reference node;
`lowerCompositions(sourceModule, module)` emits definitions and membership.

- [x] Observe acceptance/reference/CFG assertions failing before implementation.
- [x] Add stage graphs, composition definitions/records, resource references, and groups.
- [x] Lower definitions and resolved relations with qualified symbols and locations.
- [x] Print dedicated protocol/pipeline sections and stage CFGs; validate every target.
- [x] Verify deterministic output, multiple declarations, and move construction/assignment.

### Task 4: Integration and handoff

Files: `README.md`, `tmp/protocols_and_pipelines.mtr`, plan/progress records.

- [x] Run the focused composition tests and the full CTest suite.
- [x] Compile the requested input and inspect its generated GraphIR.
- [x] Update supported-syntax documentation and remove obsolete sample support comments.
- [x] Review the entire diff and obtain an independent code review if available.
- [x] Verify the master-plan hash, rerun checks after fixes, and report limitations.
