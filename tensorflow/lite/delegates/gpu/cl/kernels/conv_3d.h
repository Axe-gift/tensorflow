/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONV_3D_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONV_3D_H_

#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/buffer.h"
#include "tensorflow/lite/delegates/gpu/cl/cl_device.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/linear_storage.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"
#include "tensorflow/lite/delegates/gpu/cl/util.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class Conv3D : public GPUOperation {
 public:
  Conv3D() = default;
  void GetPossibleKernelWorkGroups(
      TuningType tuning_type, const DeviceInfo& device_info,
      const KernelInfo& kernel_info,
      std::vector<int3>* work_groups) const override;
  absl::Status BindArguments() override;
  int3 GetGridSize() const override;

  // Move only
  Conv3D(Conv3D&& operation);
  Conv3D& operator=(Conv3D&& operation);
  Conv3D(const Conv3D&) = delete;
  Conv3D& operator=(const Conv3D&) = delete;

 private:
  enum class WeightsUploadType {
    LOCAL_MEM_ASYNC_SUBGROUP,  // we use it for PowerVR with workgroup size = 32
    LOCAL_MEM_BY_THREADS,
    GLOBAL_MEM,
    TEXTURES_MEM,
  };

  struct ConvParams {
    int4 block_size;  // WHDS
    int3 work_group_launch_order;
    int src_depth_loop_size;
    WeightsUploadType weights_upload_type;
    bool AreWeightsBuffer() const {
      return weights_upload_type != WeightsUploadType::TEXTURES_MEM;
    }
    bool x_kernel_is_1;
    bool y_kernel_is_1;
    bool z_kernel_is_1;
  };

  Conv3D(const OperationDef& definition, const Convolution3DAttributes& attr,
         const CLDevice& device);

  template <DataType T>
  absl::Status UploadData(const tflite::gpu::Tensor<OHWDI, T>& weights,
                          const tflite::gpu::Tensor<Linear, T>& biases,
                          CLContext* context);
  template <DataType T>
  absl::Status UploadWeights(const tflite::gpu::Tensor<OHWDI, T>& weights,
                             CLContext* context);

  template <DataType S, typename T>
  void RearrangeWeightsData(const tflite::gpu::Tensor<OHWDI, S>& weights,
                            absl::Span<T> dst);

  friend absl::Status CreateConv3D(const CreationContext& creation_context,
                                   const OperationDef& definition,
                                   const Convolution3DAttributes& attr,
                                   Conv3D* result);

  friend std::string GenerateConv3D(const OperationDef& op_def,
                                    bool stride_correction,
                                    const ConvParams& conv_params,
                                    Arguments* args);

  ConvParams GuessBestParams(const CLDevice& device,
                             const OperationDef& definition,
                             const Convolution3DAttributes& attr);

  ConvParams GuessBestParams(const CLDevice& device,
                             const OperationDef& definition, int src_slices,
                             int dst_slices, bool x_kernel_is_1,
                             bool y_kernel_is_1, bool z_kernel_is_1);

  std::string GenerateConv3D(const OperationDef& op_def, bool stride_correction,
                             const Conv3D::ConvParams& conv_params);

  int3 stride_;
  int3 padding_;
  int3 kernel_size_;
  int3 dilation_;
  ConvParams conv_params_;
};

template <DataType T>
absl::Status Conv3D::UploadData(const tflite::gpu::Tensor<OHWDI, T>& weights,
                                const tflite::gpu::Tensor<Linear, T>& biases,
                                CLContext* context) {
  RETURN_IF_ERROR(UploadWeights(weights, context));
  TensorLinearDescriptor desc;
  desc.storage_type = conv_params_.AreWeightsBuffer()
                          ? LinearStorageType::BUFFER
                          : LinearStorageType::TEXTURE_2D;
  desc.element_type = definition_.GetDataType();

  LinearStorage lt;
  RETURN_IF_ERROR(CreateLinearStorage(desc, biases, context, &lt));
  args_.AddObject("biases", AccessType::READ,
                  absl::make_unique<LinearStorage>(std::move(lt)),
                  absl::make_unique<TensorLinearDescriptor>(desc));
  return absl::OkStatus();
}

template <DataType T>
absl::Status Conv3D::UploadWeights(const tflite::gpu::Tensor<OHWDI, T>& weights,
                                   CLContext* context) {
  const int block_size = conv_params_.block_size.w;
  const int dst_slices =
      AlignByN(DivideRoundUp(weights.shape.o, 4), block_size);
  const int src_slices = DivideRoundUp(weights.shape.i, 4);
  const int kernel_x = kernel_size_.x;
  const int kernel_y = kernel_size_.y;
  const int kernel_z = kernel_size_.z;
  const int texture_width = dst_slices;
  const int texture_height = src_slices * kernel_x * kernel_y * kernel_z;

  const int elements_count =
      kernel_x * kernel_y * kernel_z * src_slices * dst_slices * 4;
  const bool f32_weights = definition_.precision == CalculationsPrecision::F32;

  const int float4_size = f32_weights ? 16 : 8;

  Texture2D weights_0;
  Texture2D weights_1;
  Texture2D weights_2;
  Texture2D weights_3;
  Buffer weights_buf;
  if (f32_weights) {
    std::vector<float4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (conv_params_.AreWeightsBuffer()) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data(), context, &weights_0));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height, context,
          &weights_1));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height * 2, context,
          &weights_2));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height * 3, context,
          &weights_3));
    }
  } else {
    std::vector<half4> gpu_data(elements_count);
    RearrangeWeightsData(weights, absl::MakeSpan(gpu_data));
    if (conv_params_.AreWeightsBuffer()) {
      RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * elements_count,
                                           gpu_data.data(), context,
                                           &weights_buf));
    } else {
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data(), context, &weights_0));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height, context,
          &weights_1));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height * 2, context,
          &weights_2));
      RETURN_IF_ERROR(CreateTexture2DRGBA(
          definition_.GetDataType(), texture_width, texture_height,
          gpu_data.data() + texture_width * texture_height * 3, context,
          &weights_3));
    }
  }

  if (conv_params_.AreWeightsBuffer()) {
    BufferDescriptor desc;
    desc.element_type = f32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    desc.element_size = 4;
    args_.AddObject("weights", AccessType::READ,
                    absl::make_unique<Buffer>(std::move(weights_buf)),
                    absl::make_unique<BufferDescriptor>(desc));
  } else {
    Texture2DDescriptor desc;
    desc.element_type = f32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
    args_.AddObject("weights0", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_0)),
                    absl::make_unique<Texture2DDescriptor>(desc));
    args_.AddObject("weights1", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_1)),
                    absl::make_unique<Texture2DDescriptor>(desc));
    args_.AddObject("weights2", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_2)),
                    absl::make_unique<Texture2DDescriptor>(desc));
    args_.AddObject("weights3", AccessType::READ,
                    absl::make_unique<Texture2D>(std::move(weights_3)),
                    absl::make_unique<Texture2DDescriptor>(desc));
  }

  return absl::OkStatus();
}

template <DataType S, typename T>
void Conv3D::RearrangeWeightsData(const tflite::gpu::Tensor<OHWDI, S>& weights,
                                  absl::Span<T> dst) {
  const int block_size = conv_params_.block_size.w;
  const int dst_slices =
      AlignByN(DivideRoundUp(weights.shape.o, 4), block_size);
  const int src_slices = DivideRoundUp(weights.shape.i, 4);
  const int kernel_x = kernel_size_.x;
  const int kernel_y = kernel_size_.y;
  const int kernel_z = kernel_size_.z;
  const int texture_width = dst_slices;
  const int texture_height = src_slices * kernel_x * kernel_y * kernel_z;

  int counter = 0;
  for (int d = 0; d < dst_slices / block_size; ++d) {
    for (int z = 0; z < kernel_z; ++z) {
      for (int y = 0; y < kernel_y; ++y) {
        for (int x = 0; x < kernel_x; ++x) {
          for (int s = 0; s < src_slices; ++s) {
            for (int sub_d = 0; sub_d < block_size; ++sub_d) {
              T filters[4];
              for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                  const int s_ch = s * 4 + j;
                  const int d_ch = (d * block_size + sub_d) * 4 + i;
                  if (s_ch < weights.shape.i && d_ch < weights.shape.o) {
                    const int f_index =
                        weights.shape.LinearIndex({d_ch, y, x, z, s_ch});
                    filters[j][i] = weights.data[f_index];
                  } else {
                    filters[j][i] = 0.0f;
                  }
                }
              }
              if (conv_params_.AreWeightsBuffer()) {
                dst[counter++] = filters[0];
                dst[counter++] = filters[1];
                dst[counter++] = filters[2];
                dst[counter++] = filters[3];
              } else {
                int x_coord = d * block_size + sub_d;
                int y_coord =
                    ((z * kernel_y + y) * kernel_x + x) * src_slices + s;
                int offset = y_coord * dst_slices + x_coord;
                dst[offset + texture_width * texture_height * 0] = filters[0];
                dst[offset + texture_width * texture_height * 1] = filters[1];
                dst[offset + texture_width * texture_height * 2] = filters[2];
                dst[offset + texture_width * texture_height * 3] = filters[3];
              }
            }
          }
        }
      }
    }
  }
}

absl::Status CreateConv3D(const CreationContext& creation_context,
                          const OperationDef& definition,
                          const Convolution3DAttributes& attr, Conv3D* result);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONV_3D_H_
