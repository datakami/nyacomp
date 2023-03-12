#include <pybind11/pybind11.h>
#include "nvcomp/lz4.hpp"
#include "nvcomp.hpp" 
#include "nvcomp/nvcompManagerFactory.hpp"

#include <chrono>
#include <random>
#include <assert.h>
#include <iostream>
#include <fstream>
#include <vector>

#include <torch/extension.h>
#define STRINGIFY(x) #x
#define MACRO_STRINGIFY(x) STRINGIFY(x)

using namespace nvcomp;
namespace py = pybind11;


#define CUDA_CHECK(cond)                                                       \
  do {                                                                         \
    cudaError_t err = cond;                                                    \
    if (err != cudaSuccess) {                                               \
      std::cerr << "Failure" << std::endl;                                \
      exit(1);                                                              \
    }                                                                         \
  } while (false)


std::pair<std::vector<uint8_t>, size_t> load_file(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + filename);
  }

  size_t file_size = static_cast<size_t>(file.tellg());
  std::vector<uint8_t> buffer(file_size);

  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
    throw std::runtime_error("Failed to read file: " + filename);
  }

  return std::make_pair(buffer, file_size);
}


//float compress(void* input_ptrs, size_t input_buffer_len) {
float compress(const std::string filename) {
  std::vector<uint8_t> uncompressed_data;
  size_t input_buffer_len;
  std::tie(uncompressed_data, input_buffer_len) = load_file(filename);
  std::cout << "read " << input_buffer_len << " bytes from " << filename << std::endl;

  // Initialize a random array of chars
  // const size_t input_buffer_len = 1000000;
  // std::vector<uint8_t> uncompressed_data(input_buffer_len);
  
  // std::mt19937 random_gen(42);

  // // char specialization of std::uniform_int_distribution is
  // // non-standard, and isn't available on MSVC, so use short instead,
  // // but with the range limited, and then cast below.
  // std::uniform_int_distribution<short> uniform_dist(0, 255);
  // for (size_t ix = 0; ix < input_buffer_len; ++ix) {
  //   uncompressed_data[ix] = static_cast<uint8_t>(uniform_dist(random_gen));
  // }

  uint8_t* device_input_ptrs;
  CUDA_CHECK(cudaMalloc(&device_input_ptrs, input_buffer_len));
  CUDA_CHECK(cudaMemcpy(device_input_ptrs, uncompressed_data.data(), input_buffer_len, cudaMemcpyDefault));

  // start compressing

  cudaStream_t stream;
  CUDA_CHECK(cudaStreamCreate(&stream));

  const int chunk_size = 1 << 16;
  nvcompType_t data_type = NVCOMP_TYPE_CHAR;

  LZ4Manager nvcomp_manager{chunk_size, data_type, stream};
  CompressionConfig comp_config = nvcomp_manager.configure_compression(input_buffer_len);
  uint8_t* comp_buffer;
  CUDA_CHECK(cudaMalloc(&comp_buffer, comp_config.max_compressed_buffer_size));

  // check how long compression takes

  std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
 
  nvcomp_manager.compress(device_input_ptrs, comp_buffer, comp_config);
  CUDA_CHECK(cudaStreamSynchronize(stream));

  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  std::cout << "compression time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;

  // size of compressed buffer
  size_t comp_size = nvcomp_manager.get_compressed_output_size(comp_buffer);
  // compression ratio
  float comp_ratio = (float)comp_size / (float)input_buffer_len;

  // copy compressed buffer to host memory and then write it to a file
  
  std::ofstream comp_file("compressed.bin", std::ios::binary);
  if (!comp_file.is_open()) {
    throw std::runtime_error("Failed to open file: compressed.bin");
  }

  std::vector<uint8_t> comp_buffer_host(comp_size);
  std::cout <<"doing cuda memcpy" << std::endl;
  // copy compressed buffer to host memory using stream and syncronize to block for the copy to finish
  CUDA_CHECK(cudaMemcpy(comp_buffer_host.data(), comp_buffer, comp_size, cudaMemcpyDefault));
  std::cout <<"writing compressed buffer to file: " << "compressed.bin" << std::endl;
  comp_file.write(reinterpret_cast<const char*>(comp_buffer_host.data()), comp_size);
  comp_file.close();
  

  CUDA_CHECK(cudaFree(comp_buffer));
  CUDA_CHECK(cudaFree(device_input_ptrs));
  CUDA_CHECK(cudaStreamDestroy(stream));

  std::cout << "compressed size: " << comp_size << std::endl;
  std::cout << "compression ratio: " << comp_ratio << std::endl;

  return comp_ratio;
}

torch::Tensor decompress(const std::string filename) {
  std::vector<uint8_t> compressed_data;
  size_t input_buffer_len;
  std::tie(compressed_data, input_buffer_len) = load_file(filename);
  std::cout << "read " << input_buffer_len << " bytes from " << filename << std::endl;


  uint8_t* comp_buffer;
  CUDA_CHECK(cudaMalloc(&comp_buffer, input_buffer_len));
  CUDA_CHECK(cudaMemcpy(comp_buffer, compressed_data.data(), input_buffer_len, cudaMemcpyDefault));

  cudaStream_t stream;
  CUDA_CHECK(cudaStreamCreate(&stream));

  auto decomp_nvcomp_manager = create_manager(comp_buffer, stream);

  DecompressionConfig decomp_config = decomp_nvcomp_manager->configure_decompression(comp_buffer);
  uint8_t* res_decomp_buffer;
  CUDA_CHECK(cudaMalloc(&res_decomp_buffer, decomp_config.decomp_data_size));

  decomp_nvcomp_manager->decompress(res_decomp_buffer, comp_buffer, decomp_config);

  CUDA_CHECK(cudaFree(comp_buffer));


  {
    auto options = torch::TensorOptions().dtype(torch::kUInt8).device(torch::kCUDA, 0);
    // create and return torch tensor from the decompressed buffer
    torch::Tensor tensor = torch::from_blob(res_decomp_buffer, {decomp_config.decomp_data_size}, options).clone();
    CUDA_CHECK(cudaStreamSynchronize(stream));
    CUDA_CHECK(cudaFree(res_decomp_buffer));
    CUDA_CHECK(cudaStreamDestroy(stream));
    return tensor;
  }
  // std::cout << "decompressed size: " << decomp_config.decomp_data_size << std::endl;
  // return 1.0;
}

// torch::Tensor& make_tensor() {
//     // auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
//     // auto tensor = torch::empty({1024 / sizeof(float)}, options);
//     torch::Tensor tensor = torch::ones(5);
//     // std::cout << "made tensor" << std::endl;
//     return torch::ones(5);;
// }
  // std::shared_ptr<torch::Tensor> make_tensor() {
  //     auto tensor = std::make_shared<torch::Tensor>(torch::ones(5));
  //     std::cout << "made tensor" << std::endl;
  //     return tensor;
  // }

// torch::Tensor make_tensor() {
//     auto tensor = torch::ones(5);
//     return tensor;
// }

torch::Tensor d_sigmoid(torch::Tensor z) {
  auto s = torch::sigmoid(z);
  return (1 - s) * s;
}


PYBIND11_MODULE(python_example, m) {
    m.doc() = R"pbdoc(
        Pybind11 example plugin
        -----------------------

        .. currentmodule:: python_example

        .. autosummary::
           :toctree: _generate

           add
           subtract
    )pbdoc";

    m.def("compress", &compress, R"pbdoc(
        compress
    )pbdoc",  py::arg("filename"));

    m.def("decompress", &decompress, R"pbdoc(
        decompress
    )pbdoc",  py::arg("filename"));
    m.def("sigmoid", &d_sigmoid, "sigmod fn");


    // m.def("make_tensor", &make_tensor, py::return_value_policy::automatic);

    m.def("make_tensor", &make_tensor, "make_tensor");

    m.def("subtract", [](int i, int j) { return i - j; }, R"pbdoc(
        Subtract two numbers

        Some other explanation about the subtract function.
    )pbdoc");

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}
