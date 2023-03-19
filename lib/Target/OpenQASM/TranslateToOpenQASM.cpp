/*************************************************************** -*- C++ -*- ***
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 *******************************************************************************/

#include "cudaq/Frontend/nvqpp/AttributeNames.h"
#include "cudaq/Optimizer/Dialect/QTX/QTXOps.h"
#include "cudaq/Target/Emitter.h"
#include "cudaq/Target/OpenQASM/OpenQASMEmitter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace cudaq;

//===----------------------------------------------------------------------===//
// Helper functions
//===----------------------------------------------------------------------===//

/// Translates QTX operation names into OpenQASM gate names
static LogicalResult translateOperatorName(qtx::OperatorInterface optor,
                                           StringRef &name) {
  StringRef qtxName = optor->getName().stripDialect();
  if (optor.getControls().size() == 0) {
    name = StringSwitch<StringRef>(qtxName).Case("r1", "cu1").Default(qtxName);
  } else if (optor.getControls().size() == 1) {
    name = StringSwitch<StringRef>(qtxName)
               .Case("h", "ch")
               .Case("x", "cx")
               .Case("y", "cy")
               .Case("z", "cz")
               .Case("r1", "cu1")
               .Case("rx", "crx")
               .Case("ry", "cry")
               .Case("rz", "crz")
               .Default("");
  } else if (optor.getControls().size() == 2) {
    name = StringSwitch<StringRef>(qtxName).Case("x", "ccx").Default("");
  }
  if (name.empty())
    return failure();
  return success();
}

static LogicalResult printParameters(Emitter &emitter, ValueRange parameters) {
  if (parameters.empty())
    return success();
  emitter.os << '(';
  auto isFailure = false;
  llvm::interleaveComma(parameters, emitter.os, [&](Value value) {
    auto parameter = getParameterValueAsDouble(value);
    if (!parameter.has_value()) {
      isFailure = true;
      return;
    }
    emitter.os << *parameter;
  });
  emitter.os << ')';
  // TODO: emit error here?
  return failure(isFailure);
}

static StringRef printClassicalAllocation(Emitter &emitter, Value bitOrVector) {
  auto name = emitter.createName();
  auto size = 1;
  if (auto vector = bitOrVector.getType().dyn_cast_or_null<VectorType>()) {
    assert(vector.hasStaticShape() && "vector must have a known size");
    size = vector.getNumElements();
  }

  emitter.os << llvm::formatv("creg {0}[{1}];\n", name, size);
  if (!bitOrVector.getType().isa<VectorType>())
    name.append("[0]");
  return emitter.getOrAssignName(bitOrVector, name);
}

//===----------------------------------------------------------------------===//
// Emitters functions
//===----------------------------------------------------------------------===//

static LogicalResult emitOperation(Emitter &emitter, Operation &op);

static LogicalResult emitEntryPoint(Emitter &emitter,
                                    qtx::CircuitOp circuitOp) {
  Emitter::Scope scope(emitter, /*isEntryPoint=*/true);
  for (Operation &op : circuitOp.getOps()) {
    if (failed(emitOperation(emitter, op)))
      return failure();
  }
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, ModuleOp moduleOp) {
  qtx::CircuitOp entryPoint = nullptr;
  // TODO: Improve header
  emitter.os << "// Code generated by NVIDIA's nvq++ compiler\n";
  emitter.os << "OPENQASM 2.0;\n\n";
  emitter.os << "include \"qelib1.inc\";\n\n";
  for (Operation &op : moduleOp) {
    if (op.hasAttr(cudaq::entryPointAttrName)) {
      if (entryPoint)
        return moduleOp.emitError("has multiple entrypoints");
      entryPoint = dyn_cast_or_null<qtx::CircuitOp>(op);
      continue;
    }
    if (failed(emitOperation(emitter, op)))
      return failure();
    emitter.os << '\n';
  }
  if (!entryPoint)
    return moduleOp.emitError("does not contain an entrypoint");
  return emitEntryPoint(emitter, entryPoint);
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::AllocaOp allocaOp) {
  Value wireOrArray = allocaOp.getWireOrArray();
  auto name = emitter.createName();
  auto size = 1;
  if (auto array =
          wireOrArray.getType().dyn_cast_or_null<qtx::WireArrayType>()) {
    size = array.getSize();
  }
  emitter.os << llvm::formatv("qreg {0}[{1}];\n", name, size);
  if (wireOrArray.getType().isa<qtx::WireType>())
    name.append("[0]");
  emitter.getOrAssignName(wireOrArray, name);
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::ApplyOp op) {
  if (op.getNumClassicalResults())
    return op.emitError("cannot return classical results");
  emitter.os << op.getCallee();
  if (op.getNumParameters()) {
    emitter.os << '(';
    llvm::interleaveComma(op.getParameters(), emitter.os, [&](auto param) {
      emitter.os << emitter.getOrAssignName(param);
    });
    emitter.os << ')';
  }
  emitter.os << ' ';
  llvm::interleaveComma(op.getTargets(), emitter.os, [&](auto target) {
    emitter.os << emitter.getOrAssignName(target);
  });
  emitter.mapValuesName(op.getTargets(), op.getNewTargets());
  emitter.os << ";\n";
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::CircuitOp circuitOp) {
  // Check if we can translate
  if (circuitOp.getClassicalResultTypes().size())
    return circuitOp.emitError("cannot return classical results");
  for (auto target : circuitOp.getTargets()) {
    if (isa<qtx::WireArrayType>(target.getType()))
      return circuitOp.emitError(
          "Cannot translate array arguments into OpenQASM 2.0");
  }

  Emitter::Scope scope(emitter);
  emitter.os << "gate " << circuitOp.getName();
  if (circuitOp.getNumParameters()) {
    emitter.os << '(';
    llvm::interleaveComma(circuitOp.getParameters(), emitter.os,
                          [&](auto param) {
                            auto name = emitter.createName("param");
                            emitter.getOrAssignName(param, name);
                            emitter.os << name;
                          });
    emitter.os << ')';
  }
  emitter.os << ' ';
  llvm::interleaveComma(circuitOp.getTargets(), emitter.os, [&](auto target) {
    auto name = emitter.createName("q");
    emitter.getOrAssignName(target, name);
    emitter.os << name;
  });
  emitter.os << " {\n";
  emitter.os.indent();
  for (Operation &op : circuitOp.getOps()) {
    if (failed(emitOperation(emitter, op)))
      return failure();
  }
  emitter.os.unindent();
  emitter.os << "}\n";
  return success();
}

static LogicalResult emitOperation(Emitter &emitter,
                                   qtx::OperatorInterface optor) {
  // TODO: Handle adjoint for T and S
  if (optor.isAdj())
    return optor.emitError("cannot convert adjoint operations to OpenQASM 2.0");

  StringRef name;
  if (failed(translateOperatorName(optor, name)))
    return optor.emitError("cannot convert operation to OpenQASM 2.0");
  emitter.os << name << ' ';

  if (failed(printParameters(emitter, optor.getParameters())))
    return optor.emitError("failed to emit parameters");

  if (!optor.getControls().empty()) {
    emitter.os << ' ';
    llvm::interleaveComma(optor.getControls(), emitter.os, [&](auto control) {
      emitter.os << emitter.getOrAssignName(control);
    });
    emitter.os << ',';
  }
  emitter.os << ' ';
  llvm::interleaveComma(optor.getTargets(), emitter.os, [&](auto target) {
    emitter.os << emitter.getOrAssignName(target);
  });
  emitter.os << ";\n";
  emitter.mapValuesName(optor.getTargets(), optor.getNewTargets());
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::MzOp op) {
  if (op.getTargets().size() > 1)
    return op.emitError(
        "cannot translate measurements with more than one target");
  auto bitsName = printClassicalAllocation(emitter, op.getBits());
  auto wireOrArray = op.getTargets()[0];
  emitter.os << "measure " << emitter.getOrAssignName(wireOrArray) << " -> "
             << bitsName << ";\n";
  emitter.mapValuesName(op.getTargets(), op.getNewTargets());
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::ResetOp op) {
  for (auto target : op.getTargets())
    emitter.os << "reset " << emitter.getOrAssignName(target) << ";";
  emitter.mapValuesName(op.getTargets(), op.getNewTargets());
  return success();
}

// Since OpenQASM uses memory semantics and can index quantum registers (which
// in QTX are represented by arrays) using `array[index]` syntax, there is not
// array operations to emit.  We just need to handle the correct name mapping
// for resulting values.

static LogicalResult emitOperation(Emitter &emitter, qtx::ArraySplitOp op) {
  auto arrayName = emitter.getOrAssignName(op.getArray());
  for (auto [i, wire] : llvm::enumerate(op.getWires())) {
    auto wireName = llvm::formatv("{0}[{1}]", arrayName, i);
    emitter.getOrAssignName(wire, wireName);
  }
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::ArrayBorrowOp op) {
  auto arrayName = emitter.getOrAssignName(op.getArray());
  for (auto [indexValue, wire] : llvm::zip(op.getIndices(), op.getWires())) {
    auto index = getIndexValueAsInt(indexValue);
    if (!index.has_value())
      return op.emitError("cannot translate runtime index to OpenQASM 2.0");
    auto wireName = llvm::formatv("{0}[{1}]", arrayName, *index);
    emitter.getOrAssignName(wire, wireName);
  }
  emitter.mapValuesName(op.getArray(), op.getNewArray());
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, qtx::ArrayYieldOp op) {
  emitter.mapValuesName(op.getArray(), op.getNewArray());
  return success();
}

static LogicalResult emitOperation(Emitter &emitter, Operation &op) {
  using namespace qtx;
  return llvm::TypeSwitch<Operation *, LogicalResult>(&op)
      .Case<ModuleOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<CircuitOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<ApplyOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<AllocaOp>([&](auto op) { return emitOperation(emitter, op); })
      // Arrays
      .Case<ArraySplitOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<ArrayBorrowOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<ArrayYieldOp>([&](auto op) { return emitOperation(emitter, op); })
      // Operators
      .Case<OperatorInterface>(
          [&](auto optor) { return emitOperation(emitter, optor); })
      // Measurements
      .Case<MzOp>([&](auto op) { return emitOperation(emitter, op); })
      .Case<ResetOp>([&](auto op) { return emitOperation(emitter, op); })
      // Ignore
      .Case<DeallocOp>([&](auto op) { return success(); })
      .Case<ReturnOp>([&](auto op) { return success(); })
      .Case<arith::ConstantOp>([&](auto op) { return success(); })
      .Default([&](Operation *) -> LogicalResult {
        if (op.getName().getDialectNamespace().equals("llvm"))
          return success();
        return op.emitOpError("unable to translate op to OpenQASM 2.0");
      });
}

LogicalResult cudaq::translateToOpenQASM(Operation *op, raw_ostream &os) {
  Emitter emitter(os);
  return emitOperation(emitter, *op);
}
