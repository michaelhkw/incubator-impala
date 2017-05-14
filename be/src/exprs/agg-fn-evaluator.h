// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_EXPRS_AGG_FN_EVALUATOR_H
#define IMPALA_EXPRS_AGG_FN_EVALUATOR_H

#include <string>

#include <boost/scoped_ptr.hpp>
#include "common/status.h"
#include "exprs/agg-fn.h"
#include "runtime/descriptors.h"
#include "runtime/lib-cache.h"
#include "runtime/tuple-row.h"
#include "runtime/types.h"
#include "udf/udf.h"
#include "udf/udf-internal.h"

#include "gen-cpp/Exprs_types.h"
#include "gen-cpp/PlanNodes_types.h"
#include "gen-cpp/Types_types.h"

namespace impala {

class MemPool;
class MemTracker;
class ObjectPool;
class RowDescriptor;
class RuntimeState;
class SlotDescriptor;
class Tuple;
class TupleRow;
class TExprNode;

/// AggFnEvaluator is the interface for evaluating aggregate functions during execution.
///
/// AggFnEvaluator contains runtime state and implements wrapper functions which convert
/// the input TupleRow into AnyVal format expected by UDAF functions defined in AggFn.
/// It also evaluates TupleRow against input expressions, stores the results in staging
/// input values which are passed to Update() function to update the intermediate value
/// and handles the merging of intermediate values in the merge phases of execution.
///
/// This class is not threadsafe. An evaluator can be cloned to isolate resource
/// consumption per partition in an aggregation node.
///
class AggFnEvaluator {
 public:
  /// Creates an AggFnEvaluator object from the aggregate expression 'agg_fn'.
  /// The evaluator is added to 'pool' and returned in 'evaluator'. This will also
  /// create a single evaluator for each input expression. All allocations will come
  /// from 'mem_pool'. Note that it's the responsibility to call Close() all evaluators
  /// even if this function returns error status on initialization failure.
  static Status Create(const AggFn& agg_fn, RuntimeState* state, ObjectPool* pool,
      MemPool* mem_pool, AggFnEvaluator** evaluator) WARN_UNUSED_RESULT;

  /// Convenience functions for creating evaluators for multiple aggregate functions.
  static Status Create(const std::vector<AggFn*>& agg_fns, RuntimeState* state,
      ObjectPool* pool, MemPool* mem_pool, std::vector<AggFnEvaluator*>* evaluators)
      WARN_UNUSED_RESULT;

  ~AggFnEvaluator();

  /// Initializes the evaluator by calling Open() on all the input expressions' evaluators
  /// and caches all constant input arguments.
  /// TODO: Move the evaluation of constant input arguments to AggFn setup.
  Status Open(RuntimeState* state) WARN_UNUSED_RESULT;

  /// Convenience functions for opening multiple AggFnEvaluators.
  static Status Open(const std::vector<AggFnEvaluator*>& evaluators,
      RuntimeState* state) WARN_UNUSED_RESULT;

  /// Used by PartitionedAggregation node to initialize one evaluator per partition.
  /// Avoid the overhead of re-initializing an evaluator (e.g. calling GetConstVal()
  /// on the input expressions). Cannot be called until after Open() has been called.
  /// 'cloned_evaluator' is a shallow copy of this evaluator: all input values, staging
  /// intermediate values and merge values are shared with the original evaluator. Only
  /// the FunctionContext 'agg_fn_ctx' is cloned for resource isolation per partition.
  /// So, it's not safe to use cloned evaluators concurrently.
  void ShallowClone(
      ObjectPool* pool, MemPool* mem_pool, AggFnEvaluator** cloned_evaluator) const;

  /// Convenience function for cloning multiple evaluators. The newly cloned evaluators
  /// are appended to 'cloned_evaluators'.
  static void ShallowClone(ObjectPool* pool, MemPool* mem_pool,
      const std::vector<AggFnEvaluator*>& evaluators,
      std::vector<AggFnEvaluator*>* cloned_evaluators);

  /// Free resources owned by the evaluator.
  void Close(RuntimeState* state);
  static void Close(const std::vector<AggFnEvaluator*>& evaluators, RuntimeState* state);

  const AggFn& agg_fn() const { return agg_fn_; }

  FunctionContext* IR_ALWAYS_INLINE agg_fn_ctx() const;

  ScalarExprEvaluator* const* IR_ALWAYS_INLINE input_evaluators() const;

  /// Call the initialization function of the AggFn. May update 'dst'.
  void Init(Tuple* dst);

  /// Updates the intermediate state dst based on adding the input src row. This can be
  /// called either to drive the UDA's Update() or Merge() function, depending on whether
  /// the AggFn is a merging aggregation.
  void Add(const TupleRow* src, Tuple* dst);

  /// Updates the intermediate state dst to remove the input src row, i.e. undo
  /// Add(src, dst). Only used internally for analytic fn builtins.
  void Remove(const TupleRow* src, Tuple* dst);

  /// Explicitly does a merge, even if this evaluator is not marked as merging.
  /// This is used by the partitioned agg node when it needs to merge spill results.
  /// In the non-spilling case, this node would normally not merge.
  void Merge(Tuple* src, Tuple* dst);

  /// Flattens any intermediate values containing pointers, and frees any memory
  /// allocated during the init, update and merge phases.
  void Serialize(Tuple* dst);

  /// Does one final transformation of the aggregated value in 'agg_val' and stores the
  /// result in 'output_val'. Also frees the resources allocated during init, update and
  /// merge phases.
  void Finalize(Tuple* agg_val, Tuple* output_val);

  /// Puts the finalized value from Tuple* src in Tuple* dst just as Finalize() does.
  /// However, unlike Finalize(), GetValue() does not clean up state in src.
  /// GetValue() can be called repeatedly with the same src. Only used internally for
  /// analytic fn builtins. Note that StringVal result is from local allocation (which
  /// will be freed in the next QueryMaintenance()) so it needs to be copied out if it
  /// needs to survive beyond QueryMaintenance() (e.g. if 'dst' lives in a row batch).
  void GetValue(Tuple* src, Tuple* dst);

  /// Helper functions for calling the above functions on many evaluators.
  static void Init(const std::vector<AggFnEvaluator*>& evaluators, Tuple* dst);
  static void Add(const std::vector<AggFnEvaluator*>& evaluators, const TupleRow* src,
      Tuple* dst);
  static void Remove(const std::vector<AggFnEvaluator*>& evaluators,
      const TupleRow* src, Tuple* dst);
  static void Serialize(const std::vector<AggFnEvaluator*>& evaluators,
      Tuple* dst);
  static void GetValue(const std::vector<AggFnEvaluator*>& evaluators, Tuple* src,
      Tuple* dst);
  static void Finalize(const std::vector<AggFnEvaluator*>& evaluators, Tuple* src,
      Tuple* dst);

  /// Free local allocations made in UDA functions and input arguments' evaluators.
  void FreeLocalAllocations();
  static void FreeLocalAllocations(const std::vector<AggFnEvaluator*>& evaluators);

  std::string DebugString() const;
  static std::string DebugString(const std::vector<AggFnEvaluator*>& evaluators);

  static const char* LLVM_CLASS_NAME;

 private:
  /// True if the evaluator has been initialized.
  bool opened_ = false;

  /// True if the evaluator has been closed.
  bool closed_ = false;

  /// True if this evaluator is created from a ShallowClone() call.
  const bool is_clone_;

  const AggFn& agg_fn_;

  /// Pointer to the MemPool which all allocations come from.
  /// Owned by the exec node which owns this evaluator.
  MemPool* mem_pool_ = nullptr;

  /// This contains runtime state such as constant input arguments to the aggregate
  /// functions and a FreePool from which the intermediate values are allocated.
  FunctionContext* agg_fn_ctx_ = nullptr;

  /// Evaluators for input expressions for this aggregate function.
  /// Empty if there is no input expression (e.g. count(*)).
  std::vector<ScalarExprEvaluator*> input_evaluators_;

  /// Staging input values used by the interpreted Update() / Merge() paths.
  /// It stores the evaluation results of input expressions to be passed to the
  /// Update() / Merge() function.
  std::vector<impala_udf::AnyVal*> staging_input_vals_;

  /// Staging intermediate and merged values used in the interpreted
  /// Update() / Merge() paths.
  impala_udf::AnyVal* staging_intermediate_val_ = nullptr;
  impala_udf::AnyVal* staging_merge_input_val_ = nullptr;

  /// Use Create() instead.
  AggFnEvaluator(const AggFn& agg_fn, MemPool* mem_pool, bool is_clone);

  /// Return the intermediate type of the aggregate function.
  inline const SlotDescriptor& intermediate_slot_desc() const;
  inline const ColumnType& intermediate_type() const;

  /// The interpreted path for the UDA's Update() function. It sets up the arguments to
  /// call 'fn' is either the 'update_fn_' or 'merge_fn_' of agg_fn_, depending on whether
  /// agg_fn_ is a merging aggregation. This converts from the agg-expr signature, taking
  /// TupleRow to the UDA signature taking AnyVals by evaluating any input expressions
  /// and populating the staging input values.
  ///
  /// Note that this function may be superseded by the codegend Update() IR function
  /// generated by AggFn::CodegenUpdateOrMergeFunction() when codegen is enabled.
  void Update(const TupleRow* row, Tuple* dst, void* fn);

  /// Sets up the arguments to call 'fn'. This converts from the agg-expr signature,
  /// taking TupleRow to the UDA signature taking AnyVals. Writes the serialize/finalize
  /// result to the given destination slot/tuple. 'fn' can be NULL to indicate the src
  /// value should simply be written into the destination. Note that StringVal result is
  /// from local allocation (which will be freed in the next QueryMaintenance()) so it
  /// needs to be copied out if it needs to survive beyond QueryMaintenance() (e.g. if
  /// 'dst' lives in a row batch).
  void SerializeOrFinalize(Tuple* src, const SlotDescriptor& dst_slot_desc,
      Tuple* dst, void* fn);

  /// Writes the result in src into dst pointed to by dst_slot_desc
  inline void SetDstSlot(
      const impala_udf::AnyVal* src, const SlotDescriptor& dst_slot_desc, Tuple* dst);
};

inline void AggFnEvaluator::Add(const TupleRow* row, Tuple* dst) {
  agg_fn_ctx_->impl()->IncrementNumUpdates();
  Update(row, dst, agg_fn_.merge_or_update_fn());
}

inline void AggFnEvaluator::Remove(const TupleRow* row, Tuple* dst) {
  agg_fn_ctx_->impl()->IncrementNumRemoves();
  Update(row, dst, agg_fn_.remove_fn());
}

inline void AggFnEvaluator::Serialize(Tuple* tuple) {
  SerializeOrFinalize(tuple, agg_fn_.intermediate_slot_desc(), tuple,
      agg_fn_.serialize_fn());
}

inline void AggFnEvaluator::Finalize(Tuple* agg_val, Tuple* output_val) {
  SerializeOrFinalize(agg_val, agg_fn_.output_slot_desc(), output_val,
      agg_fn_.finalize_fn());
}

inline void AggFnEvaluator::GetValue(Tuple* src, Tuple* dst) {
  SerializeOrFinalize(src, agg_fn_.output_slot_desc(), dst,
      agg_fn_.get_value_fn());
}

inline void AggFnEvaluator::Init(const std::vector<AggFnEvaluator*>& evaluators, Tuple* dst) {
  for (int i = 0; i < evaluators.size(); ++i) evaluators[i]->Init(dst);
}

inline void AggFnEvaluator::Add(const std::vector<AggFnEvaluator*>& evaluators,
    const TupleRow* src, Tuple* dst) {
  for (int i = 0; i < evaluators.size(); ++i) evaluators[i]->Add(src, dst);
}

inline void AggFnEvaluator::Remove(const std::vector<AggFnEvaluator*>& evaluators,
    const TupleRow* src, Tuple* dst) {
  for (int i = 0; i < evaluators.size(); ++i) evaluators[i]->Remove(src, dst);
}

inline void AggFnEvaluator::Serialize(const std::vector<AggFnEvaluator*>& evaluators,
    Tuple* dst) {
  for (int i = 0; i < evaluators.size(); ++i) evaluators[i]->Serialize(dst);
}

inline void AggFnEvaluator::GetValue(const std::vector<AggFnEvaluator*>& evaluators,
    Tuple* src, Tuple* dst) {
  for (int i = 0; i < evaluators.size(); ++i) evaluators[i]->GetValue(src, dst);
}

inline void AggFnEvaluator::Finalize(const std::vector<AggFnEvaluator*>& evaluators,
    Tuple* agg_val, Tuple* output_val) {
  for (int i = 0; i < evaluators.size(); ++i) {
    evaluators[i]->Finalize(agg_val, output_val);
  }
}



}

#endif
