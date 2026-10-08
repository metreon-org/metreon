# metreon
Metreon is a statically typed Domain Specific Language (DSL) that aims to verify execution contexts, effects, ownership, resource lifecycles, and asynchronous protocol topology in low-level GPU pipelines

## Run the frontend examples

```sh
cmake -S . -B build
cmake --build build --parallel
./build/metreonc --emit=graphir examples/contexts.mtr
./build/metreonc --emit=graphir examples/resources.mtr
./build/metreonc --emit=graphir examples/accumulators.mtr
./build/metreonc --emit=graphir examples/kernel_variables.mtr
./build/metreonc --emit=graphir examples/procedures.mtr
cat tmp/graphir/procedures.graphir
```

The compiler saves GraphIR to `tmp/graphir/` relative to its current working
directory and prints only the saved path. For example, running from `compiler/`
produces `compiler/tmp/graphir/procedures.graphir`, while invoking
`compiler/build/metreonc` from the parent directory produces
`tmp/graphir/procedures.graphir` in that parent directory. The output location
does not depend on the source file or executable location. When running inside
this checkout, the existing `/tmp/` rule in `.gitignore` excludes the generated
artifacts; other working directories use their own ignore rules.

The default invocation and `--emit=graphir` both save a file. Reading from stdin
with `-` produces `stdin.graphir`. Recompiling the same input stem replaces its
previous output; inputs in different directories with the same stem share that
output filename. Source validation finishes before opening the output file, so
invalid programs do not replace a previous artifact. Directory or file-write
failures produce an error and a nonzero exit status.

The current compiler parses and validates `.mtr` context declarations and
resource state machines, kernels, and procedures, then emits textual GraphIR
with context metadata, resource graphs, and ordered callable bodies. Procedures
support typed parameters and return types, resource-transition calls, nested
blocks, and execution-context/effect requirements. See
[procedure syntax and validation](docs/procedures.md) for the supported forms
and limits. It does not yet generate or execute runtime or GPU code.

## Callable control-flow projection

Each module ends with a `graphir.cfg` section containing one CFG per kernel or
procedure. It projects the ordered callable operations into basic blocks, with
an entry block, explicit successor branches, and a return terminator on every
exit. Empty or falling-through `void` bodies receive an implicit return. A return
inside any nested lexical block exits the callable and creates no continuation.

Operations in the original bodies have stable, module-local identities such as
`id(#p0op1)`. A `graphir.cfg.op #p0op1` entry refers to that existing operation;
it is not another execution. Runtime value references retain their existing
`%p0vN` or `%k0vN` identities. CFG parameters refer only to runtime parameters;
context evidence remains metadata in the original callable signature.

Basic blocks carry `scopes(...)`, listing the original lexical block identities
from outermost to innermost. Unconditional branches mark entry to and exit from
these scopes. An importer can compare scope paths across an edge or return to
identify all exited scopes without treating a lexical block as a new callable.
Source locations are retained; synthetic scope branches use the lexical block's
location, and implicit returns use the callable's location.

For example, `procedure identity(x: i32) -> i32 { return x; }` includes this
projection (source locations omitted here):

```text
graphir.cfg {
  graphir.cfg.procedure @"identity" entry(^p0bb0) parameters(%p0v0) {
    graphir.cfg.block ^p0bb0 scopes() {
      graphir.cfg.return #p0op1 operands(%p0v0)
    }
  }
}
```

The projection supports the current straight-line body grammar and lexical
scopes. It does not add conditional branches, loops, barriers, an MLIR importer,
or a uniformity analysis. `buildControlFlow` exposes the same typed projection
through `metreon/GraphIR/ControlFlow.h` for future lowering.
