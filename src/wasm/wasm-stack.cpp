/*
 * Copyright 2019 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm-stack.h"
#include "ir/find_all.h"
#include "wasm-debug.h"

namespace wasm {

static Name IMPOSSIBLE_CONTINUE("impossible-continue");

template<class IO> void BinaryInstWriter<IO>::emitResultType(Type type) {
  if (type == Type::unreachable) {
    parent.writeType(Type::none);
  } else if (type.isTuple()) {
    writeValue<ValueWritten::Type>(
      parent.getTypeIndex(Signature(Type::none, type)));
  } else {
    parent.writeType(type);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitBlock(Block* curr) {
  breakStack.push_back(curr->name);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Block);
  emitResultType(curr->type);
}

template<class IO> void BinaryInstWriter<IO>::visitIf(If* curr) {
  // the binary format requires this; we have a block if we need one
  // TODO: optimize this in Stack IR (if child is a block, we may break to this
  // instead)
  breakStack.emplace_back(IMPOSSIBLE_CONTINUE);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::If);
  emitResultType(curr->type);
}

template<class IO> void BinaryInstWriter<IO>::emitIfElse(If* curr) {
  if (func && !sourceMap) {
    parent.writeExtraDebugLocation(curr, func, BinaryLocations::Else);
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Else);
}

template<class IO> void BinaryInstWriter<IO>::visitLoop(Loop* curr) {
  breakStack.push_back(curr->name);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Loop);
  emitResultType(curr->type);
}

template<class IO> void BinaryInstWriter<IO>::visitBreak(Break* curr) {
  writeValue<ValueWritten::ASTNode>(curr->condition ? BinaryConsts::BrIf
                                                    : BinaryConsts::Br);
  writeValue<ValueWritten::BreakIndex>(getBreakIndex(curr->name));
}

template<class IO> void BinaryInstWriter<IO>::visitSwitch(Switch* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::BrTable);
  writeValue<ValueWritten::SwitchTargets>(curr->targets.size());
  for (auto target : curr->targets) {
    writeValue<ValueWritten::BreakIndex>(getBreakIndex(target));
  }
  writeValue<ValueWritten::BreakIndex>(getBreakIndex(curr->default_));
}

template<class IO> void BinaryInstWriter<IO>::visitCall(Call* curr) {
  int8_t op =
    curr->isReturn ? BinaryConsts::RetCallFunction : BinaryConsts::CallFunction;
  writeValue<ValueWritten::ASTNode>(op);
  writeValue<ValueWritten::FunctionIndex>(
    parent.getFunctionIndex(curr->target));
}

template<class IO>
void BinaryInstWriter<IO>::visitCallIndirect(CallIndirect* curr) {
  Index tableIdx = parent.getTableIndex(curr->table);
  int8_t op =
    curr->isReturn ? BinaryConsts::RetCallIndirect : BinaryConsts::CallIndirect;
  writeValue<ValueWritten::ASTNode>(op);
  writeValue<ValueWritten::TypeIndex>(parent.getTypeIndex(curr->heapType));
  writeValue<ValueWritten::TableIndex>(tableIdx);
}

template<class IO> void BinaryInstWriter<IO>::visitLocalGet(LocalGet* curr) {
  size_t numValues = func->getLocalType(curr->index).size();
  for (Index i = 0; i < numValues; ++i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalGet);
    writeValue<ValueWritten::LocalIndex>(
      mappedLocals[std::make_pair(curr->index, i)]);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitLocalSet(LocalSet* curr) {
  size_t numValues = func->getLocalType(curr->index).size();
  for (Index i = numValues - 1; i >= 1; --i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalSet);
    writeValue<ValueWritten::LocalIndex>(
      mappedLocals[std::make_pair(curr->index, i)]);
  }
  if (!curr->isTee()) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalSet);
    writeValue<ValueWritten::LocalIndex>(
      mappedLocals[std::make_pair(curr->index, 0)]);
  } else {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalTee);
    writeValue<ValueWritten::LocalIndex>(
      mappedLocals[std::make_pair(curr->index, 0)]);
    for (Index i = 1; i < numValues; ++i) {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalGet);
      writeValue<ValueWritten::LocalIndex>(
        mappedLocals[std::make_pair(curr->index, i)]);
    }
  }
}

template<class IO> void BinaryInstWriter<IO>::visitGlobalGet(GlobalGet* curr) {
  // Emit a global.get for each element if this is a tuple global
  Index index = parent.getGlobalIndex(curr->name);
  size_t numValues = curr->type.size();
  for (Index i = 0; i < numValues; ++i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::GlobalGet);
    writeValue<ValueWritten::GlobalIndex>(index + i);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitGlobalSet(GlobalSet* curr) {
  // Emit a global.set for each element if this is a tuple global
  Index index = parent.getGlobalIndex(curr->name);
  size_t numValues = parent.getModule()->getGlobal(curr->name)->type.size();
  for (int i = numValues - 1; i >= 0; --i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::GlobalSet);
    writeValue<ValueWritten::GlobalIndex>(index + i);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitLoad(Load* curr) {
  if (!curr->isAtomic) {
    switch (curr->type.getBasic()) {
      case Type::i32: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(curr->signed_
                                                ? BinaryConsts::I32LoadMem8S
                                                : BinaryConsts::I32LoadMem8U);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(curr->signed_
                                                ? BinaryConsts::I32LoadMem16S
                                                : BinaryConsts::I32LoadMem16U);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32LoadMem);
            break;
          default:
            abort();
        }
        break;
      }
      case Type::i64: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(curr->signed_
                                                ? BinaryConsts::I64LoadMem8S
                                                : BinaryConsts::I64LoadMem8U);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(curr->signed_
                                                ? BinaryConsts::I64LoadMem16S
                                                : BinaryConsts::I64LoadMem16U);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(curr->signed_
                                                ? BinaryConsts::I64LoadMem32S
                                                : BinaryConsts::I64LoadMem32U);
            break;
          case 8:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64LoadMem);
            break;
          default:
            abort();
        }
        break;
      }
      case Type::f32:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::F32LoadMem);
        break;
      case Type::f64:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::F64LoadMem);
        break;
      case Type::v128:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load);
        break;
      case Type::unreachable:
        // the pointer is unreachable, so we are never reached; just don't emit
        // a load
        return;
      case Type::funcref:
      case Type::anyref:
      case Type::eqref:
      case Type::i31ref:
      case Type::dataref:
      case Type::none:
        WASM_UNREACHABLE("unexpected type");
    }
  } else {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
    switch (curr->type.getBasic()) {
      case Type::i32: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicLoad8U);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicLoad16U);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicLoad);
            break;
          default:
            WASM_UNREACHABLE("invalid load size");
        }
        break;
      }
      case Type::i64: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicLoad8U);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicLoad16U);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicLoad32U);
            break;
          case 8:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicLoad);
            break;
          default:
            WASM_UNREACHABLE("invalid load size");
        }
        break;
      }
      case Type::unreachable:
        return;
      default:
        WASM_UNREACHABLE("unexpected type");
    }
  }
  emitMemoryAccess(curr->align, curr->bytes, curr->offset);
}

template<class IO> void BinaryInstWriter<IO>::visitStore(Store* curr) {
  if (!curr->isAtomic) {
    switch (curr->valueType.getBasic()) {
      case Type::i32: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32StoreMem8);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32StoreMem16);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32StoreMem);
            break;
          default:
            abort();
        }
        break;
      }
      case Type::i64: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64StoreMem8);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64StoreMem16);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64StoreMem32);
            break;
          case 8:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64StoreMem);
            break;
          default:
            abort();
        }
        break;
      }
      case Type::f32:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::F32StoreMem);
        break;
      case Type::f64:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::F64StoreMem);
        break;
      case Type::v128:
        writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Store);
        break;
      case Type::funcref:
      case Type::anyref:
      case Type::eqref:
      case Type::i31ref:
      case Type::dataref:
      case Type::none:
      case Type::unreachable:
        WASM_UNREACHABLE("unexpected type");
    }
  } else {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
    switch (curr->valueType.getBasic()) {
      case Type::i32: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicStore8);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicStore16);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicStore);
            break;
          default:
            WASM_UNREACHABLE("invalid store size");
        }
        break;
      }
      case Type::i64: {
        switch (curr->bytes) {
          case 1:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicStore8);
            break;
          case 2:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicStore16);
            break;
          case 4:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicStore32);
            break;
          case 8:
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicStore);
            break;
          default:
            WASM_UNREACHABLE("invalid store size");
        }
        break;
      }
      default:
        WASM_UNREACHABLE("unexpected type");
    }
  }
  emitMemoryAccess(curr->align, curr->bytes, curr->offset);
}

template<class IO> void BinaryInstWriter<IO>::visitAtomicRMW(AtomicRMW* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);

#define CASE_FOR_OP(Op)                                                        \
  case RMW##Op:                                                                \
    switch (curr->type.getBasic()) {                                           \
      case Type::i32:                                                          \
        switch (curr->bytes) {                                                 \
          case 1:                                                              \
            writeValue<ValueWritten::ASTNode>(                                 \
              BinaryConsts::I32AtomicRMW##Op##8U);                             \
            break;                                                             \
          case 2:                                                              \
            writeValue<ValueWritten::ASTNode>(                                 \
              BinaryConsts::I32AtomicRMW##Op##16U);                            \
            break;                                                             \
          case 4:                                                              \
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicRMW##Op); \
            break;                                                             \
          default:                                                             \
            WASM_UNREACHABLE("invalid rmw size");                              \
        }                                                                      \
        break;                                                                 \
      case Type::i64:                                                          \
        switch (curr->bytes) {                                                 \
          case 1:                                                              \
            writeValue<ValueWritten::ASTNode>(                                 \
              BinaryConsts::I64AtomicRMW##Op##8U);                             \
            break;                                                             \
          case 2:                                                              \
            writeValue<ValueWritten::ASTNode>(                                 \
              BinaryConsts::I64AtomicRMW##Op##16U);                            \
            break;                                                             \
          case 4:                                                              \
            writeValue<ValueWritten::ASTNode>(                                 \
              BinaryConsts::I64AtomicRMW##Op##32U);                            \
            break;                                                             \
          case 8:                                                              \
            writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicRMW##Op); \
            break;                                                             \
          default:                                                             \
            WASM_UNREACHABLE("invalid rmw size");                              \
        }                                                                      \
        break;                                                                 \
      default:                                                                 \
        WASM_UNREACHABLE("unexpected type");                                   \
    }                                                                          \
    break

  switch (curr->op) {
    CASE_FOR_OP(Add);
    CASE_FOR_OP(Sub);
    CASE_FOR_OP(And);
    CASE_FOR_OP(Or);
    CASE_FOR_OP(Xor);
    CASE_FOR_OP(Xchg);
    default:
      WASM_UNREACHABLE("unexpected op");
  }
#undef CASE_FOR_OP

  emitMemoryAccess(curr->bytes, curr->bytes, curr->offset);
}

template<class IO>
void BinaryInstWriter<IO>::visitAtomicCmpxchg(AtomicCmpxchg* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
  switch (curr->type.getBasic()) {
    case Type::i32:
      switch (curr->bytes) {
        case 1:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicCmpxchg8U);
          break;
        case 2:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicCmpxchg16U);
          break;
        case 4:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicCmpxchg);
          break;
        default:
          WASM_UNREACHABLE("invalid size");
      }
      break;
    case Type::i64:
      switch (curr->bytes) {
        case 1:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicCmpxchg8U);
          break;
        case 2:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicCmpxchg16U);
          break;
        case 4:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicCmpxchg32U);
          break;
        case 8:
          writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicCmpxchg);
          break;
        default:
          WASM_UNREACHABLE("invalid size");
      }
      break;
    default:
      WASM_UNREACHABLE("unexpected type");
  }
  emitMemoryAccess(curr->bytes, curr->bytes, curr->offset);
}

template<class IO>
void BinaryInstWriter<IO>::visitAtomicWait(AtomicWait* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
  switch (curr->expectedType.getBasic()) {
    case Type::i32: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32AtomicWait);
      emitMemoryAccess(4, 4, curr->offset);
      break;
    }
    case Type::i64: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64AtomicWait);
      emitMemoryAccess(8, 8, curr->offset);
      break;
    }
    default:
      WASM_UNREACHABLE("unexpected type");
  }
}

template<class IO>
void BinaryInstWriter<IO>::visitAtomicNotify(AtomicNotify* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicNotify);
  emitMemoryAccess(4, 4, curr->offset);
}

template<class IO>
void BinaryInstWriter<IO>::visitAtomicFence(AtomicFence* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicPrefix);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::AtomicFence);
  writeValue<ValueWritten::AtomicFenceOrder>(curr->order);
}

template<class IO>
void BinaryInstWriter<IO>::visitSIMDExtract(SIMDExtract* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case ExtractLaneSVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16ExtractLaneS);
      break;
    case ExtractLaneUVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16ExtractLaneU);
      break;
    case ExtractLaneSVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtractLaneS);
      break;
    case ExtractLaneUVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtractLaneU);
      break;
    case ExtractLaneVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtractLane);
      break;
    case ExtractLaneVecI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtractLane);
      break;
    case ExtractLaneVecF32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4ExtractLane);
      break;
    case ExtractLaneVecF64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2ExtractLane);
      break;
  }
  writeValue<ValueWritten::SIMDIndex>(curr->index);
}

template<class IO>
void BinaryInstWriter<IO>::visitSIMDReplace(SIMDReplace* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case ReplaceLaneVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16ReplaceLane);
      break;
    case ReplaceLaneVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ReplaceLane);
      break;
    case ReplaceLaneVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ReplaceLane);
      break;
    case ReplaceLaneVecI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ReplaceLane);
      break;
    case ReplaceLaneVecF32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4ReplaceLane);
      break;
    case ReplaceLaneVecF64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2ReplaceLane);
      break;
  }
  assert(curr->index < 16);
  writeValue<ValueWritten::SIMDIndex>(curr->index);
}

template<class IO>
void BinaryInstWriter<IO>::visitSIMDShuffle(SIMDShuffle* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Shuffle);
  for (uint8_t m : curr->mask) {
    writeValue<ValueWritten::SIMDIndex>(m);
  }
}

template<class IO>
void BinaryInstWriter<IO>::visitSIMDTernary(SIMDTernary* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case Bitselect:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Bitselect);
      break;
    case LaneselectI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Laneselect);
      break;
    case LaneselectI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Laneselect);
      break;
    case LaneselectI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Laneselect);
      break;
    case LaneselectI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Laneselect);
      break;
    case RelaxedFmaVecF32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4RelaxedFma);
      break;
    case RelaxedFmsVecF32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4RelaxedFms);
      break;
    case RelaxedFmaVecF64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2RelaxedFma);
      break;
    case RelaxedFmsVecF64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2RelaxedFms);
      break;
    case DotI8x16I7x16AddSToVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4DotI8x16I7x16AddS);
      break;
  }
}

template<class IO> void BinaryInstWriter<IO>::visitSIMDShift(SIMDShift* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case ShlVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Shl);
      break;
    case ShrSVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16ShrS);
      break;
    case ShrUVecI8x16:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16ShrU);
      break;
    case ShlVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Shl);
      break;
    case ShrSVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ShrS);
      break;
    case ShrUVecI16x8:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ShrU);
      break;
    case ShlVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Shl);
      break;
    case ShrSVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ShrS);
      break;
    case ShrUVecI32x4:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ShrU);
      break;
    case ShlVecI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Shl);
      break;
    case ShrSVecI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ShrS);
      break;
    case ShrUVecI64x2:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ShrU);
      break;
  }
}

template<class IO> void BinaryInstWriter<IO>::visitSIMDLoad(SIMDLoad* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case Load8SplatVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load8Splat);
      break;
    case Load16SplatVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load16Splat);
      break;
    case Load32SplatVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load32Splat);
      break;
    case Load64SplatVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load64Splat);
      break;
    case Load8x8SVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load8x8S);
      break;
    case Load8x8UVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load8x8U);
      break;
    case Load16x4SVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load16x4S);
      break;
    case Load16x4UVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load16x4U);
      break;
    case Load32x2SVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load32x2S);
      break;
    case Load32x2UVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load32x2U);
      break;
    case Load32ZeroVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load32Zero);
      break;
    case Load64ZeroVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load64Zero);
      break;
  }
  assert(curr->align);
  emitMemoryAccess(curr->align, /*(unused) bytes=*/0, curr->offset);
}

template<class IO>
void BinaryInstWriter<IO>::visitSIMDLoadStoreLane(SIMDLoadStoreLane* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
  switch (curr->op) {
    case Load8LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load8Lane);
      break;
    case Load16LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load16Lane);
      break;
    case Load32LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load32Lane);
      break;
    case Load64LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Load64Lane);
      break;
    case Store8LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Store8Lane);
      break;
    case Store16LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Store16Lane);
      break;
    case Store32LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Store32Lane);
      break;
    case Store64LaneVec128:
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Store64Lane);
      break;
  }
  assert(curr->align);
  emitMemoryAccess(curr->align, /*(unused) bytes=*/0, curr->offset);
  writeValue<ValueWritten::SIMDIndex>(curr->index);
}

template<class IO>
void BinaryInstWriter<IO>::visitMemoryInit(MemoryInit* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::MemoryInit);
  writeValue<ValueWritten::MemorySegmentIndex>(curr->segment);
  writeValue<ValueWritten::MemoryIndex>(0);
}

template<class IO> void BinaryInstWriter<IO>::visitDataDrop(DataDrop* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::DataDrop);
  writeValue<ValueWritten::MemorySegmentIndex>(curr->segment);
}

template<class IO>
void BinaryInstWriter<IO>::visitMemoryCopy(MemoryCopy* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::MemoryCopy);
  writeValue<ValueWritten::MemoryIndex>(0);
  writeValue<ValueWritten::MemoryIndex>(0);
}

template<class IO>
void BinaryInstWriter<IO>::visitMemoryFill(MemoryFill* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::MemoryFill);
  writeValue<ValueWritten::MemoryIndex>(0);
}

template<class IO> void BinaryInstWriter<IO>::visitConst(Const* curr) {
  switch (curr->type.getBasic()) {
    case Type::i32: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Const);
      writeValue<ValueWritten::ConstS32>(curr->value.geti32());
      break;
    }
    case Type::i64: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Const);
      writeValue<ValueWritten::ConstS64>(curr->value.geti64());
      break;
    }
    case Type::f32: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Const);
      writeValue<ValueWritten::ConstF32>(curr->value.reinterpreti32());
      break;
    }
    case Type::f64: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Const);
      writeValue<ValueWritten::ConstF64>(curr->value.reinterpreti64());
      break;
    }
    case Type::v128: {
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Const);
      writeValue<ValueWritten::ConstV128>(curr->value);
      break;
    }
    case Type::funcref:
    case Type::anyref:
    case Type::eqref:
    case Type::i31ref:
    case Type::dataref:
    case Type::none:
    case Type::unreachable:
      WASM_UNREACHABLE("unexpected type");
  }
}

template<class IO> void BinaryInstWriter<IO>::visitUnary(Unary* curr) {
  switch (curr->op) {
    case ClzInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Clz);
      break;
    case CtzInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Ctz);
      break;
    case PopcntInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Popcnt);
      break;
    case EqZInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32EqZ);
      break;
    case ClzInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Clz);
      break;
    case CtzInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Ctz);
      break;
    case PopcntInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Popcnt);
      break;
    case EqZInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64EqZ);
      break;
    case NegFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Neg);
      break;
    case AbsFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Abs);
      break;
    case CeilFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Ceil);
      break;
    case FloorFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Floor);
      break;
    case TruncFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Trunc);
      break;
    case NearestFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32NearestInt);
      break;
    case SqrtFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Sqrt);
      break;
    case NegFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Neg);
      break;
    case AbsFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Abs);
      break;
    case CeilFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Ceil);
      break;
    case FloorFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Floor);
      break;
    case TruncFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Trunc);
      break;
    case NearestFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64NearestInt);
      break;
    case SqrtFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Sqrt);
      break;
    case ExtendSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64SExtendI32);
      break;
    case ExtendUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64UExtendI32);
      break;
    case WrapInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32WrapI64);
      break;
    case TruncUFloat32ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32UTruncF32);
      break;
    case TruncUFloat32ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64UTruncF32);
      break;
    case TruncSFloat32ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32STruncF32);
      break;
    case TruncSFloat32ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64STruncF32);
      break;
    case TruncUFloat64ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32UTruncF64);
      break;
    case TruncUFloat64ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64UTruncF64);
      break;
    case TruncSFloat64ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32STruncF64);
      break;
    case TruncSFloat64ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64STruncF64);
      break;
    case ConvertUInt32ToFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32UConvertI32);
      break;
    case ConvertUInt32ToFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64UConvertI32);
      break;
    case ConvertSInt32ToFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32SConvertI32);
      break;
    case ConvertSInt32ToFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64SConvertI32);
      break;
    case ConvertUInt64ToFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32UConvertI64);
      break;
    case ConvertUInt64ToFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64UConvertI64);
      break;
    case ConvertSInt64ToFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32SConvertI64);
      break;
    case ConvertSInt64ToFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64SConvertI64);
      break;
    case DemoteFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32DemoteI64);
      break;
    case PromoteFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64PromoteF32);
      break;
    case ReinterpretFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32ReinterpretF32);
      break;
    case ReinterpretFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ReinterpretF64);
      break;
    case ReinterpretInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32ReinterpretI32);
      break;
    case ReinterpretInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64ReinterpretI64);
      break;
    case ExtendS8Int32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32ExtendS8);
      break;
    case ExtendS16Int32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32ExtendS16);
      break;
    case ExtendS8Int64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ExtendS8);
      break;
    case ExtendS16Int64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ExtendS16);
      break;
    case ExtendS32Int64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ExtendS32);
      break;
    case TruncSatSFloat32ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32STruncSatF32);
      break;
    case TruncSatUFloat32ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32UTruncSatF32);
      break;
    case TruncSatSFloat64ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32STruncSatF64);
      break;
    case TruncSatUFloat64ToInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32UTruncSatF64);
      break;
    case TruncSatSFloat32ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64STruncSatF32);
      break;
    case TruncSatUFloat32ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64UTruncSatF32);
      break;
    case TruncSatSFloat64ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64STruncSatF64);
      break;
    case TruncSatUFloat64ToInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64UTruncSatF64);
      break;
    case SplatVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Splat);
      break;
    case SplatVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Splat);
      break;
    case SplatVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Splat);
      break;
    case SplatVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Splat);
      break;
    case SplatVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Splat);
      break;
    case SplatVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Splat);
      break;
    case NotVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Not);
      break;
    case AnyTrueVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128AnyTrue);
      break;
    case AbsVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Abs);
      break;
    case NegVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Neg);
      break;
    case AllTrueVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16AllTrue);
      break;
    case BitmaskVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Bitmask);
      break;
    case PopcntVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Popcnt);
      break;
    case AbsVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Abs);
      break;
    case NegVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Neg);
      break;
    case AllTrueVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8AllTrue);
      break;
    case BitmaskVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Bitmask);
      break;
    case AbsVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Abs);
      break;
    case NegVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Neg);
      break;
    case AllTrueVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4AllTrue);
      break;
    case BitmaskVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Bitmask);
      break;
    case AbsVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Abs);
      break;
    case NegVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Neg);
      break;
    case AllTrueVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2AllTrue);
      break;
    case BitmaskVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Bitmask);
      break;
    case AbsVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Abs);
      break;
    case NegVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Neg);
      break;
    case SqrtVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Sqrt);
      break;
    case CeilVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Ceil);
      break;
    case FloorVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Floor);
      break;
    case TruncVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Trunc);
      break;
    case NearestVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Nearest);
      break;
    case AbsVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Abs);
      break;
    case NegVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Neg);
      break;
    case SqrtVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Sqrt);
      break;
    case CeilVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Ceil);
      break;
    case FloorVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Floor);
      break;
    case TruncVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Trunc);
      break;
    case NearestVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Nearest);
      break;
    case ExtAddPairwiseSVecI8x16ToI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I16x8ExtaddPairwiseI8x16S);
      break;
    case ExtAddPairwiseUVecI8x16ToI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I16x8ExtaddPairwiseI8x16U);
      break;
    case ExtAddPairwiseSVecI16x8ToI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4ExtaddPairwiseI16x8S);
      break;
    case ExtAddPairwiseUVecI16x8ToI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4ExtaddPairwiseI16x8U);
      break;
    case TruncSatSVecF32x4ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4TruncSatF32x4S);
      break;
    case TruncSatUVecF32x4ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4TruncSatF32x4U);
      break;
    case ConvertSVecI32x4ToVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4ConvertI32x4S);
      break;
    case ConvertUVecI32x4ToVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4ConvertI32x4U);
      break;
    case ExtendLowSVecI8x16ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtendLowI8x16S);
      break;
    case ExtendHighSVecI8x16ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtendHighI8x16S);
      break;
    case ExtendLowUVecI8x16ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtendLowI8x16U);
      break;
    case ExtendHighUVecI8x16ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtendHighI8x16U);
      break;
    case ExtendLowSVecI16x8ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtendLowI16x8S);
      break;
    case ExtendHighSVecI16x8ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtendHighI16x8S);
      break;
    case ExtendLowUVecI16x8ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtendLowI16x8U);
      break;
    case ExtendHighUVecI16x8ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtendHighI16x8U);
      break;
    case ExtendLowSVecI32x4ToVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtendLowI32x4S);
      break;
    case ExtendHighSVecI32x4ToVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtendHighI32x4S);
      break;
    case ExtendLowUVecI32x4ToVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtendLowI32x4U);
      break;
    case ExtendHighUVecI32x4ToVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtendHighI32x4U);
      break;
    case ConvertLowSVecI32x4ToVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2ConvertLowI32x4S);
      break;
    case ConvertLowUVecI32x4ToVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2ConvertLowI32x4U);
      break;
    case TruncSatZeroSVecF64x2ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4TruncSatF64x2SZero);
      break;
    case TruncSatZeroUVecF64x2ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4TruncSatF64x2UZero);
      break;
    case DemoteZeroVecF64x2ToVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4DemoteF64x2Zero);
      break;
    case PromoteLowVecF32x4ToVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2PromoteLowF32x4);
      break;
    case RelaxedTruncSVecF32x4ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4RelaxedTruncF32x4S);
      break;
    case RelaxedTruncUVecF32x4ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4RelaxedTruncF32x4U);
      break;
    case RelaxedTruncZeroSVecF64x2ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4RelaxedTruncF64x2SZero);
      break;
    case RelaxedTruncZeroUVecF64x2ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::I32x4RelaxedTruncF64x2UZero);
      break;
    case InvalidUnary:
      WASM_UNREACHABLE("invalid unary op");
  }
}

template<class IO> void BinaryInstWriter<IO>::visitBinary(Binary* curr) {
  switch (curr->op) {
    case AddInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Add);
      break;
    case SubInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Sub);
      break;
    case MulInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Mul);
      break;
    case DivSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32DivS);
      break;
    case DivUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32DivU);
      break;
    case RemSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32RemS);
      break;
    case RemUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32RemU);
      break;
    case AndInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32And);
      break;
    case OrInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Or);
      break;
    case XorInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Xor);
      break;
    case ShlInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Shl);
      break;
    case ShrUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32ShrU);
      break;
    case ShrSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32ShrS);
      break;
    case RotLInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32RotL);
      break;
    case RotRInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32RotR);
      break;
    case EqInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Eq);
      break;
    case NeInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32Ne);
      break;
    case LtSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32LtS);
      break;
    case LtUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32LtU);
      break;
    case LeSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32LeS);
      break;
    case LeUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32LeU);
      break;
    case GtSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32GtS);
      break;
    case GtUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32GtU);
      break;
    case GeSInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32GeS);
      break;
    case GeUInt32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I32GeU);
      break;

    case AddInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Add);
      break;
    case SubInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Sub);
      break;
    case MulInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Mul);
      break;
    case DivSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64DivS);
      break;
    case DivUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64DivU);
      break;
    case RemSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64RemS);
      break;
    case RemUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64RemU);
      break;
    case AndInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64And);
      break;
    case OrInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Or);
      break;
    case XorInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Xor);
      break;
    case ShlInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Shl);
      break;
    case ShrUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ShrU);
      break;
    case ShrSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64ShrS);
      break;
    case RotLInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64RotL);
      break;
    case RotRInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64RotR);
      break;
    case EqInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Eq);
      break;
    case NeInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64Ne);
      break;
    case LtSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64LtS);
      break;
    case LtUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64LtU);
      break;
    case LeSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64LeS);
      break;
    case LeUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64LeU);
      break;
    case GtSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64GtS);
      break;
    case GtUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64GtU);
      break;
    case GeSInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64GeS);
      break;
    case GeUInt64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::I64GeU);
      break;

    case AddFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Add);
      break;
    case SubFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Sub);
      break;
    case MulFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Mul);
      break;
    case DivFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Div);
      break;
    case CopySignFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32CopySign);
      break;
    case MinFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Min);
      break;
    case MaxFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Max);
      break;
    case EqFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Eq);
      break;
    case NeFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Ne);
      break;
    case LtFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Lt);
      break;
    case LeFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Le);
      break;
    case GtFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Gt);
      break;
    case GeFloat32:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F32Ge);
      break;

    case AddFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Add);
      break;
    case SubFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Sub);
      break;
    case MulFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Mul);
      break;
    case DivFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Div);
      break;
    case CopySignFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64CopySign);
      break;
    case MinFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Min);
      break;
    case MaxFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Max);
      break;
    case EqFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Eq);
      break;
    case NeFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Ne);
      break;
    case LtFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Lt);
      break;
    case LeFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Le);
      break;
    case GtFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Gt);
      break;
    case GeFloat64:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::F64Ge);
      break;

    case EqVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Eq);
      break;
    case NeVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Ne);
      break;
    case LtSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16LtS);
      break;
    case LtUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16LtU);
      break;
    case GtSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16GtS);
      break;
    case GtUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16GtU);
      break;
    case LeSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16LeS);
      break;
    case LeUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16LeU);
      break;
    case GeSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16GeS);
      break;
    case GeUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16GeU);
      break;
    case EqVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Eq);
      break;
    case NeVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Ne);
      break;
    case LtSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8LtS);
      break;
    case LtUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8LtU);
      break;
    case GtSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8GtS);
      break;
    case GtUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8GtU);
      break;
    case LeSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8LeS);
      break;
    case LeUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8LeU);
      break;
    case GeSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8GeS);
      break;
    case GeUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8GeU);
      break;
    case EqVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Eq);
      break;
    case NeVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Ne);
      break;
    case LtSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4LtS);
      break;
    case LtUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4LtU);
      break;
    case GtSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4GtS);
      break;
    case GtUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4GtU);
      break;
    case LeSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4LeS);
      break;
    case LeUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4LeU);
      break;
    case GeSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4GeS);
      break;
    case GeUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4GeU);
      break;
    case EqVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Eq);
      break;
    case NeVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Ne);
      break;
    case LtSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2LtS);
      break;
    case GtSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2GtS);
      break;
    case LeSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2LeS);
      break;
    case GeSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2GeS);
      break;
    case EqVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Eq);
      break;
    case NeVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Ne);
      break;
    case LtVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Lt);
      break;
    case GtVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Gt);
      break;
    case LeVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Le);
      break;
    case GeVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Ge);
      break;
    case EqVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Eq);
      break;
    case NeVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Ne);
      break;
    case LtVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Lt);
      break;
    case GtVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Gt);
      break;
    case LeVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Le);
      break;
    case GeVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Ge);
      break;
    case AndVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128And);
      break;
    case OrVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Or);
      break;
    case XorVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Xor);
      break;
    case AndNotVec128:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::V128Andnot);
      break;
    case AddVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Add);
      break;
    case AddSatSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16AddSatS);
      break;
    case AddSatUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16AddSatU);
      break;
    case SubVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Sub);
      break;
    case SubSatSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16SubSatS);
      break;
    case SubSatUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16SubSatU);
      break;
    case MinSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16MinS);
      break;
    case MinUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16MinU);
      break;
    case MaxSVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16MaxS);
      break;
    case MaxUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16MaxU);
      break;
    case AvgrUVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16AvgrU);
      break;
    case AddVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Add);
      break;
    case AddSatSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8AddSatS);
      break;
    case AddSatUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8AddSatU);
      break;
    case SubVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Sub);
      break;
    case SubSatSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8SubSatS);
      break;
    case SubSatUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8SubSatU);
      break;
    case MulVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Mul);
      break;
    case MinSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8MinS);
      break;
    case MinUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8MinU);
      break;
    case MaxSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8MaxS);
      break;
    case MaxUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8MaxU);
      break;
    case AvgrUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8AvgrU);
      break;
    case Q15MulrSatSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8Q15MulrSatS);
      break;
    case ExtMulLowSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtmulLowI8x16S);
      break;
    case ExtMulHighSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtmulHighI8x16S);
      break;
    case ExtMulLowUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtmulLowI8x16U);
      break;
    case ExtMulHighUVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8ExtmulHighI8x16U);
      break;
    case AddVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Add);
      break;
    case SubVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Sub);
      break;
    case MulVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4Mul);
      break;
    case MinSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4MinS);
      break;
    case MinUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4MinU);
      break;
    case MaxSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4MaxS);
      break;
    case MaxUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4MaxU);
      break;
    case DotSVecI16x8ToVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4DotI16x8S);
      break;
    case ExtMulLowSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtmulLowI16x8S);
      break;
    case ExtMulHighSVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtmulHighI16x8S);
      break;
    case ExtMulLowUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtmulLowI16x8U);
      break;
    case ExtMulHighUVecI32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I32x4ExtmulHighI16x8U);
      break;
    case AddVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Add);
      break;
    case SubVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Sub);
      break;
    case MulVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2Mul);
      break;
    case ExtMulLowSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtmulLowI32x4S);
      break;
    case ExtMulHighSVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtmulHighI32x4S);
      break;
    case ExtMulLowUVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtmulLowI32x4U);
      break;
    case ExtMulHighUVecI64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I64x2ExtmulHighI32x4U);
      break;

    case AddVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Add);
      break;
    case SubVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Sub);
      break;
    case MulVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Mul);
      break;
    case DivVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Div);
      break;
    case MinVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Min);
      break;
    case MaxVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Max);
      break;
    case PMinVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Pmin);
      break;
    case PMaxVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4Pmax);
      break;
    case AddVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Add);
      break;
    case SubVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Sub);
      break;
    case MulVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Mul);
      break;
    case DivVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Div);
      break;
    case MinVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Min);
      break;
    case MaxVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Max);
      break;
    case PMinVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Pmin);
      break;
    case PMaxVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2Pmax);
      break;

    case NarrowSVecI16x8ToVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16NarrowI16x8S);
      break;
    case NarrowUVecI16x8ToVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16NarrowI16x8U);
      break;
    case NarrowSVecI32x4ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8NarrowI32x4S);
      break;
    case NarrowUVecI32x4ToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8NarrowI32x4U);
      break;

    case SwizzleVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16Swizzle);
      break;

    case RelaxedSwizzleVecI8x16:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I8x16RelaxedSwizzle);
      break;
    case RelaxedMinVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4RelaxedMin);
      break;
    case RelaxedMaxVecF32x4:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F32x4RelaxedMax);
      break;
    case RelaxedMinVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2RelaxedMin);
      break;
    case RelaxedMaxVecF64x2:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::F64x2RelaxedMax);
      break;
    case RelaxedQ15MulrSVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8RelaxedQ15MulrS);
      break;
    case DotI8x16I7x16SToVecI16x8:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::SIMDPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::I16x8DotI8x16I7x16S);
      break;

    case InvalidBinary:
      WASM_UNREACHABLE("invalid binary op");
  }
}

template<class IO> void BinaryInstWriter<IO>::visitSelect(Select* curr) {
  if (curr->type.isRef()) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::SelectWithType);
    writeValue<ValueWritten::NumSelectTypes>(curr->type.size());
    for (size_t i = 0; i < curr->type.size(); i++) {
      parent.writeType(curr->type != Type::unreachable ? curr->type
                                                       : Type::none);
    }
  } else {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::Select);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitReturn(Return* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Return);
}

template<class IO>
void BinaryInstWriter<IO>::visitMemorySize(MemorySize* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MemorySize);
  writeValue<ValueWritten::MemorySizeFlags>(0);
}

template<class IO>
void BinaryInstWriter<IO>::visitMemoryGrow(MemoryGrow* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MemoryGrow);
  // Reserved flags field
  writeValue<ValueWritten::MemoryGrowFlags>(0);
}

template<class IO> void BinaryInstWriter<IO>::visitRefNull(RefNull* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::RefNull);
  parent.writeHeapType(curr->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitRefIs(RefIs* curr) {
  switch (curr->op) {
    case RefIsNull:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::RefIsNull);
      break;
    case RefIsFunc:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode>(BinaryConsts::RefIsFunc);
      break;
    case RefIsData:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode>(BinaryConsts::RefIsData);
      break;
    case RefIsI31:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode>(BinaryConsts::RefIsI31);
      break;
    default:
      WASM_UNREACHABLE("unimplemented ref.is_*");
  }
}

template<class IO> void BinaryInstWriter<IO>::visitRefFunc(RefFunc* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::RefFunc);
  writeValue<ValueWritten::FunctionIndex>(parent.getFunctionIndex(curr->func));
}

template<class IO> void BinaryInstWriter<IO>::visitRefEq(RefEq* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::RefEq);
}

template<class IO> void BinaryInstWriter<IO>::visitTableGet(TableGet* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::TableGet);
  writeValue<ValueWritten::TableIndex>(parent.getTableIndex(curr->table));
}

template<class IO> void BinaryInstWriter<IO>::visitTableSet(TableSet* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::TableSet);
  writeValue<ValueWritten::TableIndex>(parent.getTableIndex(curr->table));
}

template<class IO> void BinaryInstWriter<IO>::visitTableSize(TableSize* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::TableSize);
  writeValue<ValueWritten::TableIndex>(parent.getTableIndex(curr->table));
}

template<class IO> void BinaryInstWriter<IO>::visitTableGrow(TableGrow* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::MiscPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::TableGrow);
  writeValue<ValueWritten::TableIndex>(parent.getTableIndex(curr->table));
}

template<class IO> void BinaryInstWriter<IO>::visitTry(Try* curr) {
  breakStack.push_back(curr->name);
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Try);
  emitResultType(curr->type);
}

template<class IO> void BinaryInstWriter<IO>::emitCatch(Try* curr, Index i) {
  if (func && !sourceMap) {
    parent.writeExtraDebugLocation(curr, func, i);
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Catch);
  writeValue<ValueWritten::TagIndex>(parent.getTagIndex(curr->catchTags[i]));
}

template<class IO> void BinaryInstWriter<IO>::emitCatchAll(Try* curr) {
  if (func && !sourceMap) {
    parent.writeExtraDebugLocation(curr, func, curr->catchBodies.size());
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::CatchAll);
}

template<class IO> void BinaryInstWriter<IO>::emitDelegate(Try* curr) {
  // The delegate ends the scope in effect, and pops the try's name. Note that
  // the getBreakIndex is intentionally after that pop, as the delegate cannot
  // target its own try.
  assert(!breakStack.empty());
  breakStack.pop_back();
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Delegate);
  writeValue<ValueWritten::BreakIndex>(getBreakIndex(curr->delegateTarget));
}

template<class IO> void BinaryInstWriter<IO>::visitThrow(Throw* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Throw);
  writeValue<ValueWritten::TagIndex>(parent.getTagIndex(curr->tag));
}

template<class IO> void BinaryInstWriter<IO>::visitRethrow(Rethrow* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Rethrow);
  writeValue<ValueWritten::BreakIndex>(getBreakIndex(curr->target));
}

template<class IO> void BinaryInstWriter<IO>::visitNop(Nop* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Nop);
}

template<class IO>
void BinaryInstWriter<IO>::visitUnreachable(Unreachable* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Unreachable);
}

template<class IO> void BinaryInstWriter<IO>::visitDrop(Drop* curr) {
  size_t numValues = curr->value->type.size();
  for (size_t i = 0; i < numValues; i++) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::Drop);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitPop(Pop* curr) {
  // Turns into nothing in the binary format
}

template<class IO> void BinaryInstWriter<IO>::visitTupleMake(TupleMake* curr) {
  // Turns into nothing in the binary format
}

template<class IO>
void BinaryInstWriter<IO>::visitTupleExtract(TupleExtract* curr) {
  size_t numVals = curr->tuple->type.size();
  // Drop all values after the one we want
  for (size_t i = curr->index + 1; i < numVals; ++i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::Drop);
  }
  // If the extracted value is the only one left, we're done
  if (curr->index == 0) {
    return;
  }
  // Otherwise, save it to a scratch local, drop the others, then retrieve it
  assert(scratchLocals.find(curr->type) != scratchLocals.end());
  auto scratch = scratchLocals[curr->type];
  writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalSet);
  writeValue<ValueWritten::ScratchLocalIndex>(scratch);
  for (size_t i = 0; i < curr->index; ++i) {
    writeValue<ValueWritten::ASTNode>(BinaryConsts::Drop);
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::LocalGet);
  writeValue<ValueWritten::ScratchLocalIndex>(scratch);
}

template<class IO> void BinaryInstWriter<IO>::visitI31New(I31New* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::I31New);
}

template<class IO> void BinaryInstWriter<IO>::visitI31Get(I31Get* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(curr->signed_ ? BinaryConsts::I31GetS
                                                    : BinaryConsts::I31GetU);
}

template<class IO> void BinaryInstWriter<IO>::visitCallRef(CallRef* curr) {
  writeValue<ValueWritten::ASTNode>(curr->isReturn ? BinaryConsts::RetCallRef
                                                   : BinaryConsts::CallRef);
}

template<class IO> void BinaryInstWriter<IO>::visitRefTest(RefTest* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  if (curr->rtt) {
    writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefTest);
  } else {
    writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefTestStatic);
    parent.writeIndexedHeapType(curr->intendedType);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitRefCast(RefCast* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  if (curr->rtt) {
    writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefCast);
  } else {
    if (curr->safety == RefCast::Unsafe) {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefCastNopStatic);
    } else {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefCastStatic);
    }
    parent.writeIndexedHeapType(curr->intendedType);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitBrOn(BrOn* curr) {
  switch (curr->op) {
    case BrOnNull:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::BrOnNull);
      break;
    case BrOnNonNull:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::BrOnNonNull);
      break;
    case BrOnCast:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      if (curr->rtt) {
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnCast);
      } else {
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnCastStatic);
      }
      break;
    case BrOnCastFail:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      if (curr->rtt) {
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnCastFail);
      } else {
        writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnCastStaticFail);
      }
      break;
    case BrOnFunc:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnFunc);
      break;
    case BrOnNonFunc:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnNonFunc);
      break;
    case BrOnData:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnData);
      break;
    case BrOnNonData:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnNonData);
      break;
    case BrOnI31:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnI31);
      break;
    case BrOnNonI31:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::BrOnNonI31);
      break;
    default:
      WASM_UNREACHABLE("invalid br_on_*");
  }
  writeValue<ValueWritten::BreakIndex>(getBreakIndex(curr->name));
  if ((curr->op == BrOnCast || curr->op == BrOnCastFail) && !curr->rtt) {
    parent.writeIndexedHeapType(curr->intendedType);
  }
}

template<class IO> void BinaryInstWriter<IO>::visitRttCanon(RttCanon* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::RttCanon);
  parent.writeIndexedHeapType(curr->type.getRtt().heapType);
}

template<class IO> void BinaryInstWriter<IO>::visitRttSub(RttSub* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(curr->fresh ? BinaryConsts::RttFreshSub
                                                  : BinaryConsts::RttSub);
  parent.writeIndexedHeapType(curr->type.getRtt().heapType);
}

template<class IO> void BinaryInstWriter<IO>::visitStructNew(StructNew* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  if (curr->rtt) {
    if (curr->isWithDefault()) {
      writeValue<ValueWritten::ASTNode32>(
        BinaryConsts::StructNewDefaultWithRtt);
    } else {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::StructNewWithRtt);
    }
  } else {
    if (curr->isWithDefault()) {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::StructNewDefault);
    } else {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::StructNew);
    }
  }
  parent.writeIndexedHeapType(curr->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitStructGet(StructGet* curr) {
  const auto& heapType = curr->ref->type.getHeapType();
  const auto& field = heapType.getStruct().fields[curr->index];
  int8_t op;
  if (field.type != Type::i32 || field.packedType == Field::not_packed) {
    op = BinaryConsts::StructGet;
  } else if (curr->signed_) {
    op = BinaryConsts::StructGetS;
  } else {
    op = BinaryConsts::StructGetU;
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(op);
  parent.writeIndexedHeapType(heapType);
  writeValue<ValueWritten::StructFieldIndex>(curr->index);
}

template<class IO> void BinaryInstWriter<IO>::visitStructSet(StructSet* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::StructSet);
  parent.writeIndexedHeapType(curr->ref->type.getHeapType());
  writeValue<ValueWritten::StructFieldIndex>(curr->index);
}

template<class IO> void BinaryInstWriter<IO>::visitArrayNew(ArrayNew* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  if (curr->rtt) {
    if (curr->isWithDefault()) {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayNewDefaultWithRtt);
    } else {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayNewWithRtt);
    }
  } else {
    if (curr->isWithDefault()) {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayNewDefault);
    } else {
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayNew);
    }
  }
  parent.writeIndexedHeapType(curr->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitArrayInit(ArrayInit* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  if (curr->rtt) {
    writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayInit);
  } else {
    writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayInitStatic);
  }
  parent.writeIndexedHeapType(curr->type.getHeapType());
  writeValue<ValueWritten::ArraySize>(curr->values.size());
}

template<class IO> void BinaryInstWriter<IO>::visitArrayGet(ArrayGet* curr) {
  auto heapType = curr->ref->type.getHeapType();
  const auto& field = heapType.getArray().element;
  int8_t op;
  if (field.type != Type::i32 || field.packedType == Field::not_packed) {
    op = BinaryConsts::ArrayGet;
  } else if (curr->signed_) {
    op = BinaryConsts::ArrayGetS;
  } else {
    op = BinaryConsts::ArrayGetU;
  }
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(op);
  parent.writeIndexedHeapType(heapType);
}

template<class IO> void BinaryInstWriter<IO>::visitArraySet(ArraySet* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArraySet);
  parent.writeIndexedHeapType(curr->ref->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitArrayLen(ArrayLen* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayLen);
  parent.writeIndexedHeapType(curr->ref->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitArrayCopy(ArrayCopy* curr) {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
  writeValue<ValueWritten::ASTNode32>(BinaryConsts::ArrayCopy);
  parent.writeIndexedHeapType(curr->destRef->type.getHeapType());
  parent.writeIndexedHeapType(curr->srcRef->type.getHeapType());
}

template<class IO> void BinaryInstWriter<IO>::visitRefAs(RefAs* curr) {
  switch (curr->op) {
    case RefAsNonNull:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::RefAsNonNull);
      break;
    case RefAsFunc:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefAsFunc);
      break;
    case RefAsData:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefAsData);
      break;
    case RefAsI31:
      writeValue<ValueWritten::ASTNode>(BinaryConsts::GCPrefix);
      writeValue<ValueWritten::ASTNode32>(BinaryConsts::RefAsI31);
      break;
    default:
      WASM_UNREACHABLE("invalid ref.as_*");
  }
}

template<class IO> void BinaryInstWriter<IO>::emitScopeEnd(Expression* curr) {
  assert(!breakStack.empty());
  breakStack.pop_back();
  writeValue<ValueWritten::ASTNode>(BinaryConsts::End);
  if (func && !sourceMap) {
    parent.writeDebugLocationEnd(curr, func);
  }
}

template<class IO> void BinaryInstWriter<IO>::emitFunctionEnd() {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::End);
}

template<class IO> void BinaryInstWriter<IO>::emitUnreachable() {
  writeValue<ValueWritten::ASTNode>(BinaryConsts::Unreachable);
}

template<class IO> void BinaryInstWriter<IO>::mapLocalsAndEmitHeader() {
  assert(func && "BinaryInstWriter: function is not set");
  // Map params
  for (Index i = 0; i < func->getNumParams(); i++) {
    mappedLocals[std::make_pair(i, 0)] = i;
  }
  // Normally we map all locals of the same type into a range of adjacent
  // addresses, which is more compact. However, if we need to keep DWARF valid,
  // do not do any reordering at all - instead, do a trivial mapping that
  // keeps everything unmoved.
  if (DWARF) {
    FindAll<TupleExtract> extracts(func->body);
    if (!extracts.list.empty()) {
      Fatal() << "DWARF + multivalue is not yet complete";
    }
    Index varStart = func->getVarIndexBase();
    Index varEnd = varStart + func->getNumVars();
    writeValue<ValueWritten::NumFunctionLocals>(func->getNumVars());
    for (Index i = varStart; i < varEnd; i++) {
      mappedLocals[std::make_pair(i, 0)] = i;
      writeValue<ValueWritten::FunctionLocalSize>(1);
      parent.writeType(func->getLocalType(i));
    }
    return;
  }
  for (auto type : func->vars) {
    for (const auto& t : type) {
      noteLocalType(t);
    }
  }
  countScratchLocals();
  std::unordered_map<Type, size_t> currLocalsByType;
  for (Index i = func->getVarIndexBase(); i < func->getNumLocals(); i++) {
    Index j = 0;
    for (const auto& type : func->getLocalType(i)) {
      auto fullIndex = std::make_pair(i, j++);
      Index index = func->getVarIndexBase();
      for (auto& localType : localTypes) {
        if (type == localType) {
          mappedLocals[fullIndex] = index + currLocalsByType[localType];
          currLocalsByType[type]++;
          break;
        }
        index += numLocalsByType.at(localType);
      }
    }
  }
  setScratchLocals();
  writeValue<ValueWritten::CountNumLocalsByType>(numLocalsByType.size());
  for (auto& localType : localTypes) {
    writeValue<ValueWritten::NumLocalsByType>(numLocalsByType.at(localType));
    parent.writeType(localType);
  }
}

template<class IO> void BinaryInstWriter<IO>::noteLocalType(Type type) {
  if (!numLocalsByType.count(type)) {
    localTypes.push_back(type);
  }
  numLocalsByType[type]++;
}

template<class IO> void BinaryInstWriter<IO>::countScratchLocals() {
  // Add a scratch register in `numLocalsByType` for each type of
  // tuple.extract with nonzero index present.
  FindAll<TupleExtract> extracts(func->body);
  for (auto* extract : extracts.list) {
    if (extract->type != Type::unreachable && extract->index != 0) {
      scratchLocals[extract->type] = 0;
    }
  }
  for (auto& [type, _] : scratchLocals) {
    noteLocalType(type);
  }
}

template<class IO> void BinaryInstWriter<IO>::setScratchLocals() {
  Index index = func->getVarIndexBase();
  for (auto& localType : localTypes) {
    index += numLocalsByType[localType];
    if (scratchLocals.find(localType) != scratchLocals.end()) {
      scratchLocals[localType] = index - 1;
    }
  }
}

template<class IO>
void BinaryInstWriter<IO>::emitMemoryAccess(size_t alignment,
                                            size_t bytes,
                                            uint32_t offset) {
  writeValue<ValueWritten::MemoryAccessAlignment>(
    Bits::log2(alignment ? alignment : bytes));
  writeValue<ValueWritten::MemoryAccessOffset>(offset);
}

template<class IO>
int32_t BinaryInstWriter<IO>::getBreakIndex(Name name) { // -1 if not found
  if (name == DELEGATE_CALLER_TARGET) {
    return breakStack.size();
  }
  for (int i = breakStack.size() - 1; i >= 0; i--) {
    if (breakStack[i] == name) {
      return breakStack.size() - 1 - i;
    }
  }
  WASM_UNREACHABLE("break index not found");
}

void StackIRGenerator::emit(Expression* curr) {
  StackInst* stackInst = nullptr;
  if (curr->is<Block>()) {
    stackInst = makeStackInst(StackInst::BlockBegin, curr);
  } else if (curr->is<If>()) {
    stackInst = makeStackInst(StackInst::IfBegin, curr);
  } else if (curr->is<Loop>()) {
    stackInst = makeStackInst(StackInst::LoopBegin, curr);
  } else if (curr->is<Try>()) {
    stackInst = makeStackInst(StackInst::TryBegin, curr);
  } else {
    stackInst = makeStackInst(curr);
  }
  stackIR.push_back(stackInst);
}

void StackIRGenerator::emitScopeEnd(Expression* curr) {
  StackInst* stackInst = nullptr;
  if (curr->is<Block>()) {
    stackInst = makeStackInst(StackInst::BlockEnd, curr);
  } else if (curr->is<If>()) {
    stackInst = makeStackInst(StackInst::IfEnd, curr);
  } else if (curr->is<Loop>()) {
    stackInst = makeStackInst(StackInst::LoopEnd, curr);
  } else if (curr->is<Try>()) {
    stackInst = makeStackInst(StackInst::TryEnd, curr);
  } else {
    WASM_UNREACHABLE("unexpected expr type");
  }
  stackIR.push_back(stackInst);
}

StackInst* StackIRGenerator::makeStackInst(StackInst::Op op,
                                           Expression* origin) {
  auto* ret = module.allocator.alloc<StackInst>();
  ret->op = op;
  ret->origin = origin;
  auto stackType = origin->type;
  if (origin->is<Block>() || origin->is<Loop>() || origin->is<If>() ||
      origin->is<Try>()) {
    if (stackType == Type::unreachable) {
      // There are no unreachable blocks, loops, or ifs. we emit extra
      // unreachables to fix that up, so that they are valid as having none
      // type.
      stackType = Type::none;
    } else if (op != StackInst::BlockEnd && op != StackInst::IfEnd &&
               op != StackInst::LoopEnd && op != StackInst::TryEnd) {
      // If a concrete type is returned, we mark the end of the construct has
      // having that type (as it is pushed to the value stack at that point),
      // other parts are marked as none).
      stackType = Type::none;
    }
  }
  ret->type = stackType;
  return ret;
}

template<class IO> void StackIRToBinaryWriter<IO>::write() {
  writer.mapLocalsAndEmitHeader();
  // Stack to track indices of catches within a try
  SmallVector<Index, 4> catchIndexStack;
  for (auto* inst : *func->stackIR) {
    if (!inst) {
      continue; // a nullptr is just something we can skip
    }
    switch (inst->op) {
      case StackInst::TryBegin:
        catchIndexStack.push_back(0);
        [[fallthrough]];
      case StackInst::Basic:
      case StackInst::BlockBegin:
      case StackInst::IfBegin:
      case StackInst::LoopBegin: {
        writer.visit(inst->origin);
        break;
      }
      case StackInst::TryEnd:
        catchIndexStack.pop_back();
        [[fallthrough]];
      case StackInst::BlockEnd:
      case StackInst::IfEnd:
      case StackInst::LoopEnd: {
        writer.emitScopeEnd(inst->origin);
        break;
      }
      case StackInst::IfElse: {
        writer.emitIfElse(inst->origin->cast<If>());
        break;
      }
      case StackInst::Catch: {
        writer.emitCatch(inst->origin->cast<Try>(), catchIndexStack.back()++);
        break;
      }
      case StackInst::CatchAll: {
        writer.emitCatchAll(inst->origin->cast<Try>());
        break;
      }
      case StackInst::Delegate: {
        writer.emitDelegate(inst->origin->cast<Try>());
        // Delegates end the try, like a TryEnd.
        catchIndexStack.pop_back();
        break;
      }
      default:
        WASM_UNREACHABLE("unexpected op");
    }
  }
  writer.emitFunctionEnd();
}

template class BinaryInstWriter<DefaultWasmBinaryWriter>;
template class BinaryenIRToBinaryWriter<DefaultWasmBinaryWriter>;
template class StackIRToBinaryWriter<DefaultWasmBinaryWriter>;

} // namespace wasm
