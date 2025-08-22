add_llvm_component_library(LLVMInstrumentation
  AddressSanitizer.cpp
  BoundsChecking.cpp
  CGProfile.cpp
  ControlHeightReduction.cpp
  DataFlowSanitizer.cpp
  GCOVProfiling.cpp
  BlockCoverageInference.cpp
  MemProfiler.cpp
  MemorySanitizer.cpp
  NumericalStabilitySanitizer.cpp
  IndirectCallPromotion.cpp
  InstrProfiling.cpp
  KCFI.cpp
  LowerAllowCheckPass.cpp
  PGOCtxProfFlattening.cpp
  PGOCtxProfLowering.cpp
  PGOForceFunctionAttrs.cpp
  PGOInstrumentation.cpp
  PGOMemOPSizeOpt.cpp
  SanitizerCoverage.cpp
  SanitizerBinaryMetadata.cpp
  ValueProfileCollector.cpp
  ThreadSanitizer.cpp
  TypeSanitizer.cpp
  HWAddressSanitizer.cpp
  RealtimeSanitizer.cpp
  CFGInfoPass.cpp
  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  Demangle
  MC
  Support
  TargetParser
  TransformUtils
  ProfileData
  )
  # 查找和链接外部依赖
# 设置C++标准（ONNX Runtime需要C++17）
target_compile_features(LLVMInstrumentation PRIVATE cxx_std_17)

# 查找ONNX Runtime
message(STATUS "Looking for ONNX Runtime...")

# 允许用户指定ONNX Runtime的路径
if(NOT ONNXRUNTIME_ROOT)
  if(WIN32)
    set(ONNXRUNTIME_ROOT "D:/env/onnxruntime-win-x64-1.22.1" CACHE PATH "Path to ONNX Runtime root directory")
  else()
    set(ONNXRUNTIME_ROOT "/usr/local/onnxruntime" CACHE PATH "Path to ONNX Runtime root directory")
  endif()
endif()

message(STATUS "ONNXRUNTIME_ROOT: ${ONNXRUNTIME_ROOT}")

# 查找头文件
find_path(ONNXRUNTIME_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  PATHS
    ${ONNXRUNTIME_ROOT}/include
    /usr/local/include/onnxruntime
    /usr/include/onnxruntime
  PATH_SUFFIXES onnxruntime
  NO_DEFAULT_PATH
)

# 查找库文件（支持CPU和GPU版本）
set(ONNXRUNTIME_USE_GPU OFF CACHE BOOL "Use ONNX Runtime GPU version")

# 查找主库文件
find_library(ONNXRUNTIME_LIB
  NAMES onnxruntime
  PATHS
    ${ONNXRUNTIME_ROOT}/lib
    /usr/local/lib
    /usr/lib
  NO_DEFAULT_PATH
)

# 如果启用GPU，查找GPU相关的库
if(ONNXRUNTIME_USE_GPU)
  # 查找CUDA provider库
  find_library(ONNXRUNTIME_PROVIDERS_CUDA_LIB
    NAMES onnxruntime_providers_cuda
    PATHS
      ${ONNXRUNTIME_ROOT}/lib
      /usr/local/lib
      /usr/lib
    NO_DEFAULT_PATH
  )

  # 查找shared providers库
  find_library(ONNXRUNTIME_PROVIDERS_SHARED_LIB
    NAMES onnxruntime_providers_shared
    PATHS
      ${ONNXRUNTIME_ROOT}/lib
      /usr/local/lib
      /usr/lib
    NO_DEFAULT_PATH
  )

  # 查找TensorRT provider库（可选）
  find_library(ONNXRUNTIME_PROVIDERS_TENSORRT_LIB
    NAMES onnxruntime_providers_tensorrt
    PATHS
      ${ONNXRUNTIME_ROOT}/lib
      /usr/local/lib
      /usr/lib
    NO_DEFAULT_PATH
  )

  # 打印找到的GPU库
  message(STATUS "GPU libraries search results:")
  message(STATUS "  ONNXRUNTIME_LIB: ${ONNXRUNTIME_LIB}")
  message(STATUS "  ONNXRUNTIME_PROVIDERS_CUDA_LIB: ${ONNXRUNTIME_PROVIDERS_CUDA_LIB}")
  message(STATUS "  ONNXRUNTIME_PROVIDERS_SHARED_LIB: ${ONNXRUNTIME_PROVIDERS_SHARED_LIB}")
  message(STATUS "  ONNXRUNTIME_PROVIDERS_TENSORRT_LIB: ${ONNXRUNTIME_PROVIDERS_TENSORRT_LIB}")
endif()

# 检查是否找到
message(STATUS "ONNXRUNTIME_INCLUDE_DIR: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNXRUNTIME_LIB: ${ONNXRUNTIME_LIB}")

if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
  # 验证头文件是否存在
  if(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    target_include_directories(LLVMInstrumentation PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_compile_definitions(LLVMInstrumentation PRIVATE LLVM_HAS_ONNXRUNTIME=1)

    # 链接主库
    target_link_libraries(LLVMInstrumentation PRIVATE ${ONNXRUNTIME_LIB})

    if(ONNXRUNTIME_USE_GPU)
      # GPU版本：链接所有必要的GPU库
      set(GPU_LIBS_FOUND TRUE)

      # 必需的GPU库
      if(ONNXRUNTIME_PROVIDERS_CUDA_LIB)
        target_link_libraries(LLVMInstrumentation PRIVATE ${ONNXRUNTIME_PROVIDERS_CUDA_LIB})
        message(STATUS "  ✓ Linked CUDA provider: ${ONNXRUNTIME_PROVIDERS_CUDA_LIB}")
      else()
        message(WARNING "  ✗ CUDA provider library not found")
        set(GPU_LIBS_FOUND FALSE)
      endif()

      if(ONNXRUNTIME_PROVIDERS_SHARED_LIB)
        target_link_libraries(LLVMInstrumentation PRIVATE ${ONNXRUNTIME_PROVIDERS_SHARED_LIB})
        message(STATUS "  ✓ Linked shared providers: ${ONNXRUNTIME_PROVIDERS_SHARED_LIB}")
      else()
        message(WARNING "  ✗ Shared providers library not found")
        set(GPU_LIBS_FOUND FALSE)
      endif()

      # 可选的TensorRT库
      if(ONNXRUNTIME_PROVIDERS_TENSORRT_LIB)
        target_link_libraries(LLVMInstrumentation PRIVATE ${ONNXRUNTIME_PROVIDERS_TENSORRT_LIB})
        message(STATUS "  ✓ Linked TensorRT provider: ${ONNXRUNTIME_PROVIDERS_TENSORRT_LIB}")
      else()
        message(STATUS "  ⚠ TensorRT provider library not found (optional)")
      endif()

      if(GPU_LIBS_FOUND)
        target_compile_definitions(LLVMInstrumentation PRIVATE ONNXRUNTIME_USE_GPU=1)
        message(STATUS "✓ ONNX Runtime GPU version configured successfully")
        message(STATUS "  Linked libraries:")
        message(STATUS "    - onnxruntime.lib")
        message(STATUS "    - onnxruntime_providers_cuda.lib")
        message(STATUS "    - onnxruntime_providers_shared.lib")
        if(ONNXRUNTIME_PROVIDERS_TENSORRT_LIB)
          message(STATUS "    - onnxruntime_providers_tensorrt.lib")
        endif()
      else()
        message(WARNING "✗ Some required GPU libraries not found, falling back to CPU mode")
      endif()
    else()
      # CPU版本：只链接主库
      message(STATUS "✓ ONNX Runtime CPU version found and linked to LLVMInstrumentation")
    endif()

    message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIR}")
    message(STATUS "  Main library: ${ONNXRUNTIME_LIB}")

    # Windows下的额外信息
    if(WIN32)
      get_filename_component(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_LIB} DIRECTORY)
      message(STATUS "  Library directory: ${ONNXRUNTIME_LIB_DIR}")

      if(ONNXRUNTIME_USE_GPU AND GPU_LIBS_FOUND)
        message(STATUS "  Runtime requirements:")
        message(STATUS "    - CUDA runtime libraries (cudart64_*.dll)")
        message(STATUS "    - cuDNN libraries (cudnn64_*.dll)")
        message(STATUS "    - ONNX Runtime DLLs in PATH or executable directory")
      endif()
    endif()
  else()
    message(WARNING "✗ ONNX Runtime header file not found at: ${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
  endif()
else()
  message(WARNING "✗ ONNX Runtime not found. ML inference features will be disabled.")
  message(STATUS "To enable ML features:")
  message(STATUS "  1. Download ONNX Runtime from: https://github.com/microsoft/onnxruntime/releases")
  message(STATUS "  2. Extract to a directory (e.g., D:/onnxruntime)")
  message(STATUS "  3. Set ONNXRUNTIME_ROOT: cmake -DONNXRUNTIME_ROOT=/path/to/onnxruntime")
  message(STATUS "  4. Or set environment variable: export ONNXRUNTIME_ROOT=/path/to/onnxruntime")
endif()

# 查找和配置LibTorch
message(STATUS "Looking for LibTorch...")

# 允许用户指定LibTorch的路径
if(NOT LIBTORCH_ROOT)
  if(WIN32)
    # 首先尝试当前目录下的libtorch
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../libtorch")
      set(LIBTORCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libtorch" CACHE PATH "Path to LibTorch root directory")
    else()
      set(LIBTORCH_ROOT "D:/env/libtorch" CACHE PATH "Path to LibTorch root directory")
    endif()
  else()
    set(LIBTORCH_ROOT "/usr/local/libtorch" CACHE PATH "Path to LibTorch root directory")
  endif()
endif()

message(STATUS "LIBTORCH_ROOT: ${LIBTORCH_ROOT}")

# 查找LibTorch
find_package(Torch QUIET PATHS ${LIBTORCH_ROOT} NO_DEFAULT_PATH)

if(NOT Torch_FOUND)
  # 手动查找LibTorch
  find_path(LIBTORCH_INCLUDE_DIR
    NAMES torch/torch.h
    PATHS
      ${LIBTORCH_ROOT}/include/torch/csrc/api/include
      ${LIBTORCH_ROOT}/include
    NO_DEFAULT_PATH
  )

  find_library(TORCH_LIB
    NAMES torch torch_cpu
    PATHS
      ${LIBTORCH_ROOT}/lib
    NO_DEFAULT_PATH
  )

  find_library(TORCH_CPU_LIB
    NAMES torch_cpu
    PATHS
      ${LIBTORCH_ROOT}/lib
    NO_DEFAULT_PATH
  )

  find_library(C10_LIB
    NAMES c10
    PATHS
      ${LIBTORCH_ROOT}/lib
    NO_DEFAULT_PATH
  )

  message(STATUS "Manual LibTorch search results:")
  message(STATUS "  LIBTORCH_INCLUDE_DIR: ${LIBTORCH_INCLUDE_DIR}")
  message(STATUS "  TORCH_LIB: ${TORCH_LIB}")
  message(STATUS "  TORCH_CPU_LIB: ${TORCH_CPU_LIB}")
  message(STATUS "  C10_LIB: ${C10_LIB}")

  if(LIBTORCH_INCLUDE_DIR AND C10_LIB)
    set(LIBTORCH_FOUND TRUE)
    set(LIBTORCH_LIBRARIES ${C10_LIB})

    # CPU版本优先使用torch_cpu，如果没有则使用torch
    if(TORCH_CPU_LIB)
      list(APPEND LIBTORCH_LIBRARIES ${TORCH_CPU_LIB})
      message(STATUS "  Using torch_cpu library (CPU version)")
    elseif(TORCH_LIB)
      list(APPEND LIBTORCH_LIBRARIES ${TORCH_LIB})
      message(STATUS "  Using torch library")
    endif()

    set(LIBTORCH_INCLUDE_DIRS ${LIBTORCH_INCLUDE_DIR})
  endif()
else()
  set(LIBTORCH_FOUND TRUE)
  set(LIBTORCH_LIBRARIES ${TORCH_LIBRARIES})
  set(LIBTORCH_INCLUDE_DIRS ${TORCH_INCLUDE_DIRS})
endif()

if(LIBTORCH_FOUND)
  # 验证头文件是否存在
  if(EXISTS "${LIBTORCH_INCLUDE_DIRS}/torch/torch.h" OR EXISTS "${LIBTORCH_ROOT}/include/torch/csrc/api/include/torch/torch.h")

    # 强制设置C++17标准（必须在其他设置之前）
    set_target_properties(LLVMInstrumentation PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # 添加主要的include目录
    target_include_directories(LLVMInstrumentation PRIVATE ${LIBTORCH_INCLUDE_DIRS})

    # 添加额外的include路径以确保能找到所有头文件
    target_include_directories(LLVMInstrumentation PRIVATE ${LIBTORCH_ROOT}/include)
    target_include_directories(LLVMInstrumentation PRIVATE ${LIBTORCH_ROOT}/include/torch/csrc/api/include)

    target_compile_definitions(LLVMInstrumentation PRIVATE
        LLVM_HAS_LIBTORCH=1
        TORCH_API_INCLUDE_EXTENSION_H=1
        TORCH_PYTHON_API_DISABLE=1
        TORCH_DISABLE_PYTHON_BINDINGS=1
        PYBIND11_COMPILER_TYPE=""
        PYBIND11_STDLIB=""
        PYBIND11_BUILD_ABI=""
    )

    # Windows特定的定义
    if(WIN32)
        target_compile_definitions(LLVMInstrumentation PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _USE_MATH_DEFINES
        )
    endif()

    # 设置C++标准（LibTorch需要C++17或更高）
    target_compile_features(LLVMInstrumentation PRIVATE cxx_std_17)

    # 确保使用C++17标准
    set_target_properties(LLVMInstrumentation PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # Windows MSVC特定设置
    if(WIN32 AND MSVC)
        target_compile_options(LLVMInstrumentation PRIVATE
            /std:c++17          # 强制C++17标准
            /permissive-        # 禁用非标准扩展
            /Zc:__cplusplus     # 正确设置__cplusplus宏
            /Zc:preprocessor    # 使用标准预处理器
            /bigobj             # 支持大对象文件
        )

        # 添加LibTorch特定的编译器定义
        target_compile_definitions(LLVMInstrumentation PRIVATE
            _ENABLE_EXTENDED_ALIGNED_STORAGE  # 修复aligned_storage问题
        )
    endif()

    # GCC/Clang设置
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(LLVMInstrumentation PRIVATE
            -std=c++17
        )
    endif()

    # 链接LibTorch库
    target_link_libraries(LLVMInstrumentation PRIVATE ${LIBTORCH_LIBRARIES})

    message(STATUS "✓ LibTorch found and linked to LLVMInstrumentation")
    message(STATUS "  Primary Include: ${LIBTORCH_INCLUDE_DIRS}")
    message(STATUS "  Additional Include: ${LIBTORCH_ROOT}/include")
    message(STATUS "  API Include: ${LIBTORCH_ROOT}/include/torch/csrc/api/include")
    message(STATUS "  Libraries: ${LIBTORCH_LIBRARIES}")

    # 验证关键头文件
    if(EXISTS "${LIBTORCH_ROOT}/include/torch/csrc/api/include/torch/all.h")
      message(STATUS "  ✓ torch/all.h found")
    else()
      message(WARNING "  ✗ torch/all.h not found")
    endif()

    # Windows下的额外信息
    if(WIN32)
      message(STATUS "  Runtime requirements (CPU version):")
      message(STATUS "    - LibTorch CPU DLLs in PATH or executable directory")
      message(STATUS "    - Required DLLs: torch_cpu.dll, c10.dll")
      message(STATUS "    - Optional: torch.dll (if using unified library)")
      message(STATUS "    - No CUDA dependencies required for CPU version")
      message(STATUS "    - DLL location: ${LIBTORCH_ROOT}/lib")
    endif()
  else()
    message(WARNING "✗ LibTorch header file not found at expected locations")
    message(STATUS "  Checked: ${LIBTORCH_INCLUDE_DIRS}/torch/torch.h")
    message(STATUS "  Checked: ${LIBTORCH_ROOT}/include/torch/torch.h")
  endif()
else()
  message(WARNING "✗ LibTorch not found. GNN inference features will be disabled.")
  message(STATUS "To enable LibTorch features:")
  message(STATUS "  1. Download LibTorch from: https://pytorch.org/get-started/locally/")
  message(STATUS "  2. Extract to a directory (e.g., D:/env/libtorch)")
  message(STATUS "  3. Set LIBTORCH_ROOT: cmake -DLIBTORCH_ROOT=/path/to/libtorch")
  message(STATUS "  4. Or set environment variable: export LIBTORCH_ROOT=/path/to/libtorch")
endif()

# Windows特定的tokenizers-cpp路径设置
if(WIN32)
  # 允许用户指定tokenizers-cpp的路径
  set(TOKENIZERS_CPP_ROOT "D:/exam/tokenizers-cpp" CACHE PATH "Path to tokenizers-cpp root directory")

  # 打印调试信息
  message(STATUS "Looking for tokenizers-cpp in: ${TOKENIZERS_CPP_ROOT}")

  # 检查目录是否存在
  if(EXISTS "${TOKENIZERS_CPP_ROOT}")
    message(STATUS "✓ Root directory exists: ${TOKENIZERS_CPP_ROOT}")
  else()
    message(WARNING "✗ Root directory does not exist: ${TOKENIZERS_CPP_ROOT}")
  endif()

  if(EXISTS "${TOKENIZERS_CPP_ROOT}/build")
    message(STATUS "✓ Build directory exists: ${TOKENIZERS_CPP_ROOT}/build")
  else()
    message(WARNING "✗ Build directory does not exist: ${TOKENIZERS_CPP_ROOT}/build")
  endif()

  if(EXISTS "${TOKENIZERS_CPP_ROOT}/include")
    message(STATUS "✓ Include directory exists: ${TOKENIZERS_CPP_ROOT}/include")
  else()
    message(WARNING "✗ Include directory does not exist: ${TOKENIZERS_CPP_ROOT}/include")
  endif()

  # 查找库文件（Windows下的.lib文件）- 基于实际构建结构
  find_library(TOKENIZERS_CPP_LIB
    NAMES tokenizers_cpp
    PATHS
      ${TOKENIZERS_CPP_ROOT}/build
      ${TOKENIZERS_CPP_ROOT}/build/x86_64-pc-windows-msvc/release
    NO_DEFAULT_PATH
  )

  # 查找头文件
  find_path(TOKENIZERS_CPP_INCLUDE
    NAMES tokenizers_cpp.h
    PATHS ${TOKENIZERS_CPP_ROOT}/include
    NO_DEFAULT_PATH
  )

  # 查找tokenizers_c库（Rust编译的库）
  find_library(TOKENIZERS_C_LIB
    NAMES tokenizers_c
    PATHS
      ${TOKENIZERS_CPP_ROOT}/build
      ${TOKENIZERS_CPP_ROOT}/build/x86_64-pc-windows-msvc/release
      ${TOKENIZERS_CPP_ROOT}/build/release
    NO_DEFAULT_PATH
  )

  # 查找sentencepiece库
  find_library(SENTENCEPIECE_LIB
    NAMES sentencepiece sentencepiece-static
    PATHS
      ${TOKENIZERS_CPP_ROOT}/build/sentencepiece/src
    NO_DEFAULT_PATH
  )

  # 查找msgpack库
  find_library(MSGPACK_LIB
    NAMES msgpack-cxx msgpack
    PATHS
      ${TOKENIZERS_CPP_ROOT}/build/msgpack/Release
      ${TOKENIZERS_CPP_ROOT}/build/msgpack
    NO_DEFAULT_PATH
  )

  # 打印找到的库文件
  message(STATUS "TOKENIZERS_CPP_LIB: ${TOKENIZERS_CPP_LIB}")
  message(STATUS "TOKENIZERS_C_LIB: ${TOKENIZERS_C_LIB}")
  message(STATUS "SENTENCEPIECE_LIB: ${SENTENCEPIECE_LIB}")
  message(STATUS "TOKENIZERS_CPP_INCLUDE: ${TOKENIZERS_CPP_INCLUDE}")

  # 如果find_library没找到，尝试直接指定路径
  if(NOT TOKENIZERS_CPP_LIB AND EXISTS "${TOKENIZERS_CPP_ROOT}/build/Debug/tokenizers_cpp.lib")
    set(TOKENIZERS_CPP_LIB "${TOKENIZERS_CPP_ROOT}/build/Debug/tokenizers_cpp.lib")
  endif()
  if(NOT TOKENIZERS_C_LIB AND EXISTS "${TOKENIZERS_CPP_ROOT}/build/tokenizers_c.lib")
    set(TOKENIZERS_C_LIB "${TOKENIZERS_CPP_ROOT}/build/tokenizers_c.lib")
  endif()
  if(NOT SENTENCEPIECE_LIB AND EXISTS "${TOKENIZERS_CPP_ROOT}/build/sentencepiece/src/Debug/sentencepiece.lib")
    set(SENTENCEPIECE_LIB "${TOKENIZERS_CPP_ROOT}/build/sentencepiece/src/Debug/sentencepiece.lib")
  endif()
  if(NOT TOKENIZERS_CPP_INCLUDE AND EXISTS "${TOKENIZERS_CPP_ROOT}/include/tokenizers_cpp.h")
    set(TOKENIZERS_CPP_INCLUDE "${TOKENIZERS_CPP_ROOT}/include")
  endif()

  # 检查必需的库是否都找到了
  if(TOKENIZERS_CPP_LIB AND TOKENIZERS_CPP_INCLUDE AND TOKENIZERS_C_LIB)
    set(TOKENIZERS_CPP_FOUND TRUE)
    set(TOKENIZERS_CPP_LIBRARIES ${TOKENIZERS_CPP_LIB} ${TOKENIZERS_C_LIB})

    # 添加可选的依赖库
    if(SENTENCEPIECE_LIB)
      list(APPEND TOKENIZERS_CPP_LIBRARIES ${SENTENCEPIECE_LIB})
    endif()
    if(MSGPACK_LIB)
      list(APPEND TOKENIZERS_CPP_LIBRARIES ${MSGPACK_LIB})
    endif()

    set(TOKENIZERS_CPP_INCLUDE_DIRS ${TOKENIZERS_CPP_INCLUDE})

    # Windows系统库依赖
    list(APPEND TOKENIZERS_CPP_LIBRARIES
      ntdll wsock32 ws2_32 Bcrypt iphlpapi userenv psapi)

    message(STATUS "Successfully configured tokenizers-cpp libraries:")
    message(STATUS "  - tokenizers_cpp: ${TOKENIZERS_CPP_LIB}")
    message(STATUS "  - tokenizers_c: ${TOKENIZERS_C_LIB}")
    message(STATUS "  - sentencepiece: ${SENTENCEPIECE_LIB}")
    message(STATUS "  - include dir: ${TOKENIZERS_CPP_INCLUDE}")
  else()
    message(WARNING "Required tokenizers-cpp libraries not found:")
    if(NOT TOKENIZERS_CPP_LIB)
      message(WARNING "  - tokenizers_cpp library not found")
    endif()
    if(NOT TOKENIZERS_C_LIB)
      message(WARNING "  - tokenizers_c library not found")
    endif()
    if(NOT TOKENIZERS_CPP_INCLUDE)
      message(WARNING "  - tokenizers_cpp.h header not found")
    endif()
    message(STATUS "Please ensure tokenizers-cpp is built in: ${TOKENIZERS_CPP_ROOT}/build/")
  endif()
else()
  # Linux/macOS的查找逻辑
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(TOKENIZERS_CPP QUIET tokenizers_cpp)
  endif()

  if(NOT TOKENIZERS_CPP_FOUND)
    find_library(TOKENIZERS_CPP_LIB
      NAMES tokenizers_cpp libtokenizers_cpp
      PATHS /usr/local/lib /usr/lib ${CMAKE_PREFIX_PATH}/lib
    )
    find_path(TOKENIZERS_CPP_INCLUDE
      NAMES tokenizers_cpp.h
      PATHS /usr/local/include /usr/include ${CMAKE_PREFIX_PATH}/include
    )

    if(TOKENIZERS_CPP_LIB AND TOKENIZERS_CPP_INCLUDE)
      set(TOKENIZERS_CPP_FOUND TRUE)
      set(TOKENIZERS_CPP_LIBRARIES ${TOKENIZERS_CPP_LIB})
      set(TOKENIZERS_CPP_INCLUDE_DIRS ${TOKENIZERS_CPP_INCLUDE})
    endif()
  endif()
endif()

# 如果找到tokenizers-cpp，添加相关依赖
if(TOKENIZERS_CPP_FOUND)
  # 链接tokenizers-cpp库
  target_link_libraries(LLVMInstrumentation PRIVATE ${TOKENIZERS_CPP_LIBRARIES})

  # 添加头文件路径
  target_include_directories(LLVMInstrumentation PRIVATE ${TOKENIZERS_CPP_INCLUDE_DIRS})

  # 添加编译定义
  target_compile_definitions(LLVMInstrumentation PRIVATE LLVM_HAS_TOKENIZERS_CPP=1)

  # 确保C++17支持
  target_compile_features(LLVMInstrumentation PRIVATE cxx_std_17)

  message(STATUS "CFGInfoPass: tokenizers-cpp support enabled")
else()
  message(WARNING "CFGInfoPass: tokenizers-cpp not found, building without tokenizer support")
  message(STATUS "To enable tokenizer support, install tokenizers-cpp or set CMAKE_PREFIX_PATH")
endif()
