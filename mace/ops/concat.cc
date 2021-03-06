// Copyright 2018 The MACE Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "mace/core/operator.h"
#include "mace/utils/quantize.h"
#include "mace/utils/memory.h"

#ifdef MACE_ENABLE_OPENCL
#include "mace/ops/opencl/image/concat.h"
#endif  // MACE_ENABLE_OPENCL

namespace mace {
namespace ops {

class ConcatOpBase : public Operation {
 public:
  explicit ConcatOpBase(OpConstructContext *context)
      : Operation(context),
        axis_(Operation::GetOptionalArg<int>("axis", 3)),
        checked_(false) {}

 protected:
  void Validate() {
    const int32_t input_dims = this->Input(0)->dim_size();
    axis_ =
        axis_ < 0 ? axis_ + input_dims : axis_;
    MACE_CHECK((0 <= axis_ && axis_ < input_dims),
               "Expected concatenating axis in the range [", -input_dims, ", ",
               input_dims, "], but got ", axis_);
  }

 protected:
  int axis_;
  bool checked_;
};

template <DeviceType D, class T>
class ConcatOp;

template <typename T>
class ConcatOp<DeviceType::CPU, T> : public ConcatOpBase {
 public:
  explicit ConcatOp(OpConstructContext *context)
      : ConcatOpBase(context) {}

  MaceStatus Run(OpContext *context) override {
    MACE_UNUSED(context);
    if (!checked_) {
      Validate();
      auto has_df = Operation::GetOptionalArg<int>(
          "has_data_format", 0);
      if (has_df && this->Input(0)->dim_size() == 4) {
        if (axis_ == 3) axis_ = 1;
        else if (axis_ == 2) axis_ = 3;
        else if (axis_ == 1) axis_ = 2;
      }
      checked_ = true;
    }
    const std::vector<const Tensor *> &inputs = this->Inputs();
    Tensor *output = this->Output(0);
    const Tensor *input0 = inputs.front();
    const size_t inputs_count = inputs.size();

    std::vector<index_t> output_shape(input0->shape());
    index_t inner_size = 1;
    for (int i = 0; i < axis_; ++i) {
      inner_size *= output_shape[i];
    }
    std::vector<index_t> outer_sizes(inputs_count, 0);
    outer_sizes[0] = input0->size() / inner_size;
    for (size_t i = 1; i < inputs_count; ++i) {
      const Tensor *input = inputs[i];
      MACE_CHECK(input->dim_size() == input0->dim_size(),
                 "Ranks of all input tensors must be same.");
      for (int j = 0; j < input->dim_size(); ++j) {
        if (j == axis_) {
          continue;
        }
        MACE_CHECK(input->dim(j) == input0->dim(j),
                   "Dimensions of inputs should equal except axis.");
      }
      outer_sizes[i] = input->size() / inner_size;
      output_shape[axis_] += input->dim(axis_);
    }
    MACE_RETURN_IF_ERROR(output->Resize(output_shape));

    T *output_ptr = output->mutable_data<T>();

    std::vector<const T *> input_ptrs(inputs.size(), nullptr);
    for (size_t i = 0; i < inputs_count; ++i) {
      input_ptrs[i] = inputs[i]->data<T>();
    }
    for (int inner_idx = 0; inner_idx < inner_size; ++inner_idx) {
      for (size_t i = 0; i < inputs_count; ++i) {
        if (DataTypeCanUseMemcpy(DataTypeToEnum<T>::v())) {
          memcpy(output_ptr, input_ptrs[i], outer_sizes[i] * sizeof(T));
          output_ptr += outer_sizes[i];
          input_ptrs[i] += outer_sizes[i];
        } else {
          for (index_t k = 0; k < outer_sizes[i]; ++k) {
            *output_ptr++ = *input_ptrs[i]++;
          }
        }
      }
    }

    return MaceStatus::MACE_SUCCESS;
  }
};

#ifdef MACE_ENABLE_QUANTIZE
template <>
class ConcatOp<DeviceType::CPU, uint8_t> : public ConcatOpBase {
 public:
  explicit ConcatOp(OpConstructContext *context)
      : ConcatOpBase(context) {}

  MaceStatus Run(OpContext *context) override {
    MACE_UNUSED(context);
    Validate();
    const std::vector<const Tensor *> &inputs = this->Inputs();
    Tensor *output = this->Output(0);
    MACE_CHECK(output->scale() != 0);
    const Tensor *input0 = inputs.front();
    const size_t inputs_count = inputs.size();

    std::vector<index_t> output_shape(input0->shape());
    index_t inner_size = 1;
    for (int i = 0; i < axis_; ++i) {
      inner_size *= output_shape[i];
    }
    std::vector<index_t> outer_sizes(inputs_count, 0);
    outer_sizes[0] = input0->size() / inner_size;
    for (size_t i = 1; i < inputs_count; ++i) {
      const Tensor *input = inputs[i];
      MACE_CHECK(input->dim_size() == input0->dim_size(),
                 "Ranks of all input tensors must be same.");
      for (int j = 0; j < input->dim_size(); ++j) {
        if (j == axis_) {
          continue;
        }
        MACE_CHECK(input->dim(j) == input0->dim(j),
                   "Dimensions of inputs should equal except axis.");
      }
      outer_sizes[i] = input->size() / inner_size;
      output_shape[axis_] += input->dim(axis_);
    }
    MACE_RETURN_IF_ERROR(output->Resize(output_shape));

    auto output_ptr = output->mutable_data<uint8_t>();

    std::vector<const uint8_t *> input_ptrs(inputs.size(), nullptr);
    for (size_t i = 0; i < inputs_count; ++i) {
      input_ptrs[i] = inputs[i]->data<uint8_t>();
    }

    for (int inner_idx = 0; inner_idx < inner_size; ++inner_idx) {
      for (size_t i = 0; i < inputs_count; ++i) {
        if (inputs[i]->zero_point() == output->zero_point()
            && inputs[i]->scale() == output->scale()) {
          memcpy(output_ptr, input_ptrs[i], outer_sizes[i] * sizeof(uint8_t));
          output_ptr += outer_sizes[i];
          input_ptrs[i] += outer_sizes[i];
        } else {
          const float scale = inputs[i]->scale() / output->scale();
          const float offset =
              -inputs[i]->zero_point() * scale + output->zero_point();
          for (index_t k = 0; k < outer_sizes[i]; ++k) {
            float out = (*input_ptrs[i]) * scale + offset;
            *output_ptr = Saturate<uint8_t>(roundf(out));
            ++output_ptr;
            ++input_ptrs[i];
          }
        }
      }
    }

    return MaceStatus::MACE_SUCCESS;
  }
};
#endif  // MACE_ENABLE_QUANTIZE

#ifdef MACE_ENABLE_OPENCL
template <typename T>
class ConcatOp<DeviceType::GPU, T> : public ConcatOpBase {
 public:
  explicit ConcatOp(OpConstructContext *context)
      : ConcatOpBase(context) {
    if (context->device()->gpu_runtime()->UseImageMemory()) {
      kernel_ = make_unique<opencl::image::ConcatKernel<T>>(axis_);
    } else {
      MACE_NOT_IMPLEMENTED;
    }
  }
  MaceStatus Run(OpContext *context) override {
    Validate();
    Tensor *output = this->Output(0);
    return kernel_->Compute(context, inputs_, output);
  }

 private:
  std::unique_ptr<OpenCLConcatKernel> kernel_;
};
#endif  // MACE_ENABLE_OPENCL


void RegisterConcat(OpRegistryBase *op_registry) {
  MACE_REGISTER_OP(op_registry, "Concat", ConcatOp,
                   DeviceType::CPU, float);

  MACE_REGISTER_OP(op_registry, "Concat", ConcatOp,
                   DeviceType::CPU, int32_t);

#ifdef MACE_ENABLE_QUANTIZE
  MACE_REGISTER_OP(op_registry, "Concat", ConcatOp,
                   DeviceType::CPU, uint8_t);
#endif  // MACE_ENABLE_QUANTIZE

#ifdef MACE_ENABLE_OPENCL
  MACE_REGISTER_OP(op_registry, "Concat", ConcatOp,
                   DeviceType::GPU, float);

  MACE_REGISTER_OP(op_registry, "Concat", ConcatOp,
                   DeviceType::GPU, half);

#endif  // MACE_ENABLE_OPENCL

  MACE_REGISTER_OP_CONDITION(
      op_registry,
      OpConditionBuilder("Concat")
          .SetDevicePlacerFunc(
            [](OpConstructContext *context) -> std::set<DeviceType> {
              auto op = context->operator_def();
              auto tensor_shape_info = context->tensor_shape_info();
              if (op->output_shape_size() != op->output_size()) {
                return { DeviceType::CPU, DeviceType::GPU };
              }
              if (op->output_shape(0).dims_size() != 4) {
                return { DeviceType::CPU };
              } else {
                int has_data_format =
                    ProtoArgHelper::GetOptionalArg<OperatorDef, int>(
                        *op, "has_data_format", 0);
                int axis = ProtoArgHelper::GetOptionalArg<OperatorDef, int>(
                    *op, "axis", 3);
                if (!has_data_format || axis != 3) {
                  return { DeviceType::CPU };
                }
                bool divisible_four = true;
                for (const std::string &input : op->input()) {
                  if (tensor_shape_info->find(input)
                      != tensor_shape_info->end()) {
                    divisible_four = divisible_four
                        && (tensor_shape_info->at(input)[3] % 4 == 0);
                  }
                }
                // Only support not divisible 4 case with 2 inputs.
                if (op->input_size() > 2 && !divisible_four) {
                  return { DeviceType::CPU };
                }
              }
              return { DeviceType::CPU, DeviceType::GPU };
            }));
}

}  // namespace ops
}  // namespace mace
