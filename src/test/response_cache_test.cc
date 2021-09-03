// Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "gtest/gtest.h"

#include "src/core/memory.h"
#include "src/core/logging.h"
#include "src/core/response_cache.h"

// TODO: Not sure how to incorporate this
// LOG_SET_VERBOSE(TRITONSERVER_LOG_VERBOSE);

namespace ni = nvidia::inferenceserver;

/* Mock classes for Unit Testing */
namespace nvidia { namespace inferenceserver {

//
// InferenceResponseFactory
//
Status
InferenceResponseFactory::CreateResponse(
    std::unique_ptr<InferenceResponse>* response) const
{
  response->reset(new InferenceResponse(
      backend_, id_, allocator_, alloc_userp_, response_fn_, response_userp_,
      response_delegator_));

  return Status::Success;
}

//
// InferenceRequest
//
InferenceRequest::Input::Input(
    const std::string& name, const inference::DataType datatype,
    const int64_t* shape, const uint64_t dim_count)
    : name_(name), datatype_(datatype),
      original_shape_(shape, shape + dim_count), is_shape_tensor_(false),
      data_(new MemoryReference), has_host_policy_specific_data_(false)
{
}

// Use const global var as locals can't be returned in ModelName(),
// and we don't care about the backend for the unit test
const std::string BACKEND = "backend";

const std::string&
InferenceRequest::ModelName() const
{
  return BACKEND;
}

int64_t
InferenceRequest::ActualModelVersion() const
{
  // Not using backend in unit test mock
  return requested_model_version_;
}

Status
InferenceRequest::PrepareForInference()
{
  // Remove override inputs as those are added during any previous
  // inference execution.
  inputs_.clear();
  override_inputs_.clear();

  // Initially show the actual inputs to be only the original
  // inputs. If overrides are added later they will be added to
  // 'inputs_'.
  for (auto& pr : original_inputs_) {
    inputs_.emplace(std::make_pair(pr.first, std::addressof(pr.second)));
  }

  // Clear the timestamps
  queue_start_ns_ = 0;
#ifdef TRITON_ENABLE_STATS
  request_start_ns_ = 0;
#endif  // TRITON_ENABLE_STATS

  LOG_VERBOSE(1) << "prepared: " << *this;

  return Status::Success;
}

Status
InferenceRequest::Input::DataBuffer(
    const size_t idx, const void** base, size_t* byte_size,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id) const
{
  *base = data_->BufferAt(idx, byte_size, memory_type, memory_type_id);

  return Status::Success;
}

void
InferenceRequest::SetPriority(unsigned int)
{
}

Status
InferenceRequest::AddOriginalInput(
    const std::string& name, const inference::DataType datatype,
    const int64_t* shape, const uint64_t dim_count,
    InferenceRequest::Input** input)
{
  const auto& pr = original_inputs_.emplace(
      std::piecewise_construct, std::forward_as_tuple(name),
      std::forward_as_tuple(name, datatype, shape, dim_count));
  if (!pr.second) {
    return Status(
        Status::Code::INVALID_ARG,
        "input '" + name + "' already exists in request");
  }

  if (input != nullptr) {
    *input = std::addressof(pr.first->second);
  }

  needs_normalization_ = true;
  return Status::Success;
}

Status
InferenceRequest::AddOriginalInput(
    const std::string& name, const inference::DataType datatype,
    const std::vector<int64_t>& shape, InferenceRequest::Input** input)
{
  return AddOriginalInput(name, datatype, &shape[0], shape.size(), input);
}

Status
InferenceRequest::Input::AppendData(
    const void* base, size_t byte_size, TRITONSERVER_MemoryType memory_type,
    int64_t memory_type_id)
{
  if (byte_size > 0) {
    std::static_pointer_cast<MemoryReference>(data_)->AddBuffer(
        static_cast<const char*>(base), byte_size, memory_type, memory_type_id);
  }

  return Status::Success;
}

//
// InferenceResponse
//

InferenceResponse::InferenceResponse(
    const std::shared_ptr<InferenceBackend>& backend, const std::string& id,
    const ResponseAllocator* allocator, void* alloc_userp,
    TRITONSERVER_InferenceResponseCompleteFn_t response_fn,
    void* response_userp,
    const std::function<
        void(std::unique_ptr<InferenceResponse>&&, const uint32_t)>& delegator)
    : backend_(backend), id_(id), allocator_(allocator),
      alloc_userp_(alloc_userp), response_fn_(response_fn),
      response_userp_(response_userp), response_delegator_(delegator),
      null_response_(false)
{
  // Skip allocator logic / references in unit test
}

InferenceResponse::Output::~Output()
{
  Status status = ReleaseDataBuffer();
  /*if (!status.IsOk()) {
    LOG_ERROR << "failed to release buffer for output '" << name_
              << "': " << status.AsString();
  }*/
}

Status
InferenceResponse::Output::ReleaseDataBuffer()
{
  // TRITONSERVER_Error* err = nullptr;

  if (allocated_buffer_ != nullptr) {
    free(allocated_buffer_);

    /*err = allocator_->ReleaseFn()(
        reinterpret_cast<TRITONSERVER_ResponseAllocator*>(
            const_cast<ResponseAllocator*>(allocator_)),
        allocated_buffer_, allocated_userp_, allocated_buffer_byte_size_,
        allocated_memory_type_, allocated_memory_type_id_);*/
  }

  allocated_buffer_ = nullptr;
  allocated_buffer_byte_size_ = 0;
  allocated_memory_type_ = TRITONSERVER_MEMORY_CPU;
  allocated_memory_type_id_ = 0;
  allocated_userp_ = nullptr;

  // RETURN_IF_TRITONSERVER_ERROR(err);

  return Status::Success;
}

// Same as defined in infer_response.cc
Status
InferenceResponse::Output::DataBuffer(
    const void** buffer, size_t* buffer_byte_size,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id,
    void** userp) const
{
  *buffer = allocated_buffer_;
  *buffer_byte_size = allocated_buffer_byte_size_;
  *memory_type = allocated_memory_type_;
  *memory_type_id = allocated_memory_type_id_;
  *userp = allocated_userp_;
  return Status::Success;
}

// Simplified version of AllocateDataBuffer for CPU memory only
Status
InferenceResponse::Output::AllocateDataBuffer(
    void** buffer, size_t buffer_byte_size,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id)
{
  if (allocated_buffer_ != nullptr) {
    return Status(
        Status::Code::ALREADY_EXISTS,
        "allocated buffer for output '" + name_ + "' already exists");
  }

  // Simplifications - CPU memory only for now
  if (*memory_type != TRITONSERVER_MEMORY_CPU || *memory_type_id != 0) {
    return Status(
        Status::Code::INTERNAL, "Only standard CPU memory supported for now");
  }

  // Allocate buffer to copy to
  *buffer = malloc(buffer_byte_size);
  if (buffer == nullptr || *buffer == nullptr) {
    return Status(
        Status::Code::INTERNAL, "buffer was nullptr in AllocateDataBuffer");
  }

  // Set relevant member variables for DataBuffer() to return
  allocated_buffer_ = *buffer;
  // std::memcpy(allocated_buffer_, *buffer, buffer_byte_size);
  allocated_buffer_byte_size_ = buffer_byte_size;
  allocated_memory_type_ = *memory_type;
  allocated_memory_type_id_ = *memory_type_id;
  allocated_userp_ = nullptr;
  std::cout << "Done in AllocateDataBuffer" << std::endl;
  return Status::Success;
}

Status
InferenceResponse::AddOutput(
    const std::string& name, const inference::DataType datatype,
    const std::vector<int64_t>& shape, InferenceResponse::Output** output)
{
  outputs_.emplace_back(name, datatype, shape, allocator_, alloc_userp_);

  // LOG_VERBOSE(1) << "add response output: " << outputs_.back();

  /*if (backend_ != nullptr) {
    const inference::ModelOutput* output_config;
    RETURN_IF_ERROR(backend_->GetOutput(name, &output_config));
    if (output_config->has_reshape()) {
      const bool has_batch_dim = (backend_->Config().max_batch_size() > 0);
      outputs_.back().Reshape(has_batch_dim, output_config);
    }
  }*/

  if (output != nullptr) {
    *output = std::addressof(outputs_.back());
  }

  return Status::Success;
}

}}  // namespace nvidia::inferenceserver


namespace {

// Test Fixture
class RequestResponseCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}

 public:
  ni::InferenceBackend* backend = nullptr;
  uint64_t model_version = 1;
};

// Helpers
void
check_status(ni::Status status)
{
  ASSERT_TRUE(status.IsOk()) << "ERROR: " << status.Message();
}

void
cache_stats(ni::RequestResponseCache& cache)
{
  std::cout << "Cache entries: " << cache.NumEntries() << std::endl;
  std::cout << "Cache free bytes: " << cache.FreeBytes() << std::endl;
  std::cout << "Cache alloc'd bytes: " << cache.AllocatedBytes() << std::endl;
  std::cout << "Cache total bytes: " << cache.TotalBytes() << std::endl;
}

// Test hashing for consistency on same request
TEST_F(RequestResponseCacheTest, TestHashing)
{
  // Create cache
  std::cout << "Create cache" << std::endl;
  uint64_t cache_size = 4 * 1024 * 1024;
  ni::RequestResponseCache cache(cache_size);

  // Create request
  std::cout << "Create request" << std::endl;
  ni::InferenceRequest request0(backend, model_version);
  ni::InferenceRequest request1(backend, model_version);
  ni::InferenceRequest request2(backend, model_version);

  // Create inputs
  std::cout << "Create inputs" << std::endl;
  inference::DataType dtype = inference::DataType::TYPE_INT32;
  std::vector<int64_t> shape{1, 4};
  ni::InferenceRequest::Input* input0 = nullptr;
  ni::InferenceRequest::Input* input1 = nullptr;
  ni::InferenceRequest::Input* input2 = nullptr;
  // Add input to requests
  std::cout << "Add input to request" << std::endl;
  request0.AddOriginalInput("input", dtype, shape, &input0);
  request1.AddOriginalInput("input", dtype, shape, &input1);
  request2.AddOriginalInput("input", dtype, shape, &input2);
  ASSERT_NE(input0, nullptr);
  ASSERT_NE(input1, nullptr);
  ASSERT_NE(input2, nullptr);
  // Add data to input
  int data0[4] = {1, 2, 3, 4};
  int data1[4] = {5, 6, 7, 8};
  int data2[4] = {5, 6, 7, 8};
  TRITONSERVER_MemoryType memory_type = TRITONSERVER_MEMORY_CPU;
  int64_t memory_type_id = 0;
  uint64_t input_size = sizeof(int) * 4;
  input0->AppendData(data0, input_size, memory_type, memory_type_id);
  input1->AppendData(data1, input_size, memory_type, memory_type_id);
  input2->AppendData(data2, input_size, memory_type, memory_type_id);

  // PrepareForInference for use of ImmutableInputs()
  check_status(request0.PrepareForInference());
  check_status(request1.PrepareForInference());
  check_status(request2.PrepareForInference());

  // Compare hashes
  std::cout << "Compare hashes" << std::endl;
  uint64_t hash0, hash1, hash2;
  check_status(cache.Hash(request0, &hash0));
  check_status(cache.Hash(request1, &hash1));
  check_status(cache.Hash(request2, &hash2));

  std::cout << "hash0: " << hash0 << std::endl;
  std::cout << "hash1: " << hash1 << std::endl;
  std::cout << "hash2: " << hash2 << std::endl;
  // Different input data should have different hashes
  ASSERT_NE(hash0, hash1);
  // Same input data should have same hashes
  ASSERT_EQ(hash1, hash2);
}

// Test cache too small for entry
TEST_F(RequestResponseCacheTest, TestCacheTooSmall)
{
  // Create cache
  std::cout << "Create cache" << std::endl;
  uint64_t cache_size = 1024;
  ni::RequestResponseCache cache(cache_size);

  // Create request
  std::cout << "Create request" << std::endl;
  ni::InferenceRequest request0(backend, model_version);

  inference::DataType dtype = inference::DataType::TYPE_INT32;
  std::vector<int64_t> shape{1, 4};
  TRITONSERVER_MemoryType memory_type = TRITONSERVER_MEMORY_CPU;
  int64_t memory_type_id = 0;

  // Fake hashes to input same response to cache repeatedly
  uint64_t hash0 = 0;

  std::cout << "Create response object" << std::endl;
  std::unique_ptr<ni::InferenceResponse> response0;
  check_status(request0.ResponseFactory().CreateResponse(&response0));

  std::cout << "Add output metadata to response object" << std::endl;
  ni::InferenceResponse::Output* response_output = nullptr;
  // Explicitly create output buffer larger than entire cache
  std::vector<int> output0(1025, 0);
  uint64_t output_size = sizeof(int) * output0.size();
  std::cout << "Output size bytes: " << output_size << std::endl;
  check_status(response0->AddOutput("output", dtype, shape, &response_output));

  std::cout << "Allocate output data buffer for response object" << std::endl;
  void* buffer;
  check_status(response_output->AllocateDataBuffer(
      &buffer, output_size, &memory_type, &memory_type_id));
  ASSERT_NE(buffer, nullptr);
  // Copy data from output to response buffer
  std::memcpy(buffer, output0.data(), output_size);

  std::cout << "Insert response into cache with hash0" << std::endl;
  auto status = cache.Insert(hash0, *response0);
  // We expect insertion to fail here since cache is too small
  std::cout << status.Message() << std::endl;
  ASSERT_FALSE(status.IsOk())
      << "Inserting item larger than cache succeeded when it should fail";
}

// Test hashing for consistency on same request
TEST_F(RequestResponseCacheTest, TestEviction)
{
  // Create cache
  std::cout << "Create cache" << std::endl;
  uint64_t cache_size = 1024;
  ni::RequestResponseCache cache(cache_size);
  cache_stats(cache);

  // Create request
  std::cout << "Create request" << std::endl;
  ni::InferenceRequest request0(backend, model_version);

  inference::DataType dtype = inference::DataType::TYPE_INT32;
  std::vector<int64_t> shape{1, 4};
  TRITONSERVER_MemoryType memory_type = TRITONSERVER_MEMORY_CPU;
  int64_t memory_type_id = 0;

  // Fake hashes to input same response to cache repeatedly
  uint64_t hash0 = 0;
  uint64_t hash1 = 1;
  uint64_t hash2 = 2;
  uint64_t hash3 = 3;

  std::cout << "Create response object" << std::endl;
  std::unique_ptr<ni::InferenceResponse> response0;
  check_status(request0.ResponseFactory().CreateResponse(&response0));

  std::cout << "Add output metadata to response object" << std::endl;
  ni::InferenceResponse::Output* response_output = nullptr;
  std::vector<int> output0(100, 0);
  uint64_t output_size = sizeof(int) * output0.size();
  std::cout << "Output size: " << output_size << std::endl;
  check_status(response0->AddOutput("output", dtype, shape, &response_output));

  std::cout << "Allocate output data buffer for response object" << std::endl;
  void* buffer;
  check_status(response_output->AllocateDataBuffer(
      &buffer, output_size, &memory_type, &memory_type_id));
  ASSERT_NE(buffer, nullptr);
  // Copy data from output to response buffer
  std::memcpy(buffer, output0.data(), output_size);

  std::cout << "Lookup hash0 in empty cache" << std::endl;
  auto status = cache.Lookup(hash0, nullptr);
  // This hash not in cache yet
  ASSERT_FALSE(status.IsOk())
      << "hash [" + std::to_string(hash0) + "] should not be in cache";
  std::cout << "Insert response into cache with hash0" << std::endl;
  check_status(cache.Insert(hash0, *response0));
  cache_stats(cache);
  ASSERT_EQ(cache.NumEntries(), 1u);
  ASSERT_EQ(cache.NumEvictions(), 0u);

  check_status(cache.Insert(hash1, *response0));
  cache_stats(cache);
  ASSERT_EQ(cache.NumEntries(), 2u);
  ASSERT_EQ(cache.NumEvictions(), 0u);

  check_status(cache.Insert(hash2, *response0));
  cache_stats(cache);
  ASSERT_EQ(cache.NumEntries(), 2u);
  ASSERT_EQ(cache.NumEvictions(), 1u);

  check_status(cache.Insert(hash3, *response0));
  cache_stats(cache);
  ASSERT_EQ(cache.NumEntries(), 2u);
  ASSERT_EQ(cache.NumEvictions(), 2u);
}


// Test hashing for consistency on same request
TEST_F(RequestResponseCacheTest, TestEndToEnd)
{
  // Create cache
  std::cout << "Create cache" << std::endl;
  uint64_t cache_size = 4 * 1024 * 1024;
  ni::RequestResponseCache cache(cache_size);
  cache_stats(cache);

  // Create request
  std::cout << "Create request" << std::endl;
  ni::InferenceRequest request0(backend, model_version);

  // Create input
  std::cout << "Create inputs" << std::endl;
  inference::DataType dtype = inference::DataType::TYPE_INT32;
  std::vector<int64_t> shape{1, 4};
  ni::InferenceRequest::Input* input0 = nullptr;
  // Add input to request
  std::cout << "Add input to request" << std::endl;
  request0.AddOriginalInput("input", dtype, shape, &input0);
  ASSERT_NE(input0, nullptr);
  // PrepareForInference for use of ImmutableInputs()
  check_status(request0.PrepareForInference());

  // Add data to input
  std::vector<int> data0 = {1, 2, 3, 4};
  TRITONSERVER_MemoryType memory_type = TRITONSERVER_MEMORY_CPU;
  int64_t memory_type_id = 0;
  uint64_t input_size = sizeof(int) * data0.size();
  input0->AppendData(data0.data(), input_size, memory_type, memory_type_id);

  // Hash input request
  uint64_t hash0;
  check_status(cache.Hash(request0, &hash0));

  std::cout << "Create response object" << std::endl;
  std::unique_ptr<ni::InferenceResponse> response0;
  check_status(request0.ResponseFactory().CreateResponse(&response0));

  std::cout << "Add output metadata to response object" << std::endl;
  ni::InferenceResponse::Output* response_output = nullptr;
  std::vector<int> output0 = {2, 4, 6, 8};
  uint64_t output_size = sizeof(int) * output0.size();
  std::cout << "Example InferenceResponse outputs:" << std::endl;
  for (const auto& output : output0) {
    std::cout << output << std::endl;
  }
  std::cout << "Output size bytes: " << output_size << std::endl;
  check_status(response0->AddOutput("output", dtype, shape, &response_output));

  std::cout << "Allocate output data buffer for response object" << std::endl;
  void* buffer;
  check_status(response_output->AllocateDataBuffer(
      &buffer, output_size, &memory_type, &memory_type_id));
  ASSERT_NE(buffer, nullptr);
  // Copy data from output to response buffer
  std::memcpy(buffer, output0.data(), output_size);

  std::cout << "Lookup hash0 in empty cache" << std::endl;
  auto status = cache.Lookup(hash0, nullptr);
  // This hash not in cache yet
  ASSERT_FALSE(status.IsOk())
      << "hash [" + std::to_string(hash0) + "] should not be in cache";
  std::cout << "Insert response into cache with hash0" << std::endl;
  // Insertion should succeed
  check_status(cache.Insert(hash0, *response0));
  cache_stats(cache);

  // Duplicate insertion should fail since key already exists
  status = cache.Insert(hash0, *response0);
  ASSERT_FALSE(status.IsOk())
      << "Inserting duplicate item in cache should fail";

  // Create response to test cache lookup
  std::cout << "Create response object into fill from cache" << std::endl;
  std::unique_ptr<ni::InferenceResponse> response_test;
  check_status(request0.ResponseFactory().CreateResponse(&response_test));

  // Lookup should now succeed
  std::cout << "Lookup hash0 in cache after insertion" << std::endl;
  check_status(cache.Lookup(hash0, response_test.get()));

  // Fetch output buffer details
  const void* response_buffer = nullptr;
  size_t response_byte_size = 0;
  TRITONSERVER_MemoryType response_memory_type;
  int64_t response_memory_type_id;
  void* userp;
  // TODO: How to handle different memory types? GPU vs CPU vs Pinned, etc.
  // const auto outputs = response_test->Outputs();
  // Build cache entry data from response outputs
  // TODO: Handle multiple outputs more generically
  for (const auto& response_output : response_test->Outputs()) {
    check_status(response_output.DataBuffer(
        &response_buffer, &response_byte_size, &response_memory_type,
        &response_memory_type_id, &userp));
  }

  int* output_test = (int*)response_buffer;
  std::cout << "Check output buffer data from cache entry:" << std::endl;
  for (size_t i = 0; i < response_byte_size / sizeof(int); i++) {
    std::cout << output_test[i] << " == " << output0[i] << std::endl;
    ASSERT_EQ(output_test[i], output0[i]);
  }

  // Simple Evict() test
  ASSERT_EQ(cache.NumEntries(), 1u);
  ASSERT_EQ(cache.NumEvictions(), 0u);
  cache.Evict();
  ASSERT_EQ(cache.NumEntries(), 0u);
  ASSERT_EQ(cache.NumEvictions(), 1u);
  std::cout << "Done!" << std::endl;
}

}  // namespace

int
main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
