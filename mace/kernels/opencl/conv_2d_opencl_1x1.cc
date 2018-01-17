//
// Copyright (c) 2017 XiaoMi All rights reserved.
//

#include "mace/kernels/conv_2d.h"
#include "mace/core/runtime/opencl/cl2_header.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/kernels/activation.h"
#include "mace/kernels/opencl/helper.h"
#include "mace/utils/tuner.h"
#include "mace/utils/utils.h"

namespace mace {
namespace kernels {

void Conv1x1(const Tensor *input,
             const Tensor *filter,
             const Tensor *bias,
             const int stride,
             const ActivationType activation,
             const float relux_max_limit,
             const float prelu_alpha,
             const DataType dt,
             Tensor *output,
             StatsFuture *future) {
  const index_t batch = output->dim(0);
  const index_t height = output->dim(1);
  const index_t width = output->dim(2);
  const index_t channels = output->dim(3);
  const index_t input_batch = input->dim(0);
  const index_t input_height = input->dim(1);
  const index_t input_width = input->dim(2);
  const index_t input_channels = input->dim(3);

  const index_t channel_blocks = RoundUpDiv4(channels);
  const index_t width_blocks = RoundUpDiv4(width);
  const index_t input_channel_blocks = RoundUpDiv4(input_channels);

  MACE_CHECK(input_batch == batch);

  std::set<std::string> built_options;
  std::string kernel_name = MACE_OBFUSCATE_SYMBOL("conv_2d_1x1");
  built_options.emplace("-Dconv_2d_1x1=" + kernel_name);
  built_options.emplace("-DDATA_TYPE=" + DtToUpstreamCLDt(dt));
  built_options.emplace("-DCMD_DATA_TYPE=" + DtToUpstreamCLCMDDt(dt));
  built_options.emplace("-DSTRIDE=" + ToString(stride));
  if (bias != nullptr) {
    built_options.emplace("-DBIAS");
  }
  switch (activation) {
    case NOOP:
      break;
    case RELU:
      built_options.emplace("-DUSE_RELU");
      break;
    case RELUX:
      built_options.emplace("-DUSE_RELUX");
      break;
    case PRELU:
      built_options.emplace("-DUSE_PRELU");
      break;
    case TANH:
      built_options.emplace("-DUSE_TANH");
      break;
    case SIGMOID:
      built_options.emplace("-DUSE_SIGMOID");
      break;
    defeult:
      LOG(FATAL) << "Unknown activation type: " << activation;
  }

  auto runtime = OpenCLRuntime::Global();
  auto conv_2d_kernel =
      runtime->BuildKernel("conv_2d_1x1", kernel_name, built_options);

  uint32_t idx = 0;
  conv_2d_kernel.setArg(idx++,
                        *(static_cast<const cl::Image2D *>(input->buffer())));
  conv_2d_kernel.setArg(idx++,
                        *(static_cast<const cl::Image2D *>(filter->buffer())));
  if (bias != nullptr) {
    conv_2d_kernel.setArg(idx++,
                          *(static_cast<const cl::Image2D *>(bias->buffer())));
  }
  conv_2d_kernel.setArg(idx++,
                        *(static_cast<const cl::Image2D *>(output->buffer())));
  // FIXME handle flexable data type: half not supported
  conv_2d_kernel.setArg(idx++, relux_max_limit);
  conv_2d_kernel.setArg(idx++, prelu_alpha);
  conv_2d_kernel.setArg(idx++, static_cast<int>(input_height));
  conv_2d_kernel.setArg(idx++, static_cast<int>(input_width));
  conv_2d_kernel.setArg(idx++, static_cast<int>(input_channel_blocks));
  conv_2d_kernel.setArg(idx++, static_cast<int>(height));
  conv_2d_kernel.setArg(idx++, static_cast<int>(width));

  const uint32_t gws[3] = {static_cast<uint32_t>(channel_blocks),
                           static_cast<uint32_t>(width_blocks),
                           static_cast<uint32_t>(height * batch)};
  const std::vector<uint32_t> lws = {8, 15, 8};
  const uint32_t kwg_size = runtime->GetKernelMaxWorkGroupSize(conv_2d_kernel);
  auto params_generator = [&]() -> std::vector<std::vector<uint32_t>> {
    std::vector<uint32_t> local_ws(3, 0);
    local_ws[0] = std::min<uint32_t>(channel_blocks, kwg_size);
    local_ws[1] = std::min<uint32_t>(width_blocks, kwg_size / local_ws[0]);
    local_ws[2] = std::min<uint32_t>(height * batch,
                                     kwg_size / (local_ws[0] * local_ws[1]));
    return {
        {local_ws[0], local_ws[1], local_ws[2]},
        {kwg_size / 16, 4, 4},
        {kwg_size / 32, 4, 8},
        {kwg_size / 32, 8, 4},
        {kwg_size / 64, 8, 8},
        {kwg_size / 64, 16, 4},
        {kwg_size / 128, 8, 16},
        {kwg_size / 128, 16, 8},
        {kwg_size / 128, 32, 4},
        {1, kwg_size / 32, 32},
        {1, kwg_size / 64, 64},
        {1, kwg_size / 128, 128},
        {3, 15, 9},
        {7, 15, 9},
        {9, 7, 15},
        {15, 7, 9},
        {1, kwg_size, 1},
        {4, 15, 8},  // SNPE size
    };
  };
  cl::Event event;
  auto func = [&](const std::vector<uint32_t> &params) -> cl_int {
    cl_int error = runtime->command_queue().enqueueNDRangeKernel(
        conv_2d_kernel, cl::NullRange, cl::NDRange(gws[0], gws[1], gws[2]),
        cl::NDRange(params[0], params[1], params[2]), nullptr, &event);

    MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
    return error;
  };
  std::string tuning_key =
      Concat("conv2d_1x1_opencl_kernel_", activation, output->dim(0),
             output->dim(1), output->dim(2), output->dim(3));
  OpenCLProfilingTimer timer(&event);
  Tuner<uint32_t>::Get()->template TuneOrRun<cl_int>(
      tuning_key, lws, params_generator, func, &timer);
  SetFuture(future, event);
}

extern void Conv2dOpenclK1x1S1(const Tensor *input,
                               const Tensor *filter,
                               const Tensor *bias,
                               const int *padding,
                               const int *dilations,
                               const ActivationType activation,
                               const float relux_max_limit,
                               const float prelu_alpha,
                               const DataType dt,
                               Tensor *output,
                               StatsFuture *future) {
  Conv1x1(input, filter, bias, 1, activation, relux_max_limit, prelu_alpha, dt,
          output, future);
};

extern void Conv2dOpenclK1x1S2(const Tensor *input,
                               const Tensor *filter,
                               const Tensor *bias,
                               const int *padding,
                               const int *dilations,
                               const ActivationType activation,
                               const float relux_max_limit,
                               const float prelu_alpha,
                               const DataType dt,
                               Tensor *output,
                               StatsFuture *future) {
  Conv1x1(input, filter, bias, 2, activation, relux_max_limit, prelu_alpha, dt,
          output, future);
};

}  // namespace kernels
}  // namespace mace
