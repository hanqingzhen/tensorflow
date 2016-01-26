/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/data_flow_ops.cc.

#include <deque>
#include <vector>

#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/padding_fifo_queue.h"
#include "tensorflow/core/kernels/queue_base.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

PaddingFIFOQueue::PaddingFIFOQueue(
    int capacity, const DataTypeVector& component_dtypes,
    const std::vector<PartialTensorShape>& partial_shapes, const string& name)
    : FIFOQueue(capacity, component_dtypes,
                ConvertShapesPartialDimensionsToZero(partial_shapes), name),
      partial_shapes_(partial_shapes) {}

Status PaddingFIFOQueue::Initialize() {
  Status s = FIFOQueue::Initialize();
  if (!s.ok()) return s;

  if (component_dtypes_.size() != partial_shapes_.size()) {
    return errors::InvalidArgument(
        "Shapes must be provided for all components, but received ",
        component_dtypes_.size(), " dtypes and ", partial_shapes_.size(),
        " shapes.");
  }

  return Status::OK();
}

/* static */
Status PaddingFIFOQueue::GetElementComponent(
    const PaddingFIFOQueue::Tuple& tuple, int component, OpKernelContext* ctx,
    PersistentTensor* out_tensor) {
  TensorShape element_shape(tuple[component].shape());
  Tensor* element_access = nullptr;
  TF_RETURN_IF_ERROR(ctx->allocate_persistent(
      tuple[component].dtype(), element_shape, out_tensor, &element_access));
  *element_access = tuple[component];
  return Status::OK();
}

void PaddingFIFOQueue::TryDequeueMany(int num_elements, OpKernelContext* ctx,
                                      CallbackWithTuple callback) {
  if (num_elements == 0) {
    Tuple tuple;
    tuple.reserve(num_components());
    for (int i = 0; i < num_components(); ++i) {
      // TODO(josh11b,misard): Switch to allocate_output().
      // See similar comment in fifo_queue.cc
      Tensor element;
      // Here, ManyOutShape returns zeros for undetermined shapes,
      // which is exactly what we want to use.
      ctx->allocate_temp(component_dtypes_[i], ManyOutShape(i, 0), &element);
      tuple.emplace_back(element);
    }
    callback(tuple);
    return;
  }

  CancellationManager* cm = ctx->cancellation_manager();
  CancellationToken token = cm->get_cancellation_token();
  bool already_cancelled;
  {
    mutex_lock l(mu_);
    already_cancelled = !cm->RegisterCallback(
        token, [this, cm, token]() { Cancel(kDequeue, cm, token); });
    if (!already_cancelled) {
      // TODO(josh11b): This makes two copies of callback, avoid this if possible.
      dequeue_attempts_.emplace_back(
          num_elements, [callback]() { callback(Tuple()); }, ctx, cm, token,
          [callback, this](Attempt* attempt) EXCLUSIVE_LOCKS_REQUIRED(mu_) {
            int32 s = queues_[0].size();
            if (closed_ && s < attempt->elements_requested) {
              attempt->context->SetStatus(errors::OutOfRange(
                  "PaddingFIFOQueue '", name_, "' is closed and has ",
                  "insufficient elements (requested ",
                  attempt->elements_requested, ", current size ", s, ")"));

              // TODO(mrry): Add support for producing a partial batch as
              // output when the queue is closed.
              if (!attempt->tuples.empty()) {
                // Restore already-dequeued elements to the front of the queue.
                for (int64 i = attempt->tuples.size() - 1; i >= 0; --i) {
                  for (int j = 0; j < num_components(); ++j) {
                    PersistentTensor element;
                    Status s = GetElementComponent(attempt->tuples[i], j,
                                                   attempt->context, &element);
                    if (!s.ok()) {
                      attempt->context->SetStatus(
                          errors::DataLoss("Failed to restore element from "
                                           "partially-dequeued batch "
                                           "to PaddingFIFOQueue: ",
                                           s.error_message()));
                    }
                    queues_[j].push_front(element);
                  }
                }
              }
              return kComplete;
            }

            RunResult result = kNoProgress;
            for (; s > 0; --s) {
              result = kProgress;
              Tuple tuple;
              DequeueLocked(attempt->context, &tuple);
              attempt->tuples.push_back(tuple);
              tuple.clear();
              --attempt->elements_requested;

              if (attempt->elements_requested == 0) {
                // Finished.  Allocate attempt->tuple and
                // copy from attempt->tuples to attempt->tuple.
                attempt->tuple.reserve(num_components());
                const std::vector<Tuple>& tuples = attempt->tuples;

                std::vector<bool> dynamic_shape;
                const int64 batch_size = tuples.size();

                for (int i = 0; i < num_components(); ++i) {
                  const PartialTensorShape partial_shape =
                      PartialTensorShape({batch_size})
                          .Concatenate(partial_shapes_[i]);
                  TensorShape shape({batch_size});

                  for (int j = 0; j < partial_shape.dims() - 1; ++j) {
                    if (partial_shape.dim_size(j + 1) > -1) {
                      shape.AddDim(partial_shape.dim_size(j + 1));
                    } else {
                      // Expand sizes to match.
                      int64 max_val = 0;
                      for (const Tuple& t : tuples) {
                        max_val = std::max(max_val, t[i].shape().dim_size(j));
                      }
                      shape.AddDim(max_val);
                    }
                  }

                  Tensor element;
                  attempt->context->allocate_temp(component_dtypes_[i], shape,
                                                  &element);

                  bool has_dynamic_shape = !partial_shape.IsFullyDefined();
                  if (has_dynamic_shape) {
                    // Set all values to zero because not all values
                    // will get written over.
                    attempt->context->SetStatus(SetElementZero(&element));
                    if (!attempt->context->status().ok()) return kComplete;
                  }

                  dynamic_shape.push_back(has_dynamic_shape);

                  // TODO(ebrevdo): should this be a persistent tensor?
                  attempt->tuple.emplace_back(element);
                }

                for (size_t index = 0; index < tuples.size(); ++index) {
                  for (int i = 0; i < num_components(); ++i) {
                    if (dynamic_shape[i]) {
                      // Slightly slower copy operation
                      attempt->context->SetStatus(CopyElementToLargerSlice(
                          tuples[index][i], &attempt->tuple[i], index));
                    } else {
                      attempt->context->SetStatus(CopyElementToSlice(
                          tuples[index][i], &attempt->tuple[i], index));
                    }
                    if (!attempt->context->status().ok()) return kComplete;
                  }
                }
                tuple = attempt->tuple;
                attempt->tuples.clear();
                attempt->done_callback = [callback, tuple]() {
                  callback(tuple);
                };
                return kComplete;
              }
            }
            return result;
          });
    }
  }
  if (!already_cancelled) {
    FlushUnlocked();
  } else {
    ctx->SetStatus(errors::Cancelled("Dequeue operation was cancelled"));
    callback(Tuple());
  }
}

Status PaddingFIFOQueue::ValidateTuple(const Tuple& tuple) {
  TF_RETURN_IF_ERROR(ValidateTupleCommon(tuple));
  for (size_t i = 0; i < tuple.size(); ++i) {
    if (!partial_shapes_[i].IsCompatibleWith(tuple[i].shape())) {
      return errors::InvalidArgument("Shape mismatch in tuple component ", i,
                                     ". Expected ",
                                     partial_shapes_[i].DebugString(), ", got ",
                                     tuple[i].shape().DebugString());
    }
  }
  return Status::OK();
}

Status PaddingFIFOQueue::ValidateManyTuple(const Tuple& tuple) {
  TF_RETURN_IF_ERROR(ValidateTupleCommon(tuple));
  const int64 batch_size = tuple[0].dim_size(0);
  for (size_t i = 0; i < tuple.size(); ++i) {
    // Expected shape is [batch_size] + partial_shapes_[i]
    const PartialTensorShape expected_shape =
        PartialTensorShape({batch_size}).Concatenate(partial_shapes_[i]);
    if (!expected_shape.IsCompatibleWith(tuple[i].shape())) {
      return errors::InvalidArgument("Shape mismatch in tuple component ", i,
                                     ". Expected ",
                                     expected_shape.DebugString(), ", got ",
                                     tuple[i].shape().DebugString());
    }
  }
  return Status::OK();
}

Status PaddingFIFOQueue::CompatibleNodeDefShapes(
    const NodeDef& node_def) const {
  std::vector<PartialTensorShape> requested_shapes;
  TF_RETURN_IF_ERROR(GetNodeAttr(node_def, "shapes", &requested_shapes));
  if (!PartialTensorShapeUtils::AreCompatible(requested_shapes,
                                              partial_shapes_)) {
    return errors::InvalidArgument(
        "Shared queue '", name_, "' has component shapes ",
        PartialTensorShapeUtils::PartialShapeListString(partial_shapes_),
        " but requested component shapes were ",
        PartialTensorShapeUtils::PartialShapeListString(requested_shapes));
  } else {
    return Status::OK();
  }
}

Status PaddingFIFOQueue::MatchesNodeDef(const NodeDef& node_def) {
  TF_RETURN_IF_ERROR(MatchesNodeDefOp(node_def, "PaddingFIFOQueue"));
  TF_RETURN_IF_ERROR(MatchesNodeDefCapacity(node_def, capacity_));
  TF_RETURN_IF_ERROR(MatchesNodeDefTypes(node_def));
  TF_RETURN_IF_ERROR(CompatibleNodeDefShapes(node_def));
  return Status::OK();
}

template <typename T, int NDIMS>
Status HandleElementToLargerSlice(const Tensor& element, Tensor* parent,
                                  int index) {
  DCHECK_NE(parent->dim_size(0), 0);
  if (element.NumElements() > (parent->NumElements() / parent->dim_size(0))) {
    TensorShape chip_shape = parent->shape();
    chip_shape.RemoveDim(0);
    return errors::Internal(
        "HandleElementToLargerSlice Cannot copy slice: number of entries in "
        "element is greater than number of elements in parent slice.  ",
        "Shapes are: [element]: ", element.shape().DebugString(),
        ", [parent slice]: ", chip_shape.DebugString());
  }
  auto element_t = element.tensor<T, NDIMS>();
  auto parent_t = parent->tensor<T, NDIMS + 1>();
  Eigen::DSizes<Eigen::DenseIndex, NDIMS + 1> slice_indices;
  slice_indices[0] = index;
  Eigen::DSizes<Eigen::DenseIndex, NDIMS + 1> slice_size;
  slice_size[0] = 1;
  for (int i = 1; i < slice_size.size(); ++i) {
    slice_size[i] = element_t.dimension(i - 1);
  }
  parent_t.slice(slice_indices, slice_size) = element_t.reshape(slice_size);
  return Status::OK();
}

namespace {

template <int NDIMS>
Status HandleElementToLargerSliceWithRank(const Tensor& element, Tensor* parent,
                                          int index) {
#define HANDLE_TYPE(T)                                                   \
  case DataTypeToEnum<T>::value: {                                       \
    return HandleElementToLargerSlice<T, NDIMS>(element, parent, index); \
  }

  switch (element.dtype()) {
    TF_CALL_ALL_TYPES(HANDLE_TYPE);
#undef HANDLE_TYPE
    default:
      return errors::Unimplemented(
          "HandleElementToLargerSliceWithRank Unhandled data type: ",
          element.dtype());
  }
}

}  // namespace

Status PaddingFIFOQueue::CopyElementToLargerSlice(const Tensor& element,
                                                  Tensor* parent, int index) {
  if (parent->dims() != element.dims() + 1) {
    return errors::Internal(
        "Mismatched ranks.  Element's rank is: ", element.dims(),
        " but element is meant to be a slice in output Tensor having rank: ",
        parent->dims(), " (should be: ", element.dims() + 1, ")");
  }

#define HANDLE_DIMS(NDIMS)                                                  \
  case NDIMS: {                                                             \
    TF_RETURN_IF_ERROR(                                                     \
        HandleElementToLargerSliceWithRank<NDIMS>(element, parent, index)); \
    return Status::OK();                                                    \
  }

  switch (element.dims()) {
    HANDLE_DIMS(0);
    HANDLE_DIMS(1);
    HANDLE_DIMS(2);
    HANDLE_DIMS(3);
    HANDLE_DIMS(4);
#undef HANDLE_DIMS
    default:
      return errors::Unimplemented("CopyElementToLargerSlice Unhandled rank: ",
                                   element.dims());
  }
}

// Static method
Status PaddingFIFOQueue::SetElementZero(Tensor* element) {
#define HANDLE_TYPE(T)                                \
  if (element->dtype() == DataTypeToEnum<T>::value) { \
    element->flat<T>().setConstant(T());              \
    return Status::OK();                              \
  }
  TF_CALL_ALL_TYPES(HANDLE_TYPE);
#undef HANDLE_TYPE
  return errors::Unimplemented("SetElementZero Unhandled data type: ",
                               element->dtype());
}

std::vector<TensorShape> PaddingFIFOQueue::ConvertShapesPartialDimensionsToZero(
    const gtl::ArraySlice<PartialTensorShape>& partial_shapes) {
  std::vector<TensorShape> shapes(partial_shapes.size());
  for (int i = 0; i < shapes.size(); ++i) {
    const PartialTensorShape& partial = partial_shapes[i];
    TensorShape& shape = shapes[i];
    for (int64 s : partial.dim_sizes()) shape.AddDim(s < 0 ? 0 : s);
  }
  return shapes;
}

}  // namespace tensorflow
