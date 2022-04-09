//#define POSSIBLE_TYPES_DEBUG 1
/*
 * Copyright 2022 WebAssembly Community Group participants
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

#include <variant>

#include "ir/branch-utils.h"
#include "ir/find_all.h"
#include "ir/module-utils.h"
#include "ir/possible-types.h"
#include "ir/subtypes.h"
#include "support/unique_deferring_queue.h"
#include "wasm.h"

namespace wasm::PossibleTypes {

namespace {

#ifdef POSSIBLE_TYPES_DEBUG
void dump(Location location) {
  if (auto* loc = std::get_if<ExpressionLocation>(&location)) {
    std::cout << "  exprloc \n" << *loc->expr << '\n';
  } else if (auto* loc = std::get_if<StructLocation>(&location)) {
    std::cout << "  structloc " << loc->type << " : " << loc->index << '\n';
  } else if (auto* loc = std::get_if<TagLocation>(&location)) {
    std::cout << "  tagloc " << loc->tag << '\n';
  } else if (auto* loc = std::get_if<ResultLocation>(&location)) {
    std::cout << "  resultloc " << loc->func->name << " : " << loc->index
              << '\n';
  } else {
    std::cout << "  (other)\n";
  }
}
#endif

// The data we gather from each function, as we process them in parallel. Later
// this will be merged into a single big graph.
struct FuncInfo {
  // All the connections we found in this function.
  // TODO: as an optimization we could perhaps avoid redundant copies in this
  //       vector using a set?
  std::vector<Connection> connections;

  // All the allocations (struct.new, etc.) that we found in this function.
  std::vector<Expression*> allocations;
};

bool containsRef(Type type) {
  if (type.isRef()) {
    return true;
  }
  if (type.isTuple()) {
    for (auto t : type) {
      if (t.isRef()) {
        return true;
      }
    }
  }
  return false;
}

template<typename T> bool containsRef(const T& vec) {
  for (auto* expr : vec) {
    if (expr->type.isRef()) {
      return true;
    }
  }
  return false;
}

struct ConnectionFinder
  : public PostWalker<ConnectionFinder,
                      UnifiedExpressionVisitor<ConnectionFinder>> {
  FuncInfo& info;

  ConnectionFinder(FuncInfo& info) : info(info) {}

  // Each visit*() call is responsible for connecting the children of a
  // node to that node. Responsibility for connecting the node's output to
  // anywhere else (another expression or the function itself, if we are at
  // the top level) is the responsibility of the outside.

  void visitExpression(Expression* curr) {
    // Breaks send values to their destinations. Handle that here using
    // existing utility code which is shorter than implementing multiple
    // visit*() methods (however, it may be slower TODO)
    BranchUtils::operateOnScopeNameUsesAndSentValues(
      curr, [&](Name target, Expression* value) {
        if (value && value->type.isRef()) {
          assert(!value->type.isTuple()); // TODO
          info.connections.push_back(
            {ExpressionLocation{value, 0}, BranchLocation{getFunction(), target}});
        }
      });

    // Branch targets receive the things sent to them and flow them out.
    if (curr->type.isRef()) {
      assert(!curr->type.isTuple()); // TODO
      BranchUtils::operateOnScopeNameDefs(curr, [&](Name target) {
        info.connections.push_back(
          {BranchLocation{getFunction(), target}, ExpressionLocation{curr, 0}});
      });
    }
    // TODO: if we are a branch source or target, skip the loop later
    // down? in general any ref-receiving instruction that reaches that
    // loop will end up connecting a child ref to the current expression,
    // but e.g. ref.is_* does not do so. OTOH ref.test's output is then an
    // integer anyhow, so we don't actually reach the loop.

    // The default behavior is to connect all input expressions to the
    // current one, as it might return an output that includes them. We only do
    // so for references or tuples containing a reference as anything else can
    // be ignored.
    if (!containsRef(curr->type)) {
      return;
    }

    for (auto* child : ChildIterator(curr)) {
      // We handle the simple case here of a child that passes its entire value
      // to the parent. Other things (like TupleMake) should be handled in
      // specific visitors below.
      if (!containsRef(child->type) || curr->type.size() != child->type.size()) {
        continue;
      }

      for (Index i = 0; i < curr->type.size(); i++) {
        info.connections.push_back(
          {ExpressionLocation{child, i}, ExpressionLocation{curr, i}});
      }
    }
  }

  // Locals read and write to their index.
  // TODO: we could use a LocalGraph for better precision
  void visitLocalGet(LocalGet* curr) {
    if (containsRef(curr->type)) {
      for (Index i = 0; i < curr->type.size(); i++) {
        info.connections.push_back(
          {LocalLocation{getFunction(), curr->index, i}, ExpressionLocation{curr, i}});
      }
    }
  }
  void visitLocalSet(LocalSet* curr) {
    if (!containsRef(curr->value->type)) {
      return;
    }
    for (Index i = 0; i < curr->value->type.size(); i++) {
      info.connections.push_back({ExpressionLocation{curr->value, i},
                                  LocalLocation{getFunction(), curr->index, i}});
      if (curr->isTee()) {
        info.connections.push_back(
          {ExpressionLocation{curr->value, i}, ExpressionLocation{curr, i}});
      }
    }
  }

  // Globals read and write from their location.
  void visitGlobalGet(GlobalGet* curr) {
    if (curr->type.isRef()) {
      info.connections.push_back(
        {GlobalLocation{curr->name}, ExpressionLocation{curr, 0}});
    }
  }
  void visitGlobalSet(GlobalSet* curr) {
    if (curr->value->type.isRef()) {
      info.connections.push_back(
        {ExpressionLocation{curr->value, 0}, GlobalLocation{curr->name}});
    }
  }

  // Iterates over a list of children and adds connections to parameters
  // and results as needed. The param/result functions receive the index
  // and create the proper location for it.
  void handleCall(Expression* curr,
                  ExpressionList& operands,
                  std::function<Location(Index)> makeParamLocation,
                  std::function<Location(Index)> makeResultLocation) {
    Index i = 0;
    for (auto* operand : operands) {
      if (operand->type.isRef()) {
        info.connections.push_back(
          {ExpressionLocation{operand, 0}, makeParamLocation(i)});
      }
      i++;
    }

    // Add results, if anything flows out.
    if (containsRef(curr->type)) {
      for (Index i = 0; i < curr->type.size(); i++) {
        info.connections.push_back(
          {makeResultLocation(i), ExpressionLocation{curr, i}});
      }
    }
  }

  // Calls send values to params in their possible targets, and receive
  // results.
  void visitCall(Call* curr) {
    auto* target = getModule()->getFunction(curr->target);
    handleCall(
      curr,
      curr->operands,
      [&](Index i) {
        return LocalLocation{target, i, 0};
      },
      [&](Index i) {
        return ResultLocation{target, i};
      });
  }
  void visitCallIndirect(CallIndirect* curr) {
    auto target = curr->heapType;
    handleCall(
      curr,
      curr->operands,
      [&](Index i) {
        return SignatureParamLocation{target, i};
      },
      [&](Index i) {
        return SignatureResultLocation{target, i};
      });
  }
  void visitCallRef(CallRef* curr) {
    auto targetType = curr->target->type;
    if (targetType.isRef()) {
      auto target = targetType.getHeapType();
      handleCall(
        curr,
        curr->operands,
        [&](Index i) {
          return SignatureParamLocation{target, i};
        },
        [&](Index i) {
          return SignatureResultLocation{target, i};
        });
    }
  }

  // Iterates over a list of children and adds connections as needed. The
  // target of the connection is created using a function that is passed
  // in which receives the index of the child.
  void handleChildList(ExpressionList& operands,
                       std::function<Location(Index)> makeTarget) {
    Index i = 0;
    for (auto* operand : operands) {
      assert(!operand->type.isTuple());
      if (operand->type.isRef()) {
        info.connections.push_back(
          {ExpressionLocation{operand, 0}, makeTarget(i)});
      }
      i++;
    }
  }

  // Creation operations form the starting connections from where data
  // flows. We will create an ExpressionLocation for them when their
  // parent uses them, as we do for all instructions, so nothing special
  // needs to be done here, but we do need to create connections from
  // inputs - the initialization of their data - to the proper places.
  void visitStructNew(StructNew* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }
    auto type = curr->type.getHeapType();
    handleChildList(curr->operands, [&](Index i) {
      return StructLocation{type, i};
    });
    info.allocations.push_back(curr);
  }
  void visitArrayNew(ArrayNew* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }
    // Note that if there is no initial value here then it is null, which is
    // not something we need to connect.
    if (curr->init && curr->init->type.isRef()) {
      info.connections.push_back({ExpressionLocation{curr->init, 0},
                                  ArrayLocation{curr->type.getHeapType()}});
    }
    info.allocations.push_back(curr);
  }
  void visitArrayInit(ArrayInit* curr) {
    if (curr->type == Type::unreachable) {
      return;
    }
    if (!curr->values.empty() && curr->values[0]->type.isRef()) {
      auto type = curr->type.getHeapType();
      handleChildList(curr->values,
                      [&](Index i) { return ArrayLocation{type}; });
    }
    info.allocations.push_back(curr);
  }

  // Struct operations access the struct fields' locations.
  void visitStructGet(StructGet* curr) {
    if (curr->type.isRef()) {
      info.connections.push_back(
        {StructLocation{curr->ref->type.getHeapType(), curr->index},
         ExpressionLocation{curr, 0}});
    }
  }
  void visitStructSet(StructSet* curr) {
    if (curr->value->type.isRef()) {
      info.connections.push_back(
        {ExpressionLocation{curr->value, 0},
         StructLocation{curr->ref->type.getHeapType(), curr->index}});
    }
  }
  // Array operations access the array's location.
  void visitArrayGet(ArrayGet* curr) {
    if (curr->type.isRef()) {
      info.connections.push_back({ArrayLocation{curr->ref->type.getHeapType()},
                                  ExpressionLocation{curr, 0}});
    }
  }
  void visitArraySet(ArraySet* curr) {
    if (curr->value->type.isRef()) {
      info.connections.push_back(
        {ExpressionLocation{curr->value, 0},
         ArrayLocation{curr->ref->type.getHeapType()}});
    }
  }

  // TODO: Model which throws can go to which catches. For now, anything
  //       thrown is sent to the location of that tag, and any catch of that
  //       tag can read them
  void visitTry(Try* curr) {
    auto numTags = curr->catchTags.size();
    for (Index tagIndex = 0; tagIndex < numTags; tagIndex++) {
      auto tag = curr->catchTags[tagIndex];
      auto* body = curr->catchBodies[tagIndex];

      // Find the pop of the tag's contents. The body must start with such a
      // pop, which might be of a tuple.
      FindAll<Pop> pops(body);
      assert(!pops.list.empty());
      auto* pop = pops.list[0];
      if (containsRef(pop->type)) {
        for (Index i = 0; i < pop->type.size(); i++) {
          info.connections.push_back({TagLocation{tag, i}, ExpressionLocation{pop, i}});
        }
      }
    }
  }
  void visitThrow(Throw* curr) {
    auto& operands = curr->operands;
    if (!containsRef(operands)) {
      return;
    }

    auto tag = curr->tag;
    for (Index i = 0; i < curr->operands.size(); i++) {
      info.connections.push_back(
        {ExpressionLocation{operands[i], 0}, TagLocation{tag, i}});
    }
  }

  void visitTableGet(TableGet* curr) { WASM_UNREACHABLE("todo"); }
  void visitTableSet(TableSet* curr) { WASM_UNREACHABLE("todo"); }

  void addResult(Expression* value) {
    if (value && containsRef(value->type)) {
      for (Index i = 0; i < value->type.size(); i++) {
        info.connections.push_back(
          {ExpressionLocation{value, i}, ResultLocation{getFunction(), i}});
      }
    }
  }

  void visitReturn(Return* curr) { addResult(curr->value); }

  void visitFunction(Function* curr) {
    // Functions with a result can flow a value out from their body.
    addResult(curr->body);
  }
};

} // anonymous namespace

void Oracle::analyze() {
#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "parallel phase\n";
#endif

  ModuleUtils::ParallelFunctionAnalysis<FuncInfo> analysis(
    wasm, [&](Function* func, FuncInfo& info) {
      if (func->imported()) {
        // TODO: add an option to not always assume a closed world, in which
        //       case we'd need to track values escaping etc.
        return;
      }

      ConnectionFinder finder(info);
      finder.walkFunctionInModule(func, &wasm);
    });

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "single phase\n";
#endif

  // Also walk the global module code, adding it to the map as a function of
  // null.
  auto& globalInfo = analysis.map[nullptr];
  ConnectionFinder(globalInfo).walkModuleCode(&wasm);

  // Connect global init values (which we've just processed, as part of the
  // module code) to the globals they initialize.
  for (auto& global : wasm.globals) {
    if (!global->imported()) {
      auto* init = global->init;
      if (init->type.isRef()) {
        globalInfo.connections.push_back(
          {ExpressionLocation{init, 0}, GlobalLocation{global->name}});
      }
    }
  }

  // Merge the function information into a single large graph that represents
  // the entire program all at once. First, gather all the connections from all
  // the functions. We do so into a set, which deduplicates everythings.
  // map of the possible types at each location.
  std::unordered_set<Connection> connections;
  std::vector<Expression*> allocations;

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "merging phase\n";
#endif

  for (auto& [func, info] : analysis.map) {
    for (auto& connection : info.connections) {
      connections.insert(connection);
    }
    for (auto& allocation : info.allocations) {
      allocations.push_back(allocation);
    }
  }

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "func phase\n";
#endif

  // Connect function parameters to their signature, so that any indirect call
  // of that signature will reach them.
  // TODO: find which functions are even taken by reference
  for (auto& func : wasm.functions) {
    for (Index i = 0; i < func->getParams().size(); i++) {
      connections.insert(
        {SignatureParamLocation{func->type, i}, LocalLocation{func.get(), i, 0}});
    }
    for (Index i = 0; i < func->getResults().size(); i++) {
      connections.insert({ResultLocation{func.get(), i},
                          SignatureResultLocation{func->type, i}});
    }
  }

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "struct phase\n";
#endif

  // Add subtyping connections, but see the TODO below about how we can do this
  // "dynamically" in a more effective but complex way.
  SubTypes subTypes(wasm);
  for (auto type : subTypes.types) {
    // Tie two locations together, linking them both ways.
    auto tie = [&](const Location& a, const Location& b) {
      connections.insert({a, b});
      connections.insert({b, a});
    };
    if (type.isStruct()) {
      // StructLocations refer to a struct.get/set/new and so in general they
      // may refer to data of a subtype of the type written on them. Connect to
      // their immediate subtypes here in both directions.
      auto numFields = type.getStruct().fields.size();
      for (auto subType : subTypes.getSubTypes(type)) {
        for (Index i = 0; i < numFields; i++) {
          tie(StructLocation{type, i}, StructLocation{subType, i});
        }
      }
    } else if (type.isArray()) {
      // StructLocations refer to a struct.get/set/new and so in general they
      // may refer to data of a subtype of the type written on them. Connect to
      // their immediate subtypes here.
      for (auto subType : subTypes.getSubTypes(type)) {
        tie(ArrayLocation{type}, ArrayLocation{subType});
      }
    }
  }

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "DAG phase\n";
#endif

  // Build the flow info. First, note the connection targets.
  for (auto& connection : connections) {
    flowInfoMap[connection.from].targets.push_back(connection.to);
  }

#ifndef NDEBUG
  for (auto& [location, info] : flowInfoMap) {
    // The vector of targets must have no duplicates.
    auto& targets = info.targets;
    std::unordered_set<Location> uniqueTargets;
    for (const auto& target : targets) {
      uniqueTargets.insert(target);
    }
    assert(uniqueTargets.size() == targets.size());
  }
#endif

  // The work remaining to do: locations that we just updated, which means we
  // should update their children when we pop them from this queue.
  UniqueDeferredQueue<Location> work;

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "prep phase\n";
#endif

  // The starting state for the flow is for each allocation to contain the type
  // allocated there.
  for (const auto& allocation : allocations) {
    // The type must not be a reference (as we allocated here), and it cannot be
    // unreachable (we should have ignored such things before).
    assert(allocation->type.isRef());

    auto location = ExpressionLocation{allocation, 0};
    auto& info = flowInfoMap[location];

    // There must not be anything at this location already, as each allocation
    // appears once in the vector of allocations.
    assert(info.types.empty());

    info.types.insert(allocation->type.getHeapType());
    work.push(location);
  }

#ifdef POSSIBLE_TYPES_DEBUG
  std::cout << "flow phase\n";
#endif

  // Flow the data.
  while (!work.empty()) {
    auto location = work.pop();
    const auto& info = flowInfoMap[location];

    // TODO: We can refine the types here, by not flowing anything through a
    //       ref.cast that it would trap on.
    const auto& types = info.types;

#ifdef POSSIBLE_TYPES_DEBUG
    std::cout << "pop item with " << types.size() << " lanes\n";
    dump(location);
    for (auto& lane : types) {
      std::cout << "  lane has " << lane.size() << " types\n";
    }
#endif

    // TODO: implement the following optimization, and remove the hardcoded
    //       links created above.
    // ==================================TODO==================================
    // Compute the targets we need to update. Normally we simply flow to the
    // targets defined in the graph, however, some connections are best done
    // "dynamically". Consider a struct.set that writes a references to some
    // field. We could assume that it writes it to any struct of the type it is
    // defined on, or any subtype, but instead we use the information in the
    // graph: the types that are possible in the reference field determine where
    // the values should flow. That is,
    //
    //  (struct.set $A
    //    (ref B)
    //    (value C) ;; also a reference type of some kind.
    //  )
    //
    // The rules we apply to such an expression are:
    //
    //  1. When a new type arrives in (value C), we send it to the proper field
    //     of all types that can appear in (ref B).
    //  2. When a new type arrives in (ref B) we must send the values in
    //     (value C) to their new target as well.
    // ==================================TODO==================================
    const auto& targets = info.targets;
    if (targets.empty()) {
      continue;
    }

    // Update the targets, and add the ones that change to the remaining work.
    for (const auto& target : targets) {
#ifdef POSSIBLE_TYPES_DEBUG
      std::cout << "  send to target\n";
      dump(target);
#endif

      auto& targetTypes = flowInfoMap[target].types;

      // Update types in one lane of a tuple, copying from inputs to outputs and
      // adding the target to the remaining work if we added something new.
      auto updateTypes = [&](const LocationTypes& inputs,
                             LocationTypes& outputs) {
        if (inputs.empty()) {
          return;
        }
#ifdef POSSIBLE_TYPES_DEBUG
        std::cout << "    updateTypes: src has " << inputs.size()
                  << ", dst has " << outputs.size() << '\n';
#endif
        auto oldSize = outputs.size();
        outputs.update(inputs);
        if (outputs.size() != oldSize) {
          // We inserted something, so there is work to do in this target.
          work.push(target);
        }
      };

      // Otherwise, the input and output must have the same number of lanes.
#ifdef POSSIBLE_TYPES_DEBUG
      std::cout << "  updating lane " << i << "\n";
#endif
      updateTypes(types, targetTypes);
    }
  }

  // TODO: Add analysis and retrieval logic for fields of immutable globals,
  //       including multiple levels of depth (necessary for itables in j2wasm)
}

} // namespace wasm::PossibleTypes

/*
TODO: w.wasm failure on LIMIT=1
TODO: null deref at runtime on j2wasm output
*/