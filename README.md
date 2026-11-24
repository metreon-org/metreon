# Metreon

Metreon is a statically typed domain-specific language for low-level GPU
pipelines. It aims to statically verify execution contexts, effects, ownership,
resource lifecycles, and asynchronous communication between pipeline stages.

## Build

### Requirements

- CMake 3.20 or newer.
- A C++20-compatible compiler.
- A build tool supported by CMake, such as Make or Ninja.

### Standard build

Run from the repository root:

```sh
cmake -S . -B build
cmake --build build --parallel
```

MLIR support is disabled by default. The build produces the `metreonc`
executable in the build directory.

### Build with MLIR support

Install matching LLVM and MLIR development packages, including the TableGen
tools. A C compiler is also required for this configuration. The integration
has been validated with LLVM/MLIR 18.1.3.

Replace the paths below with the CMake package directories in your installation:

```sh
cmake -S . -B build-mlir -DMETREON_ENABLE_MLIR=ON \
  -DMLIR_DIR=/path/to/llvm/lib/cmake/mlir \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build-mlir --parallel
```

This configuration also builds `metreon-opt` and the Metreon MLIR dialect.
