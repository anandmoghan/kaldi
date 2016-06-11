// nnet3/nnet-simple-component.cc

// Copyright      2016  Daniel Galvez
//                2016  Tom Ko

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "nnet3/nnet-cudnn-simple-component.h"
#include "nnet3/nnet-parse.h"
#include "cudamatrix/cudnn-utils.h"
#include "cudamatrix/cudnn-convolution.h"
#include <cudnn.h>
#include <numeric>
#include <functional>

namespace kaldi {
namespace nnet3 {

/* The following functions are used to fill up the stride array
   according to different tensor format. The order of the dimensions
   in the shape array is by default NCXYZ, but the corresponding
   position in the stride array determines the layout in the tensor.
*/

void array_strides_NXYZC(int n_dims, const int *shape, int *strides) {
  strides[0] = shape[1] * shape[2] * shape[3] * shape[4];
  strides[1] = 1;
  strides[2] = shape[1] * shape[3] * shape[4];
  strides[3] = shape[1] * shape[4];
  strides[4] = shape[1];
}

void array_strides_NXZYC(int n_dims, const int *shape, int *strides) {
  strides[0] = shape[1] * shape[2] * shape[3] * shape[4];
  strides[1] = 1;
  strides[2] = shape[1] * shape[3] * shape[4];
  strides[3] = shape[1];
  strides[4] = shape[1] * shape[3];
}

void array_strides_NCXYZ(int n_dims, const int *shape, int *strides) {
  strides[0] = shape[1] * shape[2] * shape[3] * shape[4];
  strides[1] = shape[2] * shape[3] * shape[4];
  strides[2] = shape[3] * shape[4];
  strides[3] = shape[4];
  strides[4] = 1;
}

void array_strides_NCXZY(int n_dims, const int *shape, int *strides) {
  strides[0] = shape[1] * shape[2] * shape[3] * shape[4];
  strides[1] = shape[2] * shape[3] * shape[4];
  strides[2] = shape[3] * shape[4];
  strides[3] = 1;
  strides[4] = shape[3];
}

void CuDNN3DConvolutionComponent::FillInputStrides(int32 n_dims,
                    const int32 *shape, int32 *strides) const {
  if (input_vectorization_ == kZyx)
    array_strides_NCXYZ(n_dims, shape, strides);
  else if (input_vectorization_ == kYzx)
    array_strides_NCXZY(n_dims, shape, strides);
  else
    KALDI_ERR << "Unknown or unsupported input vectorization order ";
}

void CuDNN3DConvolutionComponent::FillOutputStrides(int32 n_dims,
                      const int32 *shape, int32 *strides) const{
  array_strides_NXYZC(n_dims, shape, strides);
}


CuDNN3DConvolutionComponent::CuDNN3DConvolutionComponent() :
  UpdatableComponent(),
  input_x_dim_(0), input_y_dim_(0), input_z_dim_(0),
  input_num_filters_(0), num_filters_(0),
  work_space_(NULL), work_space_size_(0),
  is_gradient_(false), input_vectorization_(kZyx),
  forward_algo_(CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM),
  backward_filter_algo_(CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0),
  backward_data_algo_(CUDNN_CONVOLUTION_BWD_DATA_ALGO_0) {
    CUDNN_SAFE_CALL(cudnnCreateFilterDescriptor(&filter_desc_));
    CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&bias_desc_));
    CUDNN_SAFE_CALL(cudnnCreateConvolutionDescriptor(&conv_desc_));
}

CuDNN3DConvolutionComponent::CuDNN3DConvolutionComponent(
                       const CuDNN3DConvolutionComponent& component) :
  UpdatableComponent(component),
  input_x_dim_(component.input_x_dim_), input_y_dim_(component.input_y_dim_),
  input_z_dim_(component.input_z_dim_),
  input_num_filters_(component.input_num_filters_),
  filter_params_(component.filter_params_),
  bias_params_(component.bias_params_),
  num_filters_(component.num_filters_),
  // Don't copy workspace pointer; make a new one instead. We would
  // need this if we used multiple CUDA streams.
  work_space_(NULL),
  work_space_size_(component.work_space_size_),
  is_gradient_(component.is_gradient_),
  input_vectorization_(component.input_vectorization_),
  filter_desc_(cudnn::CopyFilterDesc(component.filter_desc_)),
  bias_desc_(cudnn::CopyTensorDesc(component.bias_desc_)),
  conv_desc_(cudnn::CopyConvolutionDesc(component.conv_desc_)),
  forward_algo_(component.forward_algo_),
  backward_filter_algo_(component.backward_filter_algo_),
  backward_data_algo_(component.backward_data_algo_)
 {
  if (work_space_size_ != 0) {
    work_space_ = CuDevice::Instantiate().Malloc(work_space_size_);
  }
}

CuDNN3DConvolutionComponent::~CuDNN3DConvolutionComponent() {
  CUDNN_SAFE_CALL(cudnnDestroyFilterDescriptor(filter_desc_));
  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(bias_desc_));
  CUDNN_SAFE_CALL(cudnnDestroyConvolutionDescriptor(conv_desc_));

  if(work_space_ != NULL) {
    CuDevice::Instantiate().Free(work_space_);
  }
}

void CuDNN3DConvolutionComponent::InitDescriptor(
    int32 filt_x_dim, int32 filt_y_dim, int32 filt_z_dim,
    int32 filt_x_stride, int32 filt_y_stride, int32 filt_z_stride,
    int32 pad_x_dim, int32 pad_y_dim, int32 pad_z_dim,
    int32 upscale_x_dim, int32 upscale_y_dim, int32 upscale_z_dim) {

  int32 filters[kConvolutionDimension_ + 2];
  filters[0] = num_filters_;
  filters[1] = input_num_filters_;
  filters[2] = filt_x_dim;
  filters[3] = filt_y_dim;
  filters[4] = filt_z_dim;

  // The tensor format in the filter is supposed to be NCXYZ
  // There is no "stride" option to specify another tensor format
  CUDNN_SAFE_CALL(cudnnCreateFilterDescriptor(&filter_desc_));
  CUDNN_SAFE_CALL(
    cudnnSetFilterNdDescriptor(filter_desc_,
                               cudnn::GetDataType(),
                               kConvolutionDimension_ + 2,
                               filters
                               )
  );

  int32 strides[kConvolutionDimension_];
  strides[0] = filt_x_stride;
  strides[1] = filt_y_stride;
  strides[2] = filt_z_stride;

  int32 upscales[kConvolutionDimension_];
  upscales[0] = upscale_x_dim;
  upscales[1] = upscale_y_dim;
  upscales[2] = upscale_z_dim;

  int32 padding[kConvolutionDimension_];
  padding[0] = pad_x_dim;
  padding[1] = pad_y_dim;
  padding[2] = pad_z_dim;

  CUDNN_SAFE_CALL(
    cudnnSetConvolutionNdDescriptor(conv_desc_,
                                    kConvolutionDimension_,
                                    padding,
                                    strides,
                                    upscales,
                                    CUDNN_CROSS_CORRELATION,
                                    cudnn::GetDataType())
  );

  int32 bias_dims[kConvolutionDimension_ + 2] = {
    1, num_filters_, 1, 1, 1
  };
  int32 bias_strides[kConvolutionDimension_ + 2];
  FillInputStrides(kConvolutionDimension_ + 2, bias_dims, bias_strides);

  CUDNN_SAFE_CALL(
    cudnnSetTensorNdDescriptor(bias_desc_,
                               cudnn::GetDataType(),
                               kConvolutionDimension_ + 2,
                               bias_dims,
                               bias_strides
                               )
  );
}

void CuDNN3DConvolutionComponent::Init(
    int32 input_x_dim, int32 input_y_dim, int32 input_z_dim,
    int32 filt_x_dim, int32 filt_y_dim, int32 filt_z_dim, int32 input_num_filters,
    int32 filt_x_stride, int32 filt_y_stride, int32 filt_z_stride,
    int32 pad_x_dim, int32 pad_y_dim, int32 pad_z_dim,
    int32 upscale_x_dim, int32 upscale_y_dim, int32 upscale_z_dim,
    TensorVectorizationType input_vectorization,
    std::string matrix_filename) {
  input_x_dim_ = input_x_dim;
  input_y_dim_ = input_y_dim;
  input_z_dim_ = input_z_dim;
  input_num_filters_ = input_num_filters;
  input_vectorization_ = input_vectorization;
  CuMatrix<BaseFloat> mat;
  ReadKaldiObject(matrix_filename, &mat);
  int32 filter_dim = input_num_filters_ * filt_x_dim * filt_y_dim * filt_z_dim;
  num_filters_ = mat.NumRows();
  KALDI_ASSERT(mat.NumCols() == (filter_dim + 1));
  filter_params_.Resize(num_filters_, filter_dim, kUndefined,
                        kStrideEqualNumCols);
  bias_params_.Resize(num_filters_, kUndefined);
  filter_params_.CopyFromMat(mat.Range(0, num_filters_, 0, filter_dim));
  bias_params_.CopyColFromMat(mat, filter_dim);

  InitDescriptor(
         filt_x_dim, filt_y_dim, filt_z_dim,
         filt_x_stride, filt_y_stride, filt_z_stride,
         pad_x_dim, pad_y_dim, pad_z_dim,
         upscale_x_dim, upscale_y_dim, upscale_z_dim);
}


void CuDNN3DConvolutionComponent::Init(
    int32 input_x_dim, int32 input_y_dim, int32 input_z_dim,
    int32 filt_x_dim, int32 filt_y_dim, int32 filt_z_dim, int32 input_num_filters,
    int32 filt_x_stride, int32 filt_y_stride, int32 filt_z_stride,
    int32 num_filters,
    int32 pad_x_dim, int32 pad_y_dim, int32 pad_z_dim,
    int32 upscale_x_dim, int32 upscale_y_dim, int32 upscale_z_dim,
    TensorVectorizationType input_vectorization,
    BaseFloat param_stddev, BaseFloat bias_stddev) {
  input_x_dim_ = input_x_dim;
  input_y_dim_ = input_y_dim;
  input_z_dim_ = input_z_dim;
  input_num_filters_ = input_num_filters;
  num_filters_ = num_filters;
  input_vectorization_ = input_vectorization;
  int32 filter_dim = input_num_filters_ * filt_x_dim * filt_y_dim * filt_z_dim;
  filter_params_.Resize(num_filters_, filter_dim, kUndefined,
                        kStrideEqualNumCols);
  bias_params_.Resize(num_filters_, kUndefined);
  KALDI_ASSERT(param_stddev >= 0.0 && bias_stddev >= 0.0);
  filter_params_.SetRandn();
  filter_params_.Scale(param_stddev);
  bias_params_.SetRandn();
  bias_params_.Scale(bias_stddev);

  InitDescriptor(
         filt_x_dim, filt_y_dim, filt_z_dim,
         filt_x_stride, filt_y_stride, filt_z_stride,
         pad_x_dim, pad_y_dim, pad_z_dim,
         upscale_x_dim, upscale_y_dim, upscale_z_dim);
}


void CuDNN3DConvolutionComponent::InitFromConfig(ConfigLine *cfl) {
  bool ok = true;
  std::string matrix_filename;
  int32 input_x_dim = -1, input_y_dim = -1, input_z_dim = -1,
    filt_x_dim = -1, filt_y_dim = -1, filt_z_dim = -1,
    filt_x_stride = -1, filt_y_stride = -1, filt_z_stride = -1,
    num_filters = -1,
    upscale_x_dim = -1, upscale_y_dim = -1, upscale_z_dim = -1,
    pad_x_dim = -1, pad_y_dim = -1, pad_z_dim = -1;

  InitLearningRatesFromConfig(cfl);
  ok = ok && cfl->GetValue("input-x-dim", &input_x_dim);
  ok = ok && cfl->GetValue("input-y-dim", &input_y_dim);
  ok = ok && cfl->GetValue("input-z-dim", &input_z_dim);
  ok = ok && cfl->GetValue("filt-x-dim", &filt_x_dim);
  ok = ok && cfl->GetValue("filt-y-dim", &filt_y_dim);
  ok = ok && cfl->GetValue("filt-z-dim", &filt_z_dim);
  // the config line uses "step" while the code uses "stride"
  // because we want to be more compatible with the original
  // ConvolutionComponent interface. Tis code uses "stride"
  // to be consistent with the naming in the cudnn documentation.
  ok = ok && cfl->GetValue("filt-x-step", &filt_x_stride);
  ok = ok && cfl->GetValue("filt-y-step", &filt_y_stride);
  ok = ok && cfl->GetValue("filt-z-step", &filt_z_stride);

  // optional argument
  int32 input_num_filters = 1;
  if(!cfl->GetValue("input-num-filters", &input_num_filters)) {
    input_num_filters = 1;
  }

  std::string input_vectorization_order = "zyx";
  if(!cfl->GetValue("input-vectorization-order", &input_vectorization_order)) {
    input_vectorization_order = "zyx";
  }
  TensorVectorizationType input_vectorization = kZyx;
  if (input_vectorization_order.compare("zyx") == 0) {
    input_vectorization = kZyx;
  } else if (input_vectorization_order.compare("yzx") == 0) {
    input_vectorization = kYzx;
  } else {
    KALDI_ERR << "Unknown or unsupported input vectorization order "
              << input_vectorization_order
              << " accepted candidates are 'yzx' and 'zyx'";
  }

  // upscale_<k>_dim is how many times to
  // repeat each output in the <k>th dimension. This is
  // usually used to do image synthesis. I think
  // this will not be useful to change for most
  // people. By default, it is set to all ones.
  if(!cfl->GetValue("upscale-x-dim", &upscale_x_dim)) {
    upscale_x_dim = 1;
  }
  if(!cfl->GetValue("upscale-y-dim", &upscale_y_dim)) {
    upscale_y_dim = 1;
  }
  if(!cfl->GetValue("upscale-z-dim", &upscale_z_dim)) {
    upscale_z_dim = 1;
  }

  // If zero padding is not explicitly specified, use no padding.
  pad_x_dim = 0;
  pad_y_dim = 0;
  pad_z_dim = 0;
  if(!cfl->GetValue("pad-x-dim", &pad_x_dim)) {
    pad_x_dim = 0;
  }
  if(!cfl->GetValue("pad-y-dim", &pad_y_dim)) {
    pad_y_dim = 0;
  }
  if(!cfl->GetValue("pad-z-dim", &pad_z_dim)) {
    pad_z_dim = 0;
  }

  if (cfl->GetValue("matrix", &matrix_filename)) {
    // initialize from prefined parameter matrix
    Init(input_x_dim, input_y_dim, input_z_dim,
         filt_x_dim, filt_y_dim, filt_z_dim, input_num_filters,
         filt_x_stride, filt_y_stride, filt_z_stride,
         pad_x_dim, pad_y_dim, pad_z_dim,
         upscale_x_dim, upscale_y_dim, upscale_z_dim,
         input_vectorization,
         matrix_filename);
  } else {
    ok = ok && cfl->GetValue("num-filters", &num_filters);
    if (!ok)
      KALDI_ERR << "Bad initializer " << cfl->WholeLine();
    // initialize from configuration
    int32 filter_input_dim = filt_x_dim * filt_y_dim * input_z_dim;
    BaseFloat param_stddev = 1.0 / std::sqrt(filter_input_dim), bias_stddev = 1.0;
    cfl->GetValue("param-stddev", &param_stddev);
    cfl->GetValue("bias-stddev", &bias_stddev);
    Init(input_x_dim, input_y_dim, input_z_dim,
         filt_x_dim, filt_y_dim, filt_z_dim, input_num_filters,
         filt_x_stride, filt_y_stride, filt_z_stride,
         num_filters,
         pad_x_dim, pad_y_dim, pad_z_dim,
         upscale_x_dim, upscale_y_dim, upscale_z_dim,
         input_vectorization,
         param_stddev, bias_stddev);
  }
  if (cfl->HasUnusedValues()) {
    KALDI_ERR << "Could not process these elements in initializer: "
              << cfl->UnusedValues();
  }
  if (!ok) {
    KALDI_ERR << "Bad initializer " << cfl->WholeLine();
  }
}


/**
 * Returns a three-element long vector where index 0 contains
 * the x output dimension, 1 contains the y output dimension,
 * and 2 contains the z output dimension.
 */
std::vector<int32> CuDNN3DConvolutionComponent::GetOutputDims() const {
  cudnnTensorDescriptor_t in_desc;
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&in_desc));
  int32 input_dims[kConvolutionDimension_ + 2] =
    {1,
     input_num_filters_,
     input_x_dim_,
     input_y_dim_,
     input_z_dim_,
    };
  int32 input_strides[kConvolutionDimension_ + 2];
  FillInputStrides(kConvolutionDimension_ + 2, input_dims, input_strides);
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(in_desc,
                                             cudnn::GetDataType(),
                                             kConvolutionDimension_ + 2,
                                             input_dims,
                                             input_strides
                                             )
                  );
  int32 output_dims[kConvolutionDimension_ + 2];
  CUDNN_SAFE_CALL(
    cudnnGetConvolutionNdForwardOutputDim(conv_desc_,
                                          in_desc,
                                          filter_desc_,
                                          kConvolutionDimension_ + 2,
                                          output_dims
                                          )
                  );

  KALDI_ASSERT(output_dims[0] == 1); // Sanity check: Only one element in fake batch.
  KALDI_ASSERT(output_dims[1] == num_filters_);
  // Skip the first two elements. We know what they will be.
  std::vector<int32> output_dims_vec(kConvolutionDimension_);
  for(int i = 0; i < output_dims_vec.size(); i++) {
    output_dims_vec[i] = output_dims[i + 2];
  }

  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(in_desc));
  return output_dims_vec;
}

int32 CuDNN3DConvolutionComponent::OutputDim() const {
  std::vector<int32> output_dims = GetOutputDims();
  int32 output_dim = num_filters_ *
    std::accumulate(output_dims.begin(), output_dims.end(), 1,
                    std::multiplies<int32>());
  return output_dim;
}


void CuDNN3DConvolutionComponent::Propagate(const ComponentPrecomputedIndexes *indexes,
                                            const CuMatrixBase<BaseFloat> &in,
                                            CuMatrixBase<BaseFloat> *out) const {

  BaseFloat f = filter_params_.FrobeniusNorm();
  KALDI_ASSERT(f == f);
  BaseFloat b = bias_params_.Sum();
  KALDI_ASSERT(b == b);

  KALDI_ASSERT(in.NumCols() == in.Stride());
  KALDI_ASSERT(out->NumCols() == out->Stride());

  cudnnTensorDescriptor_t in_desc;
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&in_desc));
  int32 input_dims[kConvolutionDimension_ + 2] = {in.NumRows(),
                                                  input_num_filters_,
                                                  input_x_dim_,
                                                  input_y_dim_,
                                                  input_z_dim_,
                                                 };

  KALDI_ASSERT(input_num_filters_ * input_z_dim_ * input_y_dim_ * input_x_dim_ ==
               in.Stride());
  int32 input_strides[kConvolutionDimension_ + 2];
  FillInputStrides(kConvolutionDimension_ + 2, input_dims, input_strides);
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(in_desc,
                                             cudnn::GetDataType(),
                                             // 3D convolution means:
                                             // batch dimension, channel, depth, height, and width.
                                             // thus the input tensor is 5 dimensional.
                                             kConvolutionDimension_ + 2,
                                             input_dims,
                                             input_strides
                                             ));

  std::vector<int32> output_dims_per_filter = GetOutputDims();
  int32 output_dims[kConvolutionDimension_ + 2] = {
    out->NumRows(),
    num_filters_,
    output_dims_per_filter[0],
    output_dims_per_filter[1],
    output_dims_per_filter[2]
  };
  KALDI_ASSERT(out->Stride() == num_filters_ * output_dims_per_filter[0] *
               output_dims_per_filter[1] * output_dims_per_filter[2]);

  int32 output_strides[kConvolutionDimension_ + 2];
  FillOutputStrides(kConvolutionDimension_ + 2, output_dims, output_strides);
  cudnnTensorDescriptor_t out_desc;
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&out_desc));
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(out_desc,
                                             cudnn::GetDataType(),
                                             kConvolutionDimension_ + 2,
                                             output_dims,
                                             output_strides
                                             )
                  );

  cudnn::ConvolutionForward(CuDevice::Instantiate().GetCudnnHandle(),
                            &cudnn::one,
                            in_desc, in.Data(),
                            filter_desc_, filter_params_.Data(),
                            conv_desc_,
                            forward_algo_,
                            work_space_, work_space_size_,
                            &cudnn::one,
                            out_desc, out->Data()
                            );

  CUDNN_SAFE_CALL(
    cudnnAddTensor(CuDevice::Instantiate().GetCudnnHandle(),
                   &cudnn::one,
                   bias_desc_,
                   bias_params_.Data(),
                   &cudnn::one,
                   out_desc,
                   out->Data()
                  )
                  );

  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(in_desc));
  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(out_desc));
}

void CuDNN3DConvolutionComponent::Backprop(const std::string &debug_info,
                                         const ComponentPrecomputedIndexes *indexes,
                                         const CuMatrixBase<BaseFloat> &in_value,
                                         const CuMatrixBase<BaseFloat> &, //out_value,
                                         const CuMatrixBase<BaseFloat> &out_deriv,
                                         Component *to_update_in,
                                         CuMatrixBase<BaseFloat> *in_deriv) const {
  CuDNN3DConvolutionComponent *to_update =
    dynamic_cast<CuDNN3DConvolutionComponent*>(to_update_in);

  BaseFloat f = filter_params_.FrobeniusNorm();
  KALDI_ASSERT(f == f);
  BaseFloat b = bias_params_.Sum();
  KALDI_ASSERT(b == b);

  std::vector<int32> output_dims = GetOutputDims();
  int32 out_deriv_dims[kConvolutionDimension_ + 2] = {
    out_deriv.NumRows(),
    num_filters_,
    output_dims[0],
    output_dims[1],
    output_dims[2]
  };
  KALDI_ASSERT(in_value.NumCols() == in_value.Stride());
  KALDI_ASSERT(in_value.NumCols() == input_num_filters_ * input_z_dim_ * input_y_dim_ * input_x_dim_);
  KALDI_ASSERT(out_deriv.Stride() == out_deriv.NumCols());
  KALDI_ASSERT(out_deriv.Stride() == num_filters_ * output_dims[0] * output_dims[1] * output_dims[2]);
  int32 out_deriv_strides[kConvolutionDimension_ + 2];
  FillOutputStrides(kConvolutionDimension_ + 2, out_deriv_dims, out_deriv_strides);

  cudnnTensorDescriptor_t out_deriv_desc;
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&out_deriv_desc));
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(out_deriv_desc,
                                             cudnn::GetDataType(),
                                             kConvolutionDimension_ + 2,
                                             out_deriv_dims,
                                             out_deriv_strides
                                             ));

  // Since the current cudnn ConvolutionBackwardData() only support 
  // NCXYZ tensor layout for the out_deriv tensor, so we have to
  // transform the tensor from its own format to NCXYZ, we need a dummy matrix
  // for doing that
  int32 out_deriv_strides_ncxyz[kConvolutionDimension_ + 2];
  array_strides_NCXYZ(kConvolutionDimension_ + 2,
                      out_deriv_dims, out_deriv_strides_ncxyz);
  cudnnTensorDescriptor_t out_deriv_ncxyz_desc;
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&out_deriv_ncxyz_desc));
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(out_deriv_ncxyz_desc,
                                             cudnn::GetDataType(),
                                             kConvolutionDimension_ + 2,
                                             out_deriv_dims,
                                             out_deriv_strides_ncxyz
                                             ));

  CuMatrix<BaseFloat>* out_deriv_ncxyz;
  out_deriv_ncxyz = new CuMatrix<BaseFloat>(
    out_deriv.NumRows(), out_deriv.NumCols(), kUndefined, kStrideEqualNumCols);
  CUDNN_SAFE_CALL(cudnnTransformTensor(CuDevice::Instantiate().GetCudnnHandle(),
                                       &cudnn::one,
                                       out_deriv_desc, out_deriv.Data(),
                                       &cudnn::zero,
                                       out_deriv_ncxyz_desc, out_deriv_ncxyz->Data()));

  int32 in_dims[kConvolutionDimension_ + 2] = {
    in_value.NumRows(),
    input_num_filters_,
    input_x_dim_,
    input_y_dim_,
    input_z_dim_
  };
  int32 in_strides[kConvolutionDimension_ + 2];
  FillInputStrides(kConvolutionDimension_ + 2, in_dims, in_strides);
  cudnnTensorDescriptor_t in_desc; // shared between in_value and in_deriv
  CUDNN_SAFE_CALL(cudnnCreateTensorDescriptor(&in_desc));
  CUDNN_SAFE_CALL(cudnnSetTensorNdDescriptor(in_desc,
                                             cudnn::GetDataType(),
                                             kConvolutionDimension_ + 2,
                                             in_dims,
                                             in_strides
                                             ));

  if (in_deriv) {
    cudnn::ConvolutionBackwardData(CuDevice::Instantiate().GetCudnnHandle(),
                                   &cudnn::one,
                                   filter_desc_, filter_params_.Data(),
                                   out_deriv_ncxyz_desc, out_deriv_ncxyz->Data(),
                                   conv_desc_,
                                   backward_data_algo_,
                                   work_space_, work_space_size_,
                                   &cudnn::one,
                                   in_desc, in_deriv->Data());
  }

  if (to_update) {
    to_update->Update(in_value, *out_deriv_ncxyz, in_desc, out_deriv_ncxyz_desc);
  }
  delete out_deriv_ncxyz;
  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(in_desc));
  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(out_deriv_desc));
  CUDNN_SAFE_CALL(cudnnDestroyTensorDescriptor(out_deriv_ncxyz_desc));
}

void CuDNN3DConvolutionComponent::Update(const CuMatrixBase<BaseFloat> &in_value,
                                         const CuMatrixBase<BaseFloat> &out_deriv,
                                         const cudnnTensorDescriptor_t in_desc,
                                         const cudnnTensorDescriptor_t out_deriv_desc) {
  cudnn::ConvolutionBackwardFilter(CuDevice::Instantiate().GetCudnnHandle(),
                                   &learning_rate_, // alpha
                                   in_desc, in_value.Data(),
                                   out_deriv_desc, out_deriv.Data(),
                                   conv_desc_,
                                   backward_filter_algo_,
                                   work_space_, work_space_size_,
                                   &cudnn::one, // beta
                                   filter_desc_, filter_params_.Data()
                                   );

  cudnn::ConvolutionBackwardBias(CuDevice::Instantiate().GetCudnnHandle(),
                                 &learning_rate_,
                                 out_deriv_desc,
                                 out_deriv.Data(),
                                 &cudnn::one,
                                 bias_desc_,
                                 bias_params_.Data()
                                 );
}

Component *CuDNN3DConvolutionComponent::Copy() const {
  CuDNN3DConvolutionComponent *ans = new CuDNN3DConvolutionComponent(*this);
  return ans;
}

void CuDNN3DConvolutionComponent::Read(std::istream &is, bool binary) {
  ReadUpdatableCommon(is, binary);  // Read opening tag and learning rate.
  ExpectToken(is, binary, "<InputXDim>");
  ReadBasicType(is, binary, &input_x_dim_);
  ExpectToken(is, binary, "<InputYDim>");
  ReadBasicType(is, binary, &input_y_dim_);
  ExpectToken(is, binary, "<InputZDim>");
  ReadBasicType(is, binary, &input_z_dim_);
  int32 filter_dims[kConvolutionDimension_ + 2];
  ExpectToken(is, binary, "<InputNumFilters>");
  ReadBasicType(is, binary, &input_num_filters_);
  filter_dims[1] = input_num_filters_;
  ExpectToken(is, binary, "<OutputNumFilters>");
  ReadBasicType(is, binary, &num_filters_);
  filter_dims[0] = num_filters_;
  ExpectToken(is, binary, "<FilterXDim>");
  ReadBasicType(is, binary, &filter_dims[2]);
  ExpectToken(is, binary, "<FilterYDim>");
  ReadBasicType(is, binary, &filter_dims[3]);
  ExpectToken(is, binary, "<FilterZDim>");
  ReadBasicType(is, binary, &filter_dims[4]);
  int32 padding[kConvolutionDimension_];
  ExpectToken(is, binary, "<FilterXPadding>");
  ReadBasicType(is, binary, &padding[0]);
  ExpectToken(is, binary, "<FilterYPadding>");
  ReadBasicType(is, binary, &padding[1]);
  ExpectToken(is, binary, "<FilterZPadding>");
  ReadBasicType(is, binary, &padding[2]);
  int32 strides[kConvolutionDimension_];
  ExpectToken(is, binary, "<FilterXStride>");
  ReadBasicType(is, binary, &strides[0]);
  ExpectToken(is, binary, "<FilterYStride>");
  ReadBasicType(is, binary, &strides[1]);
  ExpectToken(is, binary, "<FilterZStride>");
  ReadBasicType(is, binary, &strides[2]);
  int32 upscales[kConvolutionDimension_];
  ExpectToken(is, binary, "<FilterXUpscale>");
  ReadBasicType(is, binary, &upscales[0]);
  ExpectToken(is, binary, "<FilterYUpscale>");
  ReadBasicType(is, binary, &upscales[1]);
  ExpectToken(is, binary, "<FilterZUpscale>");
  ReadBasicType(is, binary, &upscales[2]);
  ExpectToken(is, binary, "<InputVectorization>");
  int32 input_vectorization;
  ReadBasicType(is, binary, &input_vectorization);
  input_vectorization_ = static_cast<TensorVectorizationType>(input_vectorization);
  ExpectToken(is, binary, "<FilterParams>");
  filter_params_.Read(is, binary);
  ExpectToken(is, binary, "<BiasParams>");
  bias_params_.Read(is, binary);
  ExpectToken(is, binary, "<IsGradient>");
  ReadBasicType(is, binary, &is_gradient_);
  ExpectToken(is, binary, "</CuDNN3DConvolutionComponent>");

  InitDescriptor(
         filter_dims[2], filter_dims[3], filter_dims[4],
         strides[0], strides[1], strides[2],
         padding[0], padding[1], padding[2],
         upscales[0], upscales[1], upscales[2]);
}

void CuDNN3DConvolutionComponent::Write(std::ostream &os, bool binary) const {
  WriteUpdatableCommon(os, binary);  // write opening tag and learning rate.
  WriteToken(os, binary, "<InputXDim>");
  WriteBasicType(os, binary, input_x_dim_);
  WriteToken(os, binary, "<InputYDim>");
  WriteBasicType(os, binary, input_y_dim_);
  WriteToken(os, binary, "<InputZDim>");
  WriteBasicType(os, binary, input_z_dim_);
  WriteToken(os, binary, "<InputNumFilters>");
  WriteBasicType(os, binary, input_num_filters_);
  WriteToken(os, binary, "<OutputNumFilters>");
  WriteBasicType(os, binary, num_filters_);
  int32 filter_dims[kConvolutionDimension_ + 2];
  int32 numDimensions;
  cudnnDataType_t float_type;
  CUDNN_SAFE_CALL(cudnnGetFilterNdDescriptor(filter_desc_,
                                             kConvolutionDimension_ + 2,
                                             &float_type,
                                             &numDimensions,
                                             filter_dims)
                  );
  // Note that filter_dims[0] == input_num_filters_ and
  // filter_dims[1] == num_filters_
  KALDI_ASSERT(filter_dims[1] == input_num_filters_);
  KALDI_ASSERT(filter_dims[0] == num_filters_);
  WriteToken(os, binary, "<FilterXDim>");
  WriteBasicType(os, binary, filter_dims[2]);
  WriteToken(os, binary, "<FilterYDim>");
  WriteBasicType(os, binary, filter_dims[3]);
  WriteToken(os, binary, "<FilterZDim>");
  WriteBasicType(os, binary, filter_dims[4]);
  int32 padding[kConvolutionDimension_];
  int32 strides[kConvolutionDimension_];
  int32 upscales[kConvolutionDimension_];
  cudnnConvolutionMode_t mode;
  CUDNN_SAFE_CALL(cudnnGetConvolutionNdDescriptor(conv_desc_,
                                  kConvolutionDimension_,
                                  &numDimensions,
                                  padding,
                                  strides,
                                  upscales,
                                  &mode,
                                  &float_type
                                  )
                  );
  KALDI_ASSERT(numDimensions == kConvolutionDimension_);
  KALDI_ASSERT(mode == CUDNN_CROSS_CORRELATION);
  KALDI_ASSERT(float_type == cudnn::GetDataType());
  WriteToken(os, binary, "<FilterXPadding>");
  WriteBasicType(os, binary, padding[0]);
  WriteToken(os, binary, "<FilterYPadding>");
  WriteBasicType(os, binary, padding[1]);
  WriteToken(os, binary, "<FilterZPadding>");
  WriteBasicType(os, binary, padding[2]);
  WriteToken(os, binary, "<FilterXStride>");
  WriteBasicType(os, binary, strides[0]);
  WriteToken(os, binary, "<FilterYStride>");
  WriteBasicType(os, binary, strides[1]);
  WriteToken(os, binary, "<FilterZStride>");
  WriteBasicType(os, binary, strides[2]);
  WriteToken(os, binary, "<FilterXUpscale>");
  WriteBasicType(os, binary, upscales[0]);
  WriteToken(os, binary, "<FilterYUpscale>");
  WriteBasicType(os, binary, upscales[1]);
  WriteToken(os, binary, "<FilterZUpscale>");
  WriteBasicType(os, binary, upscales[2]);
  WriteToken(os, binary, "<InputVectorization>");
  WriteBasicType(os, binary, static_cast<int32>(input_vectorization_));
  WriteToken(os, binary, "<FilterParams>");
  filter_params_.Write(os, binary);
  WriteToken(os, binary, "<BiasParams>");
  bias_params_.Write(os, binary);
  WriteToken(os, binary, "<IsGradient>");
  WriteBasicType(os, binary, is_gradient_);
  WriteToken(os, binary, "</CuDNN3DConvolutionComponent>");
}

void CuDNN3DConvolutionComponent::SetZero(bool treat_as_gradient) {
  if (treat_as_gradient) {
    SetActualLearningRate(1.0);
    is_gradient_ = true;
  }
  filter_params_.SetZero();
  bias_params_.SetZero();
}

BaseFloat CuDNN3DConvolutionComponent::DotProduct(const UpdatableComponent &other_in) const {
  const CuDNN3DConvolutionComponent *other =
    dynamic_cast<const CuDNN3DConvolutionComponent*>(&other_in);
  return TraceMatMat(filter_params_, other->filter_params_, kTrans) +
    VecVec(bias_params_, other->bias_params_);
}

std::string CuDNN3DConvolutionComponent::Info() const {
  int32 numDimensions;
  int32 pad_dims[kConvolutionDimension_];
  int32 stride_dims[kConvolutionDimension_];
  int32 upscale_dims[kConvolutionDimension_];
  cudnnConvolutionMode_t mode;
  cudnnDataType_t float_type;
  CUDNN_SAFE_CALL(cudnnGetConvolutionNdDescriptor(conv_desc_,
                                                  kConvolutionDimension_,
                                                  &numDimensions,
                                                  pad_dims,
                                                  stride_dims,
                                                  upscale_dims,
                                                  &mode,
                                                  &float_type)
                  );
  KALDI_ASSERT(float_type == cudnn::GetDataType());
  int32 filter_dims[kConvolutionDimension_ + 2];
  CUDNN_SAFE_CALL(cudnnGetFilterNdDescriptor(filter_desc_,
                                             kConvolutionDimension_ + 2,
                                             &float_type,
                                             &numDimensions,
                                             filter_dims)
                  );
  KALDI_ASSERT(float_type == cudnn::GetDataType());

  std::ostringstream stream;
  stream << UpdatableComponent::Info()
         << ", input-x-dim=" << input_x_dim_
         << ", input-y-dim=" << input_y_dim_
         << ", input-z-dim=" << input_z_dim_
         << ", filt-x-dim=" << filter_dims[2]
         << ", filt-y-dim=" << filter_dims[3]
         << ", filt-z-dim=" << filter_dims[4]
         << ", filt-x-step=" << stride_dims[0]
         << ", filt-y-step=" << stride_dims[1]
         << ", filt-z-step=" << stride_dims[2]
         << ", x-zero-pad=" << pad_dims[0]
         << ", y-zero-pad=" << pad_dims[1]
         << ", z-zero-pad=" << pad_dims[2]
         << ", x-upscale=" << upscale_dims[0]
         << ", y-upscale=" << upscale_dims[1]
         << ", z-upscale=" << upscale_dims[2]
         << ", input-vectorization=" << input_vectorization_
         << ", input-num-filters=" << input_num_filters_
         << ", num-filters=" << num_filters_;
  PrintParameterStats(stream, "filter-params", filter_params_);
  PrintParameterStats(stream, "bias-params", bias_params_, true);
  return stream.str();
}

int32 CuDNN3DConvolutionComponent::NumParameters() const {
  return filter_params_.NumCols() * filter_params_.NumRows() + bias_params_.Dim();
}

void CuDNN3DConvolutionComponent::Scale(BaseFloat scale) {
  filter_params_.Scale(scale);
  bias_params_.Scale(scale);
}

void CuDNN3DConvolutionComponent::Add(BaseFloat alpha, const Component &other_in) {
  const CuDNN3DConvolutionComponent *other =
      dynamic_cast<const CuDNN3DConvolutionComponent*>(&other_in);
  KALDI_ASSERT(other != NULL);
  filter_params_.AddMat(alpha, other->filter_params_);
  bias_params_.AddVec(alpha, other->bias_params_);
}

void CuDNN3DConvolutionComponent::Vectorize(VectorBase<BaseFloat> *params) const {
  KALDI_ASSERT(params->Dim() == this->NumParameters());
  int32 num_filter_params = filter_params_.NumCols() * filter_params_.NumRows();
  params->Range(0, num_filter_params).CopyRowsFromMat(filter_params_);
  params->Range(num_filter_params, bias_params_.Dim()).CopyFromVec(bias_params_);
}

void CuDNN3DConvolutionComponent::UnVectorize(const VectorBase<BaseFloat> &params) {
  KALDI_ASSERT(params.Dim() == this->NumParameters());
  int32 num_filter_params = filter_params_.NumCols() * filter_params_.NumRows();
  filter_params_.CopyRowsFromVec(params.Range(0, num_filter_params));
  bias_params_.CopyFromVec(params.Range(num_filter_params, bias_params_.Dim()));
}


void CuDNN3DConvolutionComponent::PerturbParams(BaseFloat stddev) {
  CuMatrix<BaseFloat> temp_filter_params(filter_params_);
  temp_filter_params.SetRandn();
  filter_params_.AddMat(stddev, temp_filter_params);

  CuVector<BaseFloat> temp_bias_params(bias_params_);
  temp_bias_params.SetRandn();
  bias_params_.AddVec(stddev, temp_bias_params);
}

} // namespace nnet3
} // namespace kaldi
