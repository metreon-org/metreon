# Contributing to Metreon

Thank you for your interest in contributing to Metreon. Contributions of all
sizes are welcome, including bug fixes, tests, documentation, tooling, and new
features. This guide establishes the general standards for contributing. 

## Getting started

You will need:

- CMake 3.20 or newer;
- a compiler with C++20 support; and
- CTest, which is normally installed with CMake.

Configure an out-of-source build from the repository root:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Build artifacts should remain inside the build directory and must not be
committed.

## Contribution workflow

Contributions should be developed in a personal fork rather than directly in
the original repository.

1. Fork the repository using the hosting platform.
2. Clone your fork:

   ```sh
   git clone <your-fork-url>
   cd <repository-directory>
   ```

3. Create a focused local branch for the change:

   ```sh
   git switch --create <topic-branch>
   ```

4. Make and verify the change on the topic branch. Keep unrelated work in
   separate branches and commits.
5. Stage and commit the completed change:

   ```sh
   git add <changed-files>
   git commit
   ```

6. Push the topic branch to your fork:

   ```sh
   git push origin <topic-branch>
   ```

7. Open a pull request from the topic branch in your fork to the appropriate
   branch in the original repository. Follow the issue, project, or maintainer
   guidance when selecting that branch; do not assume every pull request should
   target `main`.
8. Respond to review feedback by updating the same topic branch unless a
   maintainer requests a different approach.

Contributors must not create additional pull requests solely to promote an
accepted change into `main`. Merging accepted pull requests and synchronizing
branches with `main` is the responsibility of the maintainers.

## Testing

Run the complete test suite before submitting a change:

```sh
ctest --test-dir build --output-on-failure
```

Tests should be focused, deterministic, and independent of execution order.
Every behavioral change should include appropriate test coverage. In general:

- add a regression test for every bug fix;
- cover both accepted and rejected input when adding validation;
- test observable behavior instead of private implementation details;
- keep test inputs as small as possible while preserving their purpose; and
- give failures enough context to make them easy to diagnose.

Do not weaken or remove an existing test merely to make a change pass. If an
existing expectation is no longer correct, update it and explain why the
behavior changed.

## Coding style

Follow the style of the surrounding code. Consistency within the codebase is
more important than personal preference.

The build treats warnings as errors. New code must compile cleanly with the
warning settings configured by the project. 

Comments should explain intent, invariants, or non-obvious tradeoffs. Avoid
comments that merely repeat the code. Use `TODO` comments only when they state a
specific remaining task and provide enough context for another contributor to
understand it.

## Designing changes

Keep each contribution scoped to one coherent purpose. Before implementing a
large or architectural change, discuss the design and expected behavior with
the maintainers.

When changing an interface or data model:

- consider compatibility with existing users and tools;
- preserve information needed by later stages instead of reconstructing it;
- define ownership, error handling, and invariants explicitly;
- avoid coupling independent components unnecessarily; and
- document important design decisions and limitations.

## Submitting a change

Before requesting review:

1. Confirm that the pull request targets the appropriate repository branch.
2. Build the project from a clean configuration when practical.
3. Run the complete test suite.
4. Review your diff for unrelated changes and generated files.
5. Update tests and documentation as needed.
6. Write a clear summary of what changed and why.

Commit messages should be concise and written in the imperative mood. Separate
unrelated work into different commits or contributions.

A change description should include:

- the problem being solved;
- the approach taken;
- important design decisions or tradeoffs; and
- the commands used to verify the change.

If some verification could not be performed, state that explicitly rather than
implying that it passed.

## Review expectations

Be respectful and assume good intent. Reviews should focus on correctness,
clarity, maintainability, compatibility, and test coverage.

Authors are expected to respond to review feedback or explain why a suggested
change is not appropriate. Reviewers should distinguish required corrections
from optional suggestions.

## License

By contributing, you agree that your contribution is provided under the terms
of the repository's `LICENSE` file.
