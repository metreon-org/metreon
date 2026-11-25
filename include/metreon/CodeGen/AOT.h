#pragma once

#include "metreon/GraphIR/Graph.h"
#include <string>

namespace metreon {
enum class GPUBackend { CUDA, AMDGPU };
struct AOTOptions {
  std::string output;
  std::string architecture = "sm_80";
  GPUBackend backend = GPUBackend::CUDA;
};
std::string linkAMDGPUCodeObject(const std::string &object);
// Emit and link a native program. This API never initializes a GPU or executes
// the generated program; GPU code is assembled and embedded ahead of time.
void emitExecutable(const graphir::Module &source, const AOTOptions &options);
} // namespace metreon
