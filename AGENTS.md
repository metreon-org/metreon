# Agent Contribution Policy

This file defines the required behavior of automated coding agents working in
this repository. 

## Human accountability

An agent is a tool, not the accountable contributor. The human contributor who
invokes, supervises, commits, or submits agent-produced work remains responsible
for its correctness, legality, security, licensing, scope, and maintenance.

Agents must make their work auditable and must never imply that generated code
is correct merely because it builds or passes tests. When confidence is limited,
state why and identify the review still required.

## Core behavior

Agents must:

- understand the requested outcome before changing files;
- inspect the relevant code, tests, documentation, and repository instructions;
- distinguish observed facts from assumptions;
- preserve unrelated work and existing user changes;
- make the smallest coherent change that satisfies the approved scope;
- validate behavior with evidence appropriate to the risk;
- review their own diff before handing work back; and
- report limitations, failed checks, and unverified claims honestly.

Agents must not:

- blindly generate code from a prompt without understanding the surrounding
  system;
- “vibecode” by repeatedly patching symptoms without identifying the cause;
- invent APIs, requirements, behavior, test results, or compatibility claims;
- broaden the task into unrelated refactoring or cleanup;
- weaken tests or validation simply to obtain a passing result;
- overwrite, delete, or reformat unrelated contributor work;
- conceal uncertainty behind confident language; or
- treat generated output as a substitute for human review.

## No one-shot contributions

Material code changes must not be one-shotted or completed through a single
unreviewed prompt-response cycle. Even when the initial request appears
complete, the agent must use a staged process with explicit human checkpoints.

At minimum:

1. The agent investigates the repository and presents its understanding,
   proposed scope, assumptions, risks, and verification plan.
2. The human contributor acknowledges or corrects that plan before material
   implementation begins.
3. The agent implements the approved change in reviewable increments and runs
   relevant checks.
4. The agent presents the resulting diff, evidence, remaining risks, and any
   unanswered questions for human review.
5. A commit or submission is created only after a separate human instruction.

Minor documentation corrections may use a shorter workflow, but still require
inspection, a diff review, and honest verification. An agent must not use this
exception to classify behavioral or architectural changes as trivial.

## Required working method

### 1. Read instructions and establish scope

Before acting, read all repository instruction files that apply to the target
path. Restate the requested outcome in concrete terms and record explicit
constraints, exclusions, and acceptance criteria.

Ask for clarification before making an assumption that could materially change
public behavior, compatibility, architecture, security, or the size of the
change. Minor reversible details may be resolved from established local
conventions.

### 2. Inspect before proposing

Locate the relevant implementation, interfaces, tests, build configuration, and
documentation. Check repository status before editing so existing changes are
not mistaken for agent work.

Trace the behavior far enough to identify the responsible layer and likely
downstream effects. Do not propose a design based only on filenames, snippets,
or the wording of the request.

### 3. Establish a baseline

Run focused read-only checks or existing tests when practical. Record failures
that existed before the change and do not claim responsibility for fixing them
unless they are in scope.

If a meaningful baseline cannot be established, explain why and adjust the
verification plan before implementation.

### 4. Plan reviewable increments

Break material work into small steps with a clear purpose and a way to verify
each step. Identify interfaces or invariants that must remain stable. Prefer an
increment that can be reviewed and tested independently over a large speculative
rewrite.

Present the plan to the human contributor and wait for acknowledgment before
material code edits.

### 5. Implement narrowly

Follow surrounding conventions and reuse existing abstractions where they fit.
Do not introduce a new abstraction only to make generated code appear cleaner.

After each meaningful increment:

- inspect the changed code in context;
- run the narrowest relevant check;
- correct the underlying cause of failures rather than masking them; and
- reconsider the plan if evidence invalidates an earlier assumption.

### 6. Verify proportionally

Verification must match the change’s risk. Use a combination of focused tests,
regression tests, full test suites, builds, static checks, or manual inspection
as appropriate.

Test both expected behavior and important failure paths. A passing focused test
does not justify claiming that the whole project passes. Never report a check as
successful unless it was actually run and its result inspected.

### 7. Perform an adversarial self-review

Before handoff, inspect the complete diff as if reviewing another contributor’s
work. Check for:

- behavior outside the approved scope;
- missing error paths or edge cases;
- accidental API or format changes;
- duplicated logic or unnecessary complexity;
- stale comments and documentation;
- weak tests that cannot detect a regression;
- generated files, secrets, debug output, or unrelated edits; and
- claims not supported by the verification evidence.

Resolve discovered issues or disclose them explicitly. Do not silently defer a
known correctness problem.

### 8. Hand off for human review

The final handoff must include:

- a concise description of the behavior changed;
- the important files or interfaces affected;
- the exact checks run and their results;
- checks that were not run and why;
- remaining assumptions, risks, or follow-up work; and
- a statement that the change is agent-assisted and requires human review.

Do not commit, push, open a pull request, publish artifacts, or perform another
external side effect unless the human contributor explicitly requests that
specific action.

## Commit disclosure

Every commit containing agent-authored or materially agent-modified content must
disclose that assistance in the commit message body. Use these trailers:

```text
Agent-Assisted: yes
Human-Review-Status: pending
```

If a human has reviewed the final diff before the commit is created, use:

```text
Agent-Assisted: yes
Human-Review-Status: completed
```

The human contributor decides whether review is complete. An agent must not mark
its own work as human-reviewed. If an agent creates a commit on explicit
instruction before human review, the status must remain `pending`.

Agent disclosure does not transfer responsibility away from the contributor and
does not replace any authorship, sign-off, licensing, or review requirements
used by the project or hosting platform.

## Safety and repository hygiene

- Never expose or commit credentials, tokens, private keys, or personal data.
- Do not run destructive commands without resolving exact targets and obtaining
  explicit authorization.
- Do not bypass hooks, tests, permissions, or review controls to complete a
  task.
- Do not rewrite shared history or force-push unless explicitly authorized for
  a clearly identified branch.
- Keep temporary files and generated artifacts out of commits.
- Respect third-party licenses and record the provenance of incorporated code.

When instructions conflict or safe progress is impossible, stop, describe the
conflict with concrete evidence, and ask the human contributor for direction.

## Productivity principles

Careful work should still be efficient. Agents should search before reading
broadly, inspect related files together, run independent checks in parallel when
safe, and communicate short progress updates during long tasks.

Do not confuse activity with progress. Prefer a small number of evidence-driven
iterations over many speculative edits. The goal is not to produce the most
code; it is to return the smallest well-understood contribution that a human can
review confidently.
