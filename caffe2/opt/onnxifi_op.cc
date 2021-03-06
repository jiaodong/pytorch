#include "caffe2/opt/onnxifi_op.h"
#include "caffe2/operators/slice_op.h"
#include "caffe2/opt/bound_shape_inferencer.h"

namespace caffe2 {

namespace {

void SetInputTensorDescriptorTypeAndBuffer(
    const Tensor& cpu_tensor,
    onnxTensorDescriptorV1* desc) {
  if (cpu_tensor.template IsType<int32_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_INT32;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<int32_t>());
  } else if (cpu_tensor.template IsType<c10::Half>()) {
    desc->dataType = ONNXIFI_DATATYPE_FLOAT16;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<c10::Half>());
  } else if (cpu_tensor.template IsType<float>()) {
    desc->dataType = ONNXIFI_DATATYPE_FLOAT32;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<float>());
  } else if (cpu_tensor.template IsType<int8_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_INT8;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<int8_t>());
  } else if (cpu_tensor.template IsType<uint8_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_UINT8;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<uint8_t>());
  } else if (cpu_tensor.template IsType<int64_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_INT64;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<int64_t>());
  } else if (cpu_tensor.template IsType<int16_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_INT16;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<int16_t>());
  } else if (cpu_tensor.template IsType<uint16_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_UINT16;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<uint16_t>());
  } else {
    CAFFE_THROW(
        "Unsupported tensor type in ONNXIFI: ", cpu_tensor.dtype().name());
  }
}

void SetInputTensorDescriptorTypeAndBuffer(
    const int8::Int8TensorCPU& cpu_int8tensor,
    onnxTensorDescriptorV1* desc) {
  const Tensor& cpu_tensor = cpu_int8tensor.t;
  if (cpu_tensor.template IsType<uint8_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_UINT8;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<uint8_t>());
  } else if (cpu_tensor.template IsType<int32_t>()) {
    desc->dataType = ONNXIFI_DATATYPE_INT32;
    desc->buffer = reinterpret_cast<onnxPointer>(cpu_tensor.data<int32_t>());
  } else {
    CAFFE_THROW(
        "Unsupported Int8Tensor type in ONNXIFI: ", cpu_tensor.dtype().name());
  }
  desc->quantizationParams = 1;
  desc->quantizationAxis = 1;
  desc->scales = &cpu_int8tensor.scale;
  desc->biases = &cpu_int8tensor.zero_point;
}

TypeMeta OnnxifiTypeToDataType(uint64_t onnxifi_type) {
  static std::map<uint64_t, TypeMeta> data_type_map{
      {ONNXIFI_DATATYPE_FLOAT32, TypeMeta::Make<float>()},
      {ONNXIFI_DATATYPE_FLOAT16, TypeMeta::Make<c10::Half>()},
      {ONNXIFI_DATATYPE_INT32, TypeMeta::Make<int>()},
      {ONNXIFI_DATATYPE_INT8, TypeMeta::Make<int8_t>()},
      {ONNXIFI_DATATYPE_UINT8, TypeMeta::Make<uint8_t>()},
      {ONNXIFI_DATATYPE_INT64, TypeMeta::Make<int64_t>()},
      {ONNXIFI_DATATYPE_INT16, TypeMeta::Make<int16_t>()},
      {ONNXIFI_DATATYPE_UINT16, TypeMeta::Make<uint16_t>()},
  };
  const auto it = data_type_map.find(onnxifi_type);
  CAFFE_ENFORCE(
      it != data_type_map.end(),
      "Unsupported ONNXIFI data type: ",
      onnxifi_type);
  return it->second;
}

void SetOutputTensorDescriptorTypeAndBuffer(
    uint64_t onnxifi_type,
    Tensor* cpu_tensor,
    onnxTensorDescriptorV1* desc) {
  desc->dataType = onnxifi_type;
  desc->buffer = reinterpret_cast<onnxPointer>(
      cpu_tensor->raw_mutable_data(OnnxifiTypeToDataType(onnxifi_type)));
}

#ifndef C10_MOBILE
void copyDescriptor(
    const ExternalTensorDescriptor* from,
    onnxTensorDescriptorV1* to) {
  to->dataType = from->dataType;
  to->buffer = from->buffer;
  to->isOffline = from->isOffline;
  to->quantizationParams = from->quantizationParams;
  to->quantizationAxis = from->quantizationAxis;
  to->scales = from->scales;
  to->biases = from->biases;
  to->dimensions = from->dimensions;
  to->shape = from->shape;
}
#endif

void BlobToTensorDescriptor(
    const std::string& name,
    Workspace* ws,
    onnxTensorDescriptorV1* desc,
    std::vector<std::vector<uint64_t>>* shapes,
    std::vector<std::vector<float>>* all_scales,
    std::vector<std::vector<int32_t>>* all_offsets) {
  const Blob* blob = ws->GetBlob(name);
  CAFFE_ENFORCE(blob, "Blob ", name, " doesn't exist");
  const bool is_int8tensor =
      blob->meta().id() == TypeMeta::Id<int8::Int8TensorCPU>();
  bool is_external_tensor;
#ifndef C10_MOBILE
  auto function_ptr =
      ExternalTensorFunctionsBaseRegistry()->Create(blob->meta().id());
  is_external_tensor = function_ptr != nullptr;
#else
  is_external_tensor = false;
#endif
  // Memory type
  // We only allow weights to be CPU tensor or int8tensor for now
  CAFFE_ENFORCE(
      (BlobIsTensorType(*blob, CPU) || BlobIsInt8TensorCPUType(*blob) ||
       is_external_tensor),
      "Initialization blob ",
      name,
      " needs to be TensorCPU or Int8TensorCPU or Int8FCDNNLowPPackedWeightBlob Based class");
  desc->tag = ONNXIFI_TAG_TENSOR_DESCRIPTOR_V1;
  desc->memoryType = ONNXIFI_MEMORY_TYPE_CPU;
  desc->isOffline = false;

  if (is_int8tensor) {
    // Data type
    const auto& cpu_int8tensor = blob->template Get<int8::Int8TensorCPU>();
    const auto& cpu_tensor = cpu_int8tensor.t;
    SetInputTensorDescriptorTypeAndBuffer(cpu_int8tensor, desc);
    // Set dims
    const auto shape = cpu_tensor.sizes();
    desc->dimensions = shape.size();
    shapes->emplace_back(shape.cbegin(), shape.cend());
    desc->shape = shapes->back().data();
  } else if (is_external_tensor) {
#ifndef C10_MOBILE
    ExternalTensorDescriptor ext_desc;
    function_ptr->SetupExternalTensorDescriptor(
        blob, shapes, all_scales, all_offsets, &ext_desc);
    copyDescriptor(&ext_desc, desc);
#endif
  } else {
    // Data type
    const auto& cpu_tensor = blob->template Get<TensorCPU>();
    SetInputTensorDescriptorTypeAndBuffer(cpu_tensor, desc);
    // Set dims
    const auto shape = cpu_tensor.sizes();
    desc->dimensions = shape.size();
    shapes->emplace_back(shape.cbegin(), shape.cend());
    desc->shape = shapes->back().data();
    desc->quantizationParams = 0;
  }
}

uint64_t getOnnxifiDataType(caffe2::TensorProto::DataType t) {
#define CAFFE2_TO_ONNXIFI_TYPE(x) \
  case (caffe2::TensorProto::x):  \
    return ONNXIFI_DATATYPE_##x
  switch (t) {
    CAFFE2_TO_ONNXIFI_TYPE(INT8);
    CAFFE2_TO_ONNXIFI_TYPE(UINT8);
    CAFFE2_TO_ONNXIFI_TYPE(UINT16);
    CAFFE2_TO_ONNXIFI_TYPE(INT16);
    CAFFE2_TO_ONNXIFI_TYPE(INT32);
    CAFFE2_TO_ONNXIFI_TYPE(INT64);
    CAFFE2_TO_ONNXIFI_TYPE(FLOAT16);
    case (caffe2::TensorProto::FLOAT):
      return ONNXIFI_DATATYPE_FLOAT32;
    default:
      LOG(WARNING) << "Unsupported Caffe2 tensor type: " << t;
      return ONNXIFI_DATATYPE_UNDEFINED;
  }
#undef CAFFE2_TO_ONNXIFI_TYPE
}

} // namespace

namespace details {
TensorInfo::TensorInfo(const TensorProto& t)
    : onnxifi_type(getOnnxifiDataType(t.data_type())),
      quantized(false),
      quantizationAxis(0),
      quantizationParams(0) {
  for (const auto d : t.dims()) {
    dims.push_back(d);
  }
}

TensorInfo::TensorInfo(const QTensorProto& t)
    : onnxifi_type(getOnnxifiDataType(t.data_type())),
      quantized(true),
      quantizationAxis(t.has_axis() ? t.axis() : 0),
      quantizationParams(t.scales_size() ? t.scales_size() : 1) {
  for (const auto d : t.dims()) {
    dims.push_back(d);
  }
  if (t.scales_size()) {
    for (const auto d : t.scales()) {
      scales.push_back(static_cast<float>(d));
    }
    for (const auto d : t.biases()) {
      biases.push_back(static_cast<int32_t>(d));
    }
  } else {
    scales.push_back(static_cast<float>(t.scale()));
    biases.push_back(static_cast<int32_t>(t.bias()));
  }
}
} // namespace details

template <>
std::vector<onnxTensorDescriptorV1>
OnnxifiOp<CPUContext>::buildInitializationList(
    Workspace* ws,
    const std::vector<std::string>& initializers,
    std::vector<std::string>* weight_names,
    std::vector<std::vector<uint64_t>>* weight_shapes,
    std::vector<std::vector<float>>* all_scales,
    std::vector<std::vector<int32_t>>* all_offsets) const {
  std::unordered_set<std::string> initialization_list(
      initializers.begin(), initializers.end());
  const std::vector<string>& ws_blobs = ws->Blobs();
  // Since onnxTensorDescriptorV1.name will point into the memory in
  // weight_names, we need to prevent weight_names from reallocating by
  // reserving enough memory ahead of time
  weight_names->reserve(ws_blobs.size());
  std::vector<onnxTensorDescriptorV1> descs;
  for (const auto& s : ws_blobs) {
    auto it = initialization_list.find(s);
    if (it != initialization_list.end()) {
      weight_names->emplace_back(s);
      onnxTensorDescriptorV1 tensor_desc;
      tensor_desc.name = weight_names->back().c_str();
      BlobToTensorDescriptor(
          s, ws, &tensor_desc, weight_shapes, all_scales, all_offsets);
      descs.push_back(tensor_desc);
      initialization_list.erase(it);
    }
  }
  CAFFE_ENFORCE(initialization_list.empty(), "Unfulfilled initialization list");
  return descs;
}

template <>
details::OutputReshapeInfo OnnxifiOp<CPUContext>::initOutputReshapeInfo()
    const {
  details::OutputReshapeInfo output_reshape_info;
  output_reshape_info.begins.reserve(output_names_.size());
  output_reshape_info.ends.reserve(output_names_.size());
  output_reshape_info.fast_path.reserve(output_names_.size());
  for (int i = 0; i < output_names_.size(); ++i) {
    const auto it = output_shape_hints_.find(i);
    CAFFE_ENFORCE(
        it != output_shape_hints_.end(),
        "Cannot find output shape hints for ",
        output_names_[i]);
    int64_t num_dims = it->second.dims.size();
    // Initialize the tensors used to slice the output
    output_reshape_info.begins.emplace_back();
    ReinitializeTensor(
        &output_reshape_info.begins.back(),
        {num_dims},
        at::dtype<int32_t>().device(CPU));
    output_reshape_info.ends.emplace_back();
    ReinitializeTensor(
        &output_reshape_info.ends.back(),
        {num_dims},
        at::dtype<int32_t>().device(CPU));
  }
  return output_reshape_info;
}

template <>
int OnnxifiOp<CPUContext>::extractOutputBatchSizes() {
  if (use_onnx_ || !adjust_output_batch_) {
    return max_batch_size_;
  }

  // Get the real batch size from nominal input. If it's equal to
  // max_batch_size, mark that we don't need to adjust batch size and return.
  // Otherwise, do a pass of shape inference to get the real shapes of the
  // outputs.
  const auto& t = Input(nominal_batch_idx_);
  CAFFE_ENFORCE(
      !t.sizes().empty(), input_names_[nominal_batch_idx_], " cannot be empty");
  const auto dims = t.sizes();
  const int current_batch_size = dims[0];

  if (current_batch_size == max_batch_size_) {
    return max_batch_size_;
  }

  // We still need to adjust output size but we can skip the shape inference as
  // it was done before.
  if (output_reshape_info_.count(current_batch_size)) {
    return current_batch_size;
  }

  auto it =
      output_reshape_info_.emplace(current_batch_size, initOutputReshapeInfo());
  auto& output_reshape_info = it.first->second;
  BoundShapeSpec spec(dims[0], max_seq_size_);
  auto bound_shape_inferencer =
      BoundShapeInferencerRegistry()->Create("C10", spec);
  for (int i = 0; i < InputSize(); ++i) {
    const auto& t0 = Input(i);
    const auto dim0 = t0.sizes();
    TensorShape shape;
    for (const auto d : dim0) {
      shape.add_dims(d);
    }
    std::vector<TensorBoundShape::DimType> dim_type(
        shape.dims_size(), TensorBoundShape_DimType_CONSTANT);
    if (dim_type.size()) {
      dim_type[0] = TensorBoundShape_DimType_BATCH;
    }
    input_shape_info_[input_names_[i]] = ShapeInfo(dim_type, std::move(shape));
  }
  bound_shape_inferencer->InferBoundShapeAndType(
      netdef_, input_shape_info_, nullptr, false);
  const auto& shape_info = bound_shape_inferencer->shape_info();
  for (int i = 0; i < OutputSize(); ++i) {
    const auto it = shape_info.find(output_names_[i]);
    CAFFE_ENFORCE(it != shape_info.end());
    const auto& real_shape = it->second.shape;
    const auto& max_shape = output_shapes_[i];
    CAFFE_ENFORCE_EQ(real_shape.dims_size(), max_shape.size());
    const auto dim_size = real_shape.dims_size();
    auto& begin = output_reshape_info.begins[i];
    begin.Resize(dim_size);
    int32_t* begin_ptr = begin.template mutable_data<int32_t>();
    auto& end = output_reshape_info.ends[i];
    end.Resize(dim_size);
    int32_t* end_ptr = end.template mutable_data<int32_t>();
    int32_t mismatch = 0;
    for (int j = 0; j < dim_size; ++j) {
      CAFFE_ENFORCE_GE(
          max_shape[j],
          real_shape.dims(j),
          "It is weird that max shape of ",
          output_names_[i],
          " is smaller than real shape at dim ",
          j,
          " (",
          max_shape[j],
          " vs ",
          real_shape.dims(j),
          ")");
      begin_ptr[j] = 0;
      if (max_shape[j] > real_shape.dims(j)) {
        end_ptr[j] = real_shape.dims(j);
        mismatch += j;
      } else {
        end_ptr[j] = -1;
      }
    }
    output_reshape_info.fast_path[i] = !mismatch;
  }
  return current_batch_size;
}

template <>
void OnnxifiOp<CPUContext>::adjustOutputBatchSizes(int current_batch_size) {
  auto it = output_reshape_info_.find(current_batch_size);
  CAFFE_ENFORCE(
      it != output_reshape_info_.end(),
      "Cannot find current_batch_size ",
      current_batch_size,
      " in output_reshape_info_");
  const auto& output_reshape_info = it->second;
  CPUContext context;
  Tensor tmp(CPU);
  for (int i = 0; i < OutputSize(); ++i) {
    auto* output_tensor = Output(i);
    const auto& end = output_reshape_info.ends[i];
    if (output_reshape_info.fast_path[i]) {
      output_tensor->ShrinkTo(end.data<int32_t>()[0]);
    } else {
      // We need to use generic Slice
      SliceImpl<int32_t, CPUContext>(
          &tmp, *output_tensor, output_reshape_info.begins[i], end, &context);
      output_tensor->CopyFrom(tmp);
    }
  }
}

template <>
bool OnnxifiOp<CPUContext>::RunOnDevice() {
  CAFFE_ENFORCE_EQ(input_desc_.size(), InputSize());
  for (unsigned i = 0U; i < InputSize(); ++i) {
    const auto& input_tensor = Input(i);
    const at::IntArrayRef tensor_dims = input_tensor.sizes();
    auto& tensor_descriptor = input_desc_[i];
    tensor_descriptor.tag = ONNXIFI_TAG_TENSOR_DESCRIPTOR_V1;
    tensor_descriptor.memoryType = ONNXIFI_MEMORY_TYPE_CPU;
    tensor_descriptor.dimensions = tensor_dims.size();
    auto& input_shape = input_shapes_[i];
    input_shape.clear();
    input_shape.insert(
        input_shape.begin(), tensor_dims.cbegin(), tensor_dims.cend());
    tensor_descriptor.shape = input_shape.data();
    SetInputTensorDescriptorTypeAndBuffer(input_tensor, &tensor_descriptor);
  }

  CAFFE_ENFORCE_EQ(output_desc_.size(), OutputSize());
  for (unsigned i = 0U; i < OutputSize(); ++i) {
    tensor_dims_int64_.clear();
    std::vector<size_t> tensor_dims;
    uint64_t type = SetOutputShapeAndType(i, &tensor_dims);
    auto& tensor_descriptor = output_desc_[i];
    tensor_descriptor.tag = ONNXIFI_TAG_TENSOR_DESCRIPTOR_V1;
    tensor_descriptor.memoryType = ONNXIFI_MEMORY_TYPE_CPU;
    tensor_descriptor.dimensions = tensor_dims.size();
    CAFFE_ENFORCE(
        tensor_descriptor.dimensions != 0,
        tensor_descriptor.name,
        " has 0 dim");
    auto& output_shape = output_shapes_[i];
    output_shape.clear();
    output_shape.insert(
        output_shape.begin(), tensor_dims.cbegin(), tensor_dims.cend());
    tensor_descriptor.shape = output_shape.data();
    std::copy(
        tensor_dims.cbegin(),
        tensor_dims.cend(),
        std::back_inserter(tensor_dims_int64_));
    auto* output_tensor = Output(
        i,
        tensor_dims_int64_,
        at::dtype(OnnxifiTypeToDataType(type)).device(CPU));
    SetOutputTensorDescriptorTypeAndBuffer(
        type, output_tensor, &tensor_descriptor);
  }
  bool ext_supported = false;
  onnxMemoryFenceV1 input_fence;
  onnxMemoryFenceV1 output_fence;
  std::vector<int> output_batch_sizes;
  int current_batch_size = max_batch_size_;
#ifdef ONNXIFI_ENABLE_EXT
  /**
   * If onnxifi extension mode is enabled,
   * and onnxSetIOAndRunGraph is supported in backend,
   * then we run throw this workflow;
   * Else we fallback to non-onnxifi-extension workflow.
   **/
  if (onnxSetIOAndRunGraphPointer_ != nullptr) {
    ext_supported = true;
    output_fence.tag = ONNXIFI_TAG_MEMORY_FENCE_V1;
    output_fence.type = ONNXIFI_SYNCHRONIZATION_EVENT;
    if (enable_tracing_) {
      traces_.reset();
      traces_ = std::shared_ptr<onnxTraceEventList>(
          new onnxTraceEventList(), [this](onnxTraceEventList* p) {
            if (p && onnxReleaseTraceEventsPointer_) {
              CAFFE_ENFORCE_EQ(
                  (*onnxReleaseTraceEventsPointer_)(p), ONNXIFI_STATUS_SUCCESS);
            }
            delete p;
          });
      traces_->numEvents = 0;
    }
    CAFFE_ENFORCE_EQ(
        (*onnxSetIOAndRunGraphPointer_)(
            graph_,
            input_desc_.size(),
            input_desc_.data(),
            output_desc_.size(),
            output_desc_.data(),
            &output_fence,
            traces_.get()),
        ONNXIFI_STATUS_SUCCESS);
    current_batch_size = extractOutputBatchSizes();
    CAFFE_ENFORCE_EQ(
        lib_->onnxWaitEvent(output_fence.event), ONNXIFI_STATUS_SUCCESS);
    CAFFE_ENFORCE_EQ(
        lib_->onnxReleaseEvent(output_fence.event), ONNXIFI_STATUS_SUCCESS);
  }
#endif
  if (!ext_supported) {
    CAFFE_ENFORCE_EQ(
        lib_->onnxSetGraphIO(
            graph_,
            input_desc_.size(),
            input_desc_.data(),
            output_desc_.size(),
            output_desc_.data()),
        ONNXIFI_STATUS_SUCCESS);

    input_fence.tag = ONNXIFI_TAG_MEMORY_FENCE_V1;
    input_fence.type = ONNXIFI_SYNCHRONIZATION_EVENT;
    CAFFE_ENFORCE_EQ(
        lib_->onnxInitEvent(backend_, &input_fence.event),
        ONNXIFI_STATUS_SUCCESS);
    output_fence.tag = ONNXIFI_TAG_MEMORY_FENCE_V1;
    output_fence.type = ONNXIFI_SYNCHRONIZATION_EVENT;

    // Call the async run on backend, signal event on input fence and wait for
    // the event on output fence
    CAFFE_ENFORCE_EQ(
        lib_->onnxRunGraph(graph_, &input_fence, &output_fence),
        ONNXIFI_STATUS_SUCCESS);
    CAFFE_ENFORCE_EQ(
        lib_->onnxSignalEvent(input_fence.event), ONNXIFI_STATUS_SUCCESS);
    current_batch_size = extractOutputBatchSizes();
    CAFFE_ENFORCE_EQ(
        lib_->onnxWaitEvent(output_fence.event), ONNXIFI_STATUS_SUCCESS);

    // Destroy the event objects
    CAFFE_ENFORCE_EQ(
        lib_->onnxReleaseEvent(input_fence.event), ONNXIFI_STATUS_SUCCESS);
    CAFFE_ENFORCE_EQ(
        lib_->onnxReleaseEvent(output_fence.event), ONNXIFI_STATUS_SUCCESS);
  }

  if (adjust_output_batch_ && current_batch_size != max_batch_size_) {
    adjustOutputBatchSizes(current_batch_size);
  }
  enable_tracing_ = false;
  return true;
}

REGISTER_CPU_OPERATOR(Onnxifi, OnnxifiOp<CPUContext>);
OPERATOR_SCHEMA(Onnxifi)
    .NumInputs(0, INT_MAX)
    .NumOutputs(0, INT_MAX)
    .SetDoc(R"DOC(
    The Onnxifi operator is a black-box operator to lower the computation to Onnxifi backend
    )DOC")
    .Arg(
        "onnx_model",
        "(string default=\"\") Serialized ONNX model to be converted to backend representation")
    .Arg(
        "initializers",
        "Initialization pair indicating the mapping of the name between NetDef and ONNX model")
    .Arg(
        "output_resize_hints",
        "A list of key/value pairs indicating which input index to look up for real batch size for the given max output batch size");
} // namespace caffe2
