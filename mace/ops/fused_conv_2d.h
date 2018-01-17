//
// Copyright (c) 2017 XiaoMi All rights reserved.
//

#ifndef MACE_OPS_FUSED_CONV_2D_H_
#define MACE_OPS_FUSED_CONV_2D_H_

#include <memory>

#include "mace/core/operator.h"
#include "mace/kernels/conv_2d.h"
#include "mace/ops/conv_pool_2d_base.h"

namespace mace {

template <DeviceType D, typename T>
class FusedConv2dOp : public ConvPool2dOpBase<D, T> {
 public:
  FusedConv2dOp(const OperatorDef &op_def, Workspace *ws)
      : ConvPool2dOpBase<D, T>(op_def, ws),
        functor_(this->strides_.data(),
                 this->padding_,
                 this->dilations_.data(),
                 kernels::StringToActivationType(
                     OperatorBase::GetSingleArgument<std::string>("activation",
                                                                  "NOOP")),
                 OperatorBase::GetSingleArgument<float>("max_limit", 0.0f),
                 OperatorBase::GetSingleArgument<float>("alpha", 0.0f)) {}

  bool Run(StatsFuture *future) override {
    const Tensor *input = this->Input(INPUT);
    const Tensor *filter = this->Input(FILTER);
    const Tensor *bias = this->InputSize() > 2 ? this->Input(BIAS) : nullptr;
    Tensor *output = this->Output(OUTPUT);

    functor_(input, filter, bias, output, future);

    return true;
  }

 private:
  kernels::Conv2dFunctor<D, T> functor_;

 protected:
  OP_INPUT_TAGS(INPUT, FILTER, BIAS);
  OP_OUTPUT_TAGS(OUTPUT);
};

}  // namespace mace

#endif  // MACE_OPS_FUSED_CONV_2D_H_
