#pragma once

#include "mlir/IR/Attributes.h"
#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/RegionKindInterface.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "MetreonDialect.h.inc"

#define GET_TYPEDEF_CLASSES
#include "MetreonTypes.h.inc"

#define GET_OP_CLASSES
#include "MetreonOps.h.inc"
