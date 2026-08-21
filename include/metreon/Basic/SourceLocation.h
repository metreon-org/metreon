#pragma once

#include <cstddef>

namespace metreon {

struct SourceLocation {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

} // namespace metreon
