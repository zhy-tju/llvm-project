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
  ManualFeaturesCFGPass.cpp
  IRTokenizer.cpp
  MLBranchPredictionPass.cpp
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
    set(ONNXRUNTIME_ROOT "/opt/onnxruntime" CACHE PATH "Path to ONNX Runtime root directory")
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

