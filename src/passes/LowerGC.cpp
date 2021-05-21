/*
 * Copyright 2021 WebAssembly Community Group participants
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

//
// Lowers Wasm GC to linear memory.
//

#include "ir/module-utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

Type getLoweredType(Type type, Memory& memory) {
  // References and Rtts are pointers.
  if (type.isRef() || type.isRtt()) {
    return memory.indexType;
  }
  return type;
}

// The layout of a struct in linear memory.
struct Layout {
  // The total size of the struct.
  Address size;
  // The offsets of fields. Note that the first field's offset may not be 0,
  // as we need room for the rtt.
  SmallVector<Address, 4> fieldOffsets;
};

using Layouts = std::unordered_map<HeapType, Layout>;

struct LoweringInfo {
  Layouts layouts;

  Name malloc;

  Address pointerSize;
  Type pointerType;
};

// Lower GC instructions.
struct LowerGCCode : public WalkerPass<PostWalker<LowerGCCode>> {
  bool isFunctionParallel() override { return true; }

  LoweringInfo* loweringInfo;

  LowerGCCode* create() override { return new LowerGCCode(loweringInfo); }

  LowerGCCode(LoweringInfo* loweringInfo) : loweringInfo(loweringInfo) {}

  void visitStructNew(StructSet* curr) {
    Builder builder(*getModule());
    auto type = curr->ref->type.getHeapType();
    std::vector<Expression*> list;
    auto local = builder.addVar(getFunction(), loweringInfo->pointerType);
    // Malloc space for our struct.
    list.push_back(
      builder.makeLocalSet(
        local
        builder.makeCall(
          loweringInfo->malloc,
          builder.makeConst(int32_t(loweringInfo->structLayouts[type].size))
        )
      )
    );
    // Store the rtt.
    list.push_back(builder.makeStore(
      loweringInfo->pointerSize,
      0,
      loweringInfo->pointerSize,
      builder.makeLocalGet(local, loweringInfo->pointerType),
      curr->rtt,
      loweredType
    ));
    // Store the values, by representing them as StructSets.
    auto& fields = type.getStruct().fields;
    StructSet set;
    set.ref = builder.makeLocalGet(local, loweringInfo->pointerType);
    for (Index i = 0; i < fields.size(); i++) {
      set.index = i;
      if (curr->isWithDefault()) {
        set.value = builder.makeConst(LiteralUtils::makeZero(fields[i].type));
      } else {
        set.value = curr->operands[o];
      }
      list.push_back(lower(&set));
    }
    // Return the pointer.
    list.push_back(builder.makeLocalGet(local, loweringInfo->pointerType));
    replaceCurrent(
      builder.makeBlock(list)
    );
  }

  void visitStructSet(StructSet* curr) {
    replaceCurrent(lower(curr));
  }

  Expression* lower(StructSet* curr) {
    // TODO: ignore unreachable, or run dce before
    Builder builder(*getModule());
    auto type = curr->ref->type.getHeapType();
    auto& field = type.getStruct().fields[curr->index];
    auto loweredType = getLoweredType(field.type, getModule()->memory);
    return
      builder.makeStore(
        loweredType.getByteSize(),
        loweringInfo->layouts[type].fieldOffsets[curr->index],
        loweredType.getByteSize(),
        curr->ref,
        curr->value,
        loweredType
      );
  }

  void visitStructGet(StructGet* curr) {
    replaceCurrent(lower(curr));
  }

  Expression* lower(StructGet* curr) {
    // TODO: ignore unreachable, or run dce before
    Builder builder(*getModule());
    auto type = curr->ref->type.getHeapType();
    auto& field = type.getStruct().fields[curr->index];
    auto loweredType = getLoweredType(field.type, getModule()->memory);
    return
      builder.makeLoad(
        loweredType.getByteSize(),
        false, // TODO: signedness
        loweringInfo->layouts[type].fieldOffsets[curr->index],
        loweredType.getByteSize(),
        curr->ref,
        loweredType
      );
  }
};

// Lower GC types on all instructions. For example, this turns a local.get from
// a reference to an i32. We must do this in a separate pass after LowerGCCode
// as we still need the heap types to be present while we lower instructions
// (because we use the heap types to figure out the layout of struct
// operations).
struct LowerGCTypes : public WalkerPass<PostWalker<LowerGCTypes, UnifiedExpressionVisitor<LowerGCTypes>>> {
  bool isFunctionParallel() override { return true; }

  LowerGCTypes* create() override { return new LowerGCTypes(); }

  void visitExpression(Expression* curr) {
    // Update the type.
    curr->type = lower(curr->type);
  }

  void visitFunction(Function* func) {
    std::vector<Type> params;
    for (auto t : func->sig.params) {
      params.push_back(lower(t));
    }
    std::vector<Type> results;
    for (auto t : func->sig.results) {
      results.push_back(lower(t));
    }
    func->sig = Signature(Type(params), Type(results));
    for (auto& t : func->vars) {
      t = lower(t);
    }
  }

private:
  Type lower(Type type) {
    return getLoweredType(type, getModule()->memory);
  }
};

} // anonymous namespace

struct LowerGC : public Pass {
  void run(PassRunner* runner, Module* module_) override {
    module = module_;
    addMemory();
    addRuntime();
    computeStructLayouts();
    lowerCode(runner);
  }

private:
  Module* module;

  LoweringInfo loweringInfo;

  void addMemory() {
    module->memory.exists = true;

    // 16MB, arbitrarily for now.
    module->memory.initial = module->memory.max = 256;

    assert(!module->memory.is64());
    loweringInfo->pointerSize = 4;
    loweringInfo->pointerType = module->memory->indexType;
  }

  void addRuntime() {
    Builder builder(*getModule());
    loweringInfo->malloc = "malloc";
    /*
    auto* malloc = module->addFunction(builder.makeFunction(
      "malloc", { Type::i32, Type::i32 }, {},
      builder.makeSequence(
        builder.makeGlobalSet(
        builder.makeBinary(
        ),
        builder.makeBinary(
        ),
      )
      ));
    */
  }

  void computeStructLayouts() {
    // Collect all the heap types in order to analyze them and decide on their
    // layout in linear memory.
    std::vector<HeapType> types;
    std::unordered_map<HeapType, Index> typeIndices;
    ModuleUtils::collectHeapTypes(*module, types, typeIndices);
    for (auto type : types) {
      if (type.isStruct()) {
        computeLayout(type, loweringInfo.layouts[type]);
      }
    }
  }

  void computeLayout(HeapType type, Layout& layout) {
    // A pointer to the RTT takes up the first bytes in the struct, so fields
    // start afterwards.
    Address nextField = loweringInfo->pointerSize;
    auto& fields = type.getStruct().fields;
    for (auto& field : fields) {
      layout.fieldOffsets.push_back(nextField);
      // TODO: packed types? for now, always use i32 for them
      nextField = nextField + getLoweredType(field.type, module->memory).getByteSize();
    }
  }

  void lowerCode(PassRunner* runner) {
    PassRunner subRunner(runner);
    subRunner.add(std::unique_ptr<LowerGCCode>(LowerGCCode(&loweringInfo).create()));
    subRunner.add(std::make_unique<LowerGCTypes>());
    subRunner.setIsNested(true);
    subRunner.run();

    LowerGCCode(&loweringInfo).walkModuleCode(module);
    LowerGCTypes().walkModuleCode(module);
  }
};

Pass* createLowerGCPass() { return new LowerGC(); }

} // namespace wasm