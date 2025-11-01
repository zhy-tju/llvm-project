#include "llvm/Transforms/Instrumentation/MLBranchPredictionPass.h"
#include "llvm/Transforms/Instrumentation/IRTokenizer.h" // 引入您的分词器类

// LLVM Includes
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/MDBuilder.h" // 用于创建元数据
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/ADT/DenseMap.h"
// Standard C++ Includes
#include <vector>
#include <string>
#include <set>
#include <map>
#include <queue>
#include <deque>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <numeric> // For std::accumulate
#include <system_error> // For std::error_code
#include <codecvt> // For Windows path conversion if needed
#include <locale>  // For Windows path conversion if needed
#include <cstdint>
#include <memory>
#ifdef _WIN32
#include <windows.h> // <-- 添加 Windows 头文件
#endif
#include <fstream>      // <-- 新增：用于文件输出
#include <iomanip>      // <-- 新增：用于设置浮点数精度
// ONNX Runtime Includes
#include <onnxruntime_cxx_api.h>

// JSON Includes (single-header nlohmann/json placed under llvm/include/...)
#include "llvm/Transforms/Instrumentation/json.hpp"
using json = nlohmann::json;


using namespace llvm;

// --- 命令行选项 ---
namespace {
// 主开关
static cl::opt<bool> EnableMLBranchPrediction(
    "enable-ml-branch-prediction", cl::init(false), cl::Hidden,
    cl::desc("Enable ML-based branch prediction metadata generation"));

// 模型和数据路径
static cl::opt<std::string> KnownTokensPath(
    "ml-known-tokens-path", cl::init("known_tokens.json"), cl::Hidden,
    cl::desc("Path to the known_tokens.json file for IR Tokenizer"));

static cl::opt<std::string> VocabPath(
    "ml-vocab-path", cl::init("vocab.json"), cl::Hidden,
    cl::desc("Path to the vocab.json file for IR Tokenizer"));

static cl::opt<std::string> FeatureExtractorOnnxPath(
    "ml-feature-extractor-onnx-path", cl::init("feature_extractor.onnx"), cl::Hidden,
    cl::desc("Path to the feature extractor ONNX model file"));

static cl::opt<std::string> GNNPredictorOnnxPath(
    "ml-gnn-predictor-onnx-path", cl::init("gnn_predictor.onnx"), cl::Hidden,
    cl::desc("Path to the differential GNN predictor ONNX model file"));
// 【核心新增】控制调试日志输出的开关和文件名
static cl::opt<bool> EnableMLDebugLog(
    "enable-ml-debug-log", cl::init(false), cl::Hidden,
    cl::desc("Enable detailed ML prediction debug logging to a file"));

static cl::opt<std::string> MLDebugLogPath(
    "ml-debug-log-path", cl::init("ml_prediction_debug_log.txt"), cl::Hidden,
    cl::desc("Path for the ML prediction debug log file"));
// 超参数
static cl::opt<int> MaxLength(
    "ml-max-len", cl::init(256), cl::Hidden,
    cl::desc("Max sequence length for tokenizer"));

static cl::opt<int> KHops(
    "ml-k-hops", cl::init(3), cl::Hidden,
    cl::desc("Number of hops for successor BFS subgraph extraction"));

static cl::opt<double> SkewThreshold(
    "ml-skew-threshold", cl::init(0.9), cl::Hidden,
    cl::desc("Probability threshold for biased branches"));

static cl::opt<int> CountThreshold(
    "ml-count-threshold", cl::init(100), cl::Hidden,
    cl::desc("Minimum total counts for a branch to be considered hot"));

} // end anonymous namespace


// --- 全局变量与初始化 ---
namespace {
// 使用智能指针管理全局对象
static std::unique_ptr<IRTokenizer::LLVMIRTokenizer> GlobalTokenizer;
static std::unique_ptr<Ort::Env> GlobalOrtEnv;
static std::unique_ptr<Ort::Session> FeatureExtractorSession;
static std::unique_ptr<Ort::Session> GNNPredictorSession;
static std::once_flag GlobalInitStatus; // 控制全局初始化只执行一次
static std::mutex InferenceMutex;        // 保护ONNX会话的线程安全

#ifdef _WIN32
// Helper function to convert UTF-8 string to wstring on Windows
std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
#endif


// 初始化函数
void initializeGlobalResources() {
    try {
        errs() << "ML Predictor Pass: Initializing global resources...\n";

        // 1. 初始化分词器
        GlobalTokenizer = std::make_unique<IRTokenizer::LLVMIRTokenizer>(KnownTokensPath, VocabPath);
        // (可以在这里添加对tokenizer加载成功的检查, 例如检查vocabMap是否为空)
        if (!GlobalTokenizer) { // 假设构造函数失败会返回nullptr或抛出异常
             throw std::runtime_error("Tokenizer initialization failed.");
        }


        // 2. 初始化ONNX Runtime环境
        GlobalOrtEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "LLVM_ML_Predictor");

        // 3. 加载ONNX模型会话
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1); // 优化单次推理性能
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        #ifdef _WIN32
            std::wstring featureExtractorPathW = utf8_to_wstring(FeatureExtractorOnnxPath);
            std::wstring gnnPredictorPathW = utf8_to_wstring(GNNPredictorOnnxPath);
            FeatureExtractorSession = std::make_unique<Ort::Session>(*GlobalOrtEnv, featureExtractorPathW.c_str(), sessionOptions);
            GNNPredictorSession = std::make_unique<Ort::Session>(*GlobalOrtEnv, gnnPredictorPathW.c_str(), sessionOptions);
        #else
            FeatureExtractorSession = std::make_unique<Ort::Session>(*GlobalOrtEnv, FeatureExtractorOnnxPath.c_str(), sessionOptions);
            GNNPredictorSession = std::make_unique<Ort::Session>(*GlobalOrtEnv, GNNPredictorOnnxPath.c_str(), sessionOptions);
        #endif


        errs() << "  - Tokenizer initialized.\n";
        errs() << "  - Feature Extractor ONNX model loaded from: " << FeatureExtractorOnnxPath << "\n";
        errs() << "  - GNN Predictor ONNX model loaded from: " << GNNPredictorOnnxPath << "\n";
        errs() << "ML Predictor Pass: Global resources initialized successfully.\n";

    } catch (const Ort::Exception& e) {
        errs() << "ONNX Runtime exception during initialization: " << e.what() << "\n";
        FeatureExtractorSession.reset();
        GNNPredictorSession.reset();
        GlobalOrtEnv.reset();
        GlobalTokenizer.reset();
    } catch (const std::exception& e) {
        errs() << "Standard exception during initialization: " << e.what() << "\n";
        FeatureExtractorSession.reset();
        GNNPredictorSession.reset();
        GlobalOrtEnv.reset();
        GlobalTokenizer.reset();
    } catch (...) {
        errs() << "Unknown exception during initialization.\n";
        FeatureExtractorSession.reset();
        GNNPredictorSession.reset();
        GlobalOrtEnv.reset();
        GlobalTokenizer.reset();
    }
}

} // end anonymous namespace

// --- 辅助函数 ---
namespace {

// 获取基本块IR字符串
std::string getBasicBlockString(const BasicBlock &BB) {
    std::string BBStr;
    raw_string_ostream RSO(BBStr);
    bool first = true;
    for (const Instruction &I : BB) {
        if (!I.isDebugOrPseudoInst()) {
             if (!first) RSO << '\n';
             I.print(RSO);
             first = false;
        }
    }
    return RSO.str();
}

// 结构体：临时存储子图信息
struct SubgraphComponents {
    BasicBlock* pgoBlock = nullptr;
    std::vector<BasicBlock*> nodesOrdered;
    std::map<BasicBlock*, int> bbToLocalIndex;
    std::vector<std::pair<int, int>> edgeIndexList;
    int pgoNodeLocalIndex = -1;
    int leftSuccLocalIndex = -1;
    int rightSuccLocalIndex = -1;
};

// 提取K跳后继子图
bool extractKHopSuccessorSubgraph(BasicBlock* startBlock, int K, SubgraphComponents& result) {
    std::deque<std::pair<BasicBlock*, int>> Q;
    std::set<BasicBlock*> visitedNodes;

    result.pgoBlock = startBlock;
    result.nodesOrdered.push_back(startBlock);
    result.bbToLocalIndex[startBlock] = 0;
    result.pgoNodeLocalIndex = 0;
    visitedNodes.insert(startBlock);
    Q.push_back({startBlock, 0});

    int currentNodeIndex = 1;

    while (!Q.empty()) {
        BasicBlock* currentBB = Q.front().first;
        int currentDist = Q.front().second;
        Q.pop_front();

        int srcLocalIndex = result.bbToLocalIndex[currentBB];

        if (currentDist >= K) continue;

        auto *TI = currentBB->getTerminator();
        for (unsigned i = 0, e = TI->getNumSuccessors(); i != e; ++i) {
            BasicBlock *succBB = TI->getSuccessor(i);
            int dstLocalIndex;

            if (result.bbToLocalIndex.count(succBB)) {
                dstLocalIndex = result.bbToLocalIndex[succBB];
            } else {
                dstLocalIndex = currentNodeIndex++;
                result.nodesOrdered.push_back(succBB);
                result.bbToLocalIndex[succBB] = dstLocalIndex;
                visitedNodes.insert(succBB);
                Q.push_back({succBB, currentDist + 1});
            }
            result.edgeIndexList.push_back({srcLocalIndex, dstLocalIndex});
        }
    }

    BranchInst *BI = dyn_cast<BranchInst>(startBlock->getTerminator());
    if (!BI || !BI->isConditional() || BI->getNumSuccessors() != 2) return false;

    BasicBlock *leftSuccBB = BI->getSuccessor(0);
    BasicBlock *rightSuccBB = BI->getSuccessor(1);

    if (!result.bbToLocalIndex.count(leftSuccBB) || !result.bbToLocalIndex.count(rightSuccBB)) {
        return false;
    }
    result.leftSuccLocalIndex = result.bbToLocalIndex[leftSuccBB];
    result.rightSuccLocalIndex = result.bbToLocalIndex[rightSuccBB];

    return true;
}


// ONNX Feature Extractor 推理 (批量)
std::vector<std::vector<float>> runFeatureExtractor(
    const std::vector<std::vector<int>>& batchInputIds,
    const std::vector<std::vector<int>>& batchAttentionMask)
{
    if (!FeatureExtractorSession) {
        errs() << "错误：FeatureExtractorSession 未初始化！\n";
        return {};
    }
    size_t batchSize = batchInputIds.size();
    if (batchSize == 0 || batchAttentionMask.size() != batchSize) {
         errs() << "错误：特征提取器的输入批次大小不匹配或为空。\n";
        return {};
    }

    std::lock_guard<std::mutex> lock(InferenceMutex);

    try {
        size_t sequenceLength = batchInputIds[0].size(); // 假设所有序列已填充到相同长度
        std::vector<int64_t> currentInputShape = {static_cast<int64_t>(batchSize), static_cast<int64_t>(sequenceLength)};

        // 展平数据
        std::vector<int64_t> flatInputIds; flatInputIds.reserve(batchSize * sequenceLength);
        std::vector<int64_t> flatAttentionMask; flatAttentionMask.reserve(batchSize * sequenceLength);
        for (const auto& vec : batchInputIds) for (int id : vec) flatInputIds.push_back(static_cast<int64_t>(id));
        for (const auto& vec : batchAttentionMask) for (int mask : vec) flatAttentionMask.push_back(static_cast<int64_t>(mask));

        // 创建输入张量
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputIdsTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, flatInputIds.data(), flatInputIds.size(), currentInputShape.data(), currentInputShape.size());
        Ort::Value attentionMaskTensor = Ort::Value::CreateTensor<int64_t>(memoryInfo, flatAttentionMask.data(), flatAttentionMask.size(), currentInputShape.data(), currentInputShape.size());

        std::vector<Ort::Value> inputTensors;
        inputTensors.push_back(std::move(inputIdsTensor));
        inputTensors.push_back(std::move(attentionMaskTensor));

        // 获取输入输出名称 (从会话动态获取更健壮)
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName0 = FeatureExtractorSession->GetInputNameAllocated(0, allocator);
        auto inputName1 = FeatureExtractorSession->GetInputNameAllocated(1, allocator);
        auto outputName0 = FeatureExtractorSession->GetOutputNameAllocated(0, allocator);
        std::vector<const char*> inputNames = {inputName0.get(), inputName1.get()};
        std::vector<const char*> outputNames = {outputName0.get()};


        // 运行推理
        auto outputTensors = FeatureExtractorSession->Run(Ort::RunOptions{nullptr}, inputNames.data(), inputTensors.data(), inputNames.size(), outputNames.data(), outputNames.size());

        if (outputTensors.empty() || !outputTensors[0].IsTensor()) { throw std::runtime_error("Feature Extractor ONNX 推理未返回有效张量。"); }

        const float* outputData = outputTensors[0].GetTensorData<float>();
        auto outputShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();

        if (outputShape.size() != 3 || static_cast<size_t>(outputShape[0]) != batchSize) { throw std::runtime_error("Feature Extractor ONNX 输出张量形状不符合预期 [batch, seq_len, hidden]。"); }
        size_t hiddenSize = outputShape[2];

        // 提取CLS嵌入
        std::vector<std::vector<float>> clsEmbeddings;
        clsEmbeddings.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i) {
            std::vector<float> embedding(hiddenSize);
            const float* start = outputData + i * sequenceLength * hiddenSize;
            std::memcpy(embedding.data(), start, hiddenSize * sizeof(float));
            clsEmbeddings.push_back(embedding);
        }
        return clsEmbeddings;

    } catch (const Ort::Exception& e) {
        errs() << "ONNX Feature Extractor 推理失败: " << e.what() << "\n";
        return {};
    } catch (const std::exception& e) {
        errs() << "Feature Extractor 推理中发生标准异常: " << e.what() << "\n";
        return {};
    }
}


// ONNX Differential GNN 推理 (单图)
std::optional<std::vector<float>> runGNNPredictor(
    const std::vector<std::vector<float>>& combinedFeatures,
    const std::vector<std::pair<int, int>>& edgeIndexList,
    int pgoNodeLocalIndex,
    const std::vector<bool>& leftMask,
    const std::vector<bool>& rightMask
) {
    if (!GNNPredictorSession) { /* ... */ return std::nullopt; }
    if (combinedFeatures.empty()) { /* ... */ return std::nullopt; }

    std::lock_guard<std::mutex> lock(InferenceMutex);

    try {
        size_t numNodes = combinedFeatures.size();
        size_t featureDim = combinedFeatures[0].size();
        size_t numEdges = edgeIndexList.size();

        // 1. 准备节点特征 x [num_nodes, feature_dim]
        std::vector<float> flatFeatures; flatFeatures.reserve(numNodes * featureDim);
        for(const auto& nf : combinedFeatures) flatFeatures.insert(flatFeatures.end(), nf.begin(), nf.end());
        std::vector<int64_t> xShape = {static_cast<int64_t>(numNodes), static_cast<int64_t>(featureDim)};

        // 2. 准备 edge_index [2, num_edges]
        std::vector<int64_t> edgeIndexData; edgeIndexData.reserve(numEdges * 2);
        for(const auto& edge : edgeIndexList) edgeIndexData.push_back(static_cast<int64_t>(edge.first));
        for(const auto& edge : edgeIndexList) edgeIndexData.push_back(static_cast<int64_t>(edge.second));
        std::vector<int64_t> edgeIndexShape = {2, static_cast<int64_t>(numEdges)};

        // 3. 准备左右路径掩码 [num_edges]
        // ONNX 要求传入真实的 bool*（vector<bool> 不是连续存储），因此分配连续的 bool 缓冲区
        std::unique_ptr<bool[]> leftMaskBuf(new bool[numEdges]);
        std::unique_ptr<bool[]> rightMaskBuf(new bool[numEdges]);
        for (size_t i = 0; i < numEdges; ++i) {
            leftMaskBuf[i] = leftMask[i];
            rightMaskBuf[i] = rightMask[i];
        }
        std::vector<int64_t> maskShape = {static_cast<int64_t>(numEdges)};

        // 4. 准备 PGO 节点索引 [1]
        std::vector<int64_t> pgoIndexData = {static_cast<int64_t>(pgoNodeLocalIndex)};
        std::vector<int64_t> pgoIndexShape = {1};

        // 5. 准备 batch 索引 [num_nodes] (全为0)
        std::vector<int64_t> batchData(numNodes, 0);
        std::vector<int64_t> batchShape = {static_cast<int64_t>(numNodes)};


        // 创建输入张量
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputTensors;
    inputTensors.push_back(Ort::Value::CreateTensor<float>(memoryInfo, flatFeatures.data(), flatFeatures.size(), xShape.data(), xShape.size()));
    inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, edgeIndexData.data(), edgeIndexData.size(), edgeIndexShape.data(), edgeIndexShape.size()));
    // 传入 bool* 指针，注意 leftMaskBuf/rightMaskBuf 的生命周期需至少到 Run() 返回
    inputTensors.push_back(Ort::Value::CreateTensor<bool>(memoryInfo, leftMaskBuf.get(), static_cast<size_t>(numEdges), maskShape.data(), maskShape.size()));
    inputTensors.push_back(Ort::Value::CreateTensor<bool>(memoryInfo, rightMaskBuf.get(), static_cast<size_t>(numEdges), maskShape.data(), maskShape.size()));
        inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, pgoIndexData.data(), pgoIndexData.size(), pgoIndexShape.data(), pgoIndexShape.size()));
        inputTensors.push_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, batchData.data(), batchData.size(), batchShape.data(), batchShape.size()));

        // 获取输入输出名称 (从会话动态获取更健壮)
        Ort::AllocatorWithDefaultOptions allocator;
        std::vector<Ort::AllocatedStringPtr> inputNameAllocatedStrings;
        std::vector<const char*> inputNames;
        size_t numInputNodes = GNNPredictorSession->GetInputCount();
        inputNameAllocatedStrings.reserve(numInputNodes);
        inputNames.reserve(numInputNodes);
        for (size_t i = 0; i < numInputNodes; i++) {
            inputNameAllocatedStrings.push_back(GNNPredictorSession->GetInputNameAllocated(i, allocator));
            inputNames.push_back(inputNameAllocatedStrings.back().get());
        }

        auto outputName0 = GNNPredictorSession->GetOutputNameAllocated(0, allocator);
        std::vector<const char*> outputNames = {outputName0.get()};


        // 运行推理
        auto outputTensors = GNNPredictorSession->Run(Ort::RunOptions{nullptr}, inputNames.data(), inputTensors.data(), inputNames.size(), outputNames.data(), outputNames.size());

        if (outputTensors.empty() || !outputTensors[0].IsTensor()) { throw std::runtime_error("GNN ONNX 推理未返回有效张量。"); }

        // 获取输出 logits [1, 3]
        const float* outputData = outputTensors[0].GetTensorData<float>();
        auto outputShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();

        if (outputShape.size() != 2 || outputShape[0] != 1 || outputShape[1] != 3) { throw std::runtime_error("GNN ONNX 输出张量形状不符合预期 [1, 3]。"); }

        std::vector<float> logits(outputData, outputData + 3);
        return logits;

    } catch (const Ort::Exception& e) {
        errs() << "ONNX GNN Predictor 推理失败: " << e.what() << "\n";
        return std::nullopt;
    } catch (const std::exception& e) {
        errs() << "GNN Predictor 推理中发生标准异常: " << e.what() << "\n";
        return std::nullopt;
    }
}


// 将预测标签转换为 !prof 权重
std::pair<uint64_t, uint64_t> labelToWeights(int predictedLabel) {
    if (predictedLabel == 1) { // 左偏向
        return {1000000, 1};
    } else if (predictedLabel == 2) { // 右偏向
        return {1, 1000000};
    } else { // 类别 0 或 预测失败
        return {1, 1}; // 返回均衡权重
    }
}

// 【核心新增】计算均值和标准差的辅助函数
template <typename T>
double calculateMean(const std::vector<T>& data) {
    if (data.empty()) return 0.0;
    // 使用 double 进行累加避免溢出
    double sum = 0.0;
    for(const T& val : data) {
        sum += static_cast<double>(val);
    }
    return sum / data.size();
}

template <typename T>
double calculateStdDev(const std::vector<T>& data, double mean) {
    if (data.size() < 2) return 0.0; // 样本数小于2，标准差无意义或为0
    double sq_sum = 0.0;
    for (const T& val : data) {
        double diff = static_cast<double>(val) - mean;
        sq_sum += diff * diff;
    }
    // 使用样本标准差 (N-1)
    return std::sqrt(sq_sum / (data.size() - 1));
}

} // end anonymous namespace


// --- Pass Implementation ---

PreservedAnalyses MLBranchPredictionPass::run(Module &M, ModuleAnalysisManager &AM) {
    if (!EnableMLBranchPrediction) {
        return PreservedAnalyses::all();
    }

    std::call_once(GlobalInitStatus, initializeGlobalResources);
    if (!GlobalTokenizer || !FeatureExtractorSession || !GNNPredictorSession) {
        errs() << "ML Predictor Pass: 全局资源初始化失败，Pass 终止。\n";
        return PreservedAnalyses::all();
    }

    std::unique_ptr<std::ofstream> debugFile;
    if (EnableMLDebugLog) {
        std::ofstream debugFileStream(MLDebugLogPath, std::ios::app);
        if (!debugFileStream.is_open()) {
            errs() << "警告：无法打开调试日志文件 '" << MLDebugLogPath << "' 进行写入。\n";
        } else {
            debugFile = std::make_unique<std::ofstream>(std::move(debugFileStream));
            *debugFile << "\n\n=== Processing Module: " << M.getName().str() << " ===\n";
            *debugFile << "Timestamp: " << time(nullptr) << "\n";
        }
    }

    auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    int branchesPredicted = 0;
    int branchesSkipped = 0;
    errs() << "ML Predictor Pass: Starting prediction for module '" << M.getName() << "'...\n";

    for (Function &F : M) {
        if (F.isDeclaration() || F.empty()) continue;
        StringRef funcName = F.getName();
        if (funcName.starts_with("llvm.") || funcName.starts_with("__") || funcName.starts_with("_Z") || funcName.starts_with("??")) {
            continue;
        }

        bool functionHasBranch = false;
        for (BasicBlock &BB : F) {
             auto * term = BB.getTerminator();
             if (auto *BI = dyn_cast_or_null<BranchInst>(term)) {
                 if (BI->isConditional()) { functionHasBranch = true; break; }
             }
        }
        if (!functionHasBranch) continue;

        auto &LI = FAM.getResult<LoopAnalysis>(F);
        auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);

        // --- 【核心优化 步骤 A: 预收集所有目标分支和所需节点】 ---

        // 1. 收集函数中所有的二元条件分支
        std::vector<BranchInst*> targetBranches;
        DenseMap<const BasicBlock*, unsigned> BBIndexMap; // 用于JSON索引
        unsigned CurrentIndex = 0;

        for (BasicBlock &BB : F) {
             BBIndexMap[&BB] = CurrentIndex++; // 构建JSON索引
             auto * term = BB.getTerminator();
             if (auto *BI = dyn_cast_or_null<BranchInst>(term)) {
                 if (BI->isConditional() && BI->getNumSuccessors() == 2) {
                     targetBranches.push_back(BI);
                 }
             }
        }
        if (targetBranches.empty()) continue;

        // 2. 预先提取所有子图，收集所有“需要”的节点
        errs() << "  - " << F.getName() << ": Pre-collecting nodes from " << targetBranches.size() << " branches...\n";
        std::set<BasicBlock*> neededNodes;
        std::map<BranchInst*, SubgraphComponents> subgraphCache; 
        
        for (BranchInst *BI : targetBranches) {
            BasicBlock *pgoBlock = BI->getParent();
            if (!pgoBlock) continue;

            SubgraphComponents subgraph;
            if (extractKHopSuccessorSubgraph(pgoBlock, KHops, subgraph)) {
                subgraphCache[BI] = subgraph;
                for (BasicBlock* node : subgraph.nodesOrdered) {
                    if (node) neededNodes.insert(node); // 增加空指针检查
                }
            } else {
                 branchesSkipped++;
            }
        }

        if (neededNodes.empty()) {
            errs() << "  - " << F.getName() << ": No valid subgraphs found. Skipping function.\n";
            continue;
        }

        // 3. 【优化】仅对 "neededNodes" 进行批量特征提取
        errs() << "  - " << F.getName() << ": Found " << neededNodes.size() << " unique nodes needed (out of " << F.size() << " total). Running batch feature extraction...\n";
        
        std::vector<BasicBlock*> neededNodesVec(neededNodes.begin(), neededNodes.end());
        DenseMap<BasicBlock*, int> bbToFuncBatchIndex;
        std::vector<std::vector<int>> funcBatchInputIds;
        std::vector<std::vector<int>> funcBatchAttentionMask;
        std::vector<int> funcRawTokensLen; // 缓存Token长度
        std::vector<int> funcRawNumPreds; // 缓存前驱数
        std::vector<int> funcRawNumSuccs; // 缓存后继数
        
        for (size_t i = 0; i < neededNodesVec.size(); ++i) {
            BasicBlock* bb = neededNodesVec[i];
            bbToFuncBatchIndex[bb] = i;

            std::string bbStr = getBasicBlockString(*bb);
            IRTokenizer::ModelInput input = GlobalTokenizer->prepareForModel(bbStr, MaxLength, "truncate_head");
            funcBatchInputIds.push_back(input.input_ids);
            funcBatchAttentionMask.push_back(input.attention_mask);
            
            // 预先计算并缓存所有手工特征
            funcRawTokensLen.push_back(input.original_token_len);
            funcRawNumPreds.push_back(std::distance(pred_begin(bb), pred_end(bb)));
            auto* term = bb->getTerminator();
            funcRawNumSuccs.push_back(term ? term->getNumSuccessors() : 0);
        }

        // 4. 运行特征提取器
        std::vector<std::vector<float>> funcSemanticFeatures = runFeatureExtractor(funcBatchInputIds, funcBatchAttentionMask);
        if (funcSemanticFeatures.size() != neededNodes.size()) {
            errs() << "  - [ERROR] Feature extraction failed for " << F.getName() << ". Skipping function.\n";
            continue;
        }

        // 5. 将提取到的特征存入一个函数本地缓存 (DenseMap)
        DenseMap<BasicBlock*, const std::vector<float>*> funcFeatureCache;
        for (size_t i = 0; i < neededNodesVec.size(); ++i) {
            funcFeatureCache[neededNodesVec[i]] = &funcSemanticFeatures[i];
        }

        // 6. 将手工特征也存入缓存
        DenseMap<BasicBlock*, std::tuple<int, int, int>> funcManualFeatureCache;
        for (size_t i = 0; i < neededNodesVec.size(); ++i) {
             funcManualFeatureCache[neededNodesVec[i]] = std::make_tuple(
                 funcRawNumPreds[i], funcRawNumSuccs[i], funcRawTokensLen[i]
             );
        }
        
        // --- 预处理结束 ---


        // --- 【核心优化 步骤 B: 处理单个分支 (使用缓存)】 ---
        for (BranchInst *BI : targetBranches) {
            if (subgraphCache.find(BI) == subgraphCache.end()) {
                continue; // 在步骤A中提取失败了
            }
            
            const SubgraphComponents& subgraph = subgraphCache.at(BI);
            BasicBlock *pgoBlock = subgraph.pgoBlock;

            // 写入日志
            unsigned pgoBlockJsonIndex = BBIndexMap.lookup(pgoBlock);
            if (debugFile) {
                *debugFile << "\n--- Branch Target ---\n";
                *debugFile << "Function: " << F.getName().str() << "\n";
                *debugFile << "BasicBlock Name: " << pgoBlock->getNameOrAsOperand() << "\n";
                *debugFile << "BB Index (JSON): " << pgoBlockJsonIndex << "\n";
                std::string branchStr; raw_string_ostream RSO(branchStr); BI->print(RSO);
                *debugFile << "Instruction: " << branchStr << "\n";
            }

            // 2. 准备GNN输入 (现在从缓存中读取)
            std::vector<int> rawTokensLen;
            std::vector<int> rawNumPreds;
            std::vector<int> rawNumSuccs;
            std::vector<const std::vector<float>*> semanticFeaturesBatch; // 存储特征向量的指针
            bool allNodesFound = true;

            for (BasicBlock* subBB : subgraph.nodesOrdered) {
                 if (funcFeatureCache.find(subBB) == funcFeatureCache.end() ||
                     funcManualFeatureCache.find(subBB) == funcManualFeatureCache.end()) {
                     errs() << "错误：子图节点未在缓存中找到！ (逻辑错误)\n";
                     allNodesFound = false;
                     break;
                 }
                 semanticFeaturesBatch.push_back(funcFeatureCache.lookup(subBB));
                 const auto& manualFeatures = funcManualFeatureCache.lookup(subBB);
                 rawNumPreds.push_back(std::get<0>(manualFeatures));
                 rawNumSuccs.push_back(std::get<1>(manualFeatures));
                 rawTokensLen.push_back(std::get<2>(manualFeatures));
            }
            if (!allNodesFound) { branchesSkipped++; continue; }

            // 写入分词/特征日志
            if (debugFile && subgraph.pgoNodeLocalIndex >= 0) {
                 int pgoFuncBatchIndex = bbToFuncBatchIndex.lookup(pgoBlock);
                 *debugFile << "\n[DEBUG] Tokenizer Output (PGO Block):\n";
                 *debugFile << "  BB IR:\n" << getBasicBlockString(*pgoBlock) << "\n"; // 实时获取 IR
                 *debugFile << "  Input IDs (" << funcBatchInputIds[pgoFuncBatchIndex].size() << "): [";
                 for(size_t k=0; k<funcBatchInputIds[pgoFuncBatchIndex].size(); ++k) { *debugFile << funcBatchInputIds[pgoFuncBatchIndex][k] << (k == funcBatchInputIds[pgoFuncBatchIndex].size()-1 ? "" : ","); }
                 *debugFile << "]\n";
                 
                 *debugFile << "\n[DEBUG] Feature Extractor Output (Semantic Features for PGO Node):\n";
                 const auto& pgoFeatures = *funcFeatureCache.lookup(pgoBlock);
                 *debugFile << "  Features (" << pgoFeatures.size() << "): [";
                 for (size_t k = 0; k < std::min((size_t)10, pgoFeatures.size()); ++k) { *debugFile << std::fixed << std::setprecision(6) << pgoFeatures[k] << (k == std::min((size_t)10, pgoFeatures.size()) - 1 ? "" : ", "); }
                 if (pgoFeatures.size() > 10) *debugFile << ", ...";
                 *debugFile << "]\n";
            }

            // 4. 计算子图局部统计量并组合特征
            double subgraphMeanPreds = calculateMean(rawNumPreds);
            double subgraphStdPreds = calculateStdDev(rawNumPreds, subgraphMeanPreds);
            double subgraphMeanSuccs = calculateMean(rawNumSuccs);
            double subgraphStdSuccs = calculateStdDev(rawNumSuccs, subgraphMeanSuccs);
            double subgraphMeanTokens = calculateMean(rawTokensLen);
            double subgraphStdTokens = calculateStdDev(rawTokensLen, subgraphMeanTokens);

            std::vector<std::vector<float>> combinedFeatures;
            float epsilon = 1e-6;
             for (size_t i = 0; i < subgraph.nodesOrdered.size(); ++i) {
                float normPreds = (subgraphStdPreds > epsilon) ? (static_cast<float>(rawNumPreds[i]) - subgraphMeanPreds) / subgraphStdPreds : 0.0f;
                float normSuccs = (subgraphStdSuccs > epsilon) ? (static_cast<float>(rawNumSuccs[i]) - subgraphMeanSuccs) / subgraphStdSuccs : 0.0f;
                float normTokens = (subgraphStdTokens > epsilon) ? (static_cast<float>(rawTokensLen[i]) - subgraphMeanTokens) / subgraphStdTokens : 0.0f;
                
                std::vector<float> features = {normPreds, normSuccs, normTokens};
                
                const std::vector<float>* semanticFeats = semanticFeaturesBatch[i];
                features.insert(features.end(), semanticFeats->begin(), semanticFeats->end());
                
                combinedFeatures.push_back(features);
            }
            
            // 5. 计算路径掩码
            std::map<int, std::vector<int>> adj;
            for (const auto& edge : subgraph.edgeIndexList) adj[edge.first].push_back(edge.second);

            auto findPathEdges = [&](int startNodeIdx, int k) {
                std::set<std::pair<int, int>> pathEdges;
                std::deque<std::pair<int, int>> q;
                if(startNodeIdx >= 0 && startNodeIdx < static_cast<int>(subgraph.nodesOrdered.size())) q.push_back({startNodeIdx, 0});
                std::set<int> visitedInPath = {subgraph.pgoNodeLocalIndex};
                if(startNodeIdx >= 0) visitedInPath.insert(startNodeIdx);

                while (!q.empty()) {
                    int currNode = q.front().first;
                    int currDist = q.front().second;
                    q.pop_front();
                    if (currDist >= k - 1) continue;
                    if (adj.count(currNode)) {
                         for (int neighbor : adj[currNode]) {
                             pathEdges.insert({currNode, neighbor});
                             if (visitedInPath.find(neighbor) == visitedInPath.end()) {
                                 visitedInPath.insert(neighbor);
                                 q.push_back({neighbor, currDist + 1});
                             }
                         }
                    }
                }
                return pathEdges;
            };

            if (subgraph.leftSuccLocalIndex < 0 || subgraph.rightSuccLocalIndex < 0) { branchesSkipped++; continue; }

            std::set<std::pair<int, int>> leftPathEdges = findPathEdges(subgraph.leftSuccLocalIndex, KHops);
            if(subgraph.pgoNodeLocalIndex >= 0 && subgraph.leftSuccLocalIndex >= 0) leftPathEdges.insert({subgraph.pgoNodeLocalIndex, subgraph.leftSuccLocalIndex});
            std::set<std::pair<int, int>> rightPathEdges = findPathEdges(subgraph.rightSuccLocalIndex, KHops);
            if(subgraph.pgoNodeLocalIndex >= 0 && subgraph.rightSuccLocalIndex >= 0) rightPathEdges.insert({subgraph.pgoNodeLocalIndex, subgraph.rightSuccLocalIndex});

            std::vector<bool> leftMaskVec(subgraph.edgeIndexList.size(), false);
            std::vector<bool> rightMaskVec(subgraph.edgeIndexList.size(), false);
            for (size_t i = 0; i < subgraph.edgeIndexList.size(); ++i) {
                 if (leftPathEdges.count(subgraph.edgeIndexList[i])) leftMaskVec[i] = true;
                 if (rightPathEdges.count(subgraph.edgeIndexList[i])) rightMaskVec[i] = true;
            }

            // 6. 运行GNN预测器
            std::optional<std::vector<float>> predictedLogitsOpt = runGNNPredictor(
                combinedFeatures, subgraph.edgeIndexList, subgraph.pgoNodeLocalIndex, leftMaskVec, rightMaskVec
            );

            // 7. 处理预测结果并写入元数据
            int predictedLabel = 0;
            bool predictionSuccessful = false;

            if (predictedLogitsOpt) {
                const auto& logits = *predictedLogitsOpt;
                predictionSuccessful = true;

                if (debugFile) {
                    *debugFile << "\n[DEBUG] GNN Predictor Output (Logits):\n";
                    if (logits.size() == 3) {
                         *debugFile << "  Logits: [" << std::fixed << std::setprecision(6) << logits[0] << ", "
                                    << std::fixed << std::setprecision(6) << logits[1] << ", "
                                    << std::fixed << std::setprecision(6) << logits[2] << "]\n";
                    } else {
                         *debugFile << "  Error: Unexpected number of logits received (" << logits.size() << ").\n";
                         predictionSuccessful = false;
                    }
                } else if (logits.size() != 3) {
                     errs() << "错误：GNN ONNX 返回的 Logits 数量不为 3！跳过分支。\n";
                     predictionSuccessful = false;
                }


                if (predictionSuccessful) {
                    float maxLogit = logits[0];
                    if (logits[1] > maxLogit) { maxLogit = logits[1]; predictedLabel = 1; }
                    if (logits[2] > maxLogit) { predictedLabel = 2; }
                    if (debugFile) *debugFile << "  Predicted Label: " << predictedLabel << "\n";
                }

            } else {
                errs() << "  - GNN prediction failed for branch in BB '" << pgoBlock->getName() << "'. Skipping.\n";
                if (debugFile) *debugFile << "\n[DEBUG] GNN Prediction Failed (returned nullopt).\n";
            }

            if (predictionSuccessful) {
                std::pair<uint64_t, uint64_t> weights = labelToWeights(predictedLabel);
                if (predictedLabel != 0) {
                    LLVMContext &Ctx = F.getContext();
                    MDBuilder MDB(Ctx);
                    Metadata *Counts[] = {
                        ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(Ctx), weights.first)),
                        ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(Ctx), weights.second))
                    };
                    MDNode *ProfNode = MDNode::get(Ctx, {MDString::get(Ctx, "branch_weights"), Counts[0], Counts[1]});
                    BI->setMetadata(LLVMContext::MD_prof, ProfNode);
                    branchesPredicted++;
                    if (debugFile) *debugFile << "  Metadata Written: [" << weights.first << ", " << weights.second << "]\n";
                    errs() << "  - Predicted label " << predictedLabel << " for branch in BB '"
                           << pgoBlock->getName() << "', wrote weights [" << weights.first << ", " << weights.second << "]\n";
                } else {
                     branchesSkipped++;
                     if (debugFile) *debugFile << "  Metadata Not Written (Label 0).\n";
                     errs() << "  - Predicted label 0 for branch in BB '" << pgoBlock->getName() << "'. No metadata written.\n";
                }
            } else {
                branchesSkipped++;
            }

             if (debugFile) {
                 *debugFile << "-------------------------------------------\n";
             }

        } // End loop over target branches
    } // End loop over functions

    errs() << "ML Predictor Pass: Finished module '" << M.getName() << "'.\n";
    errs() << "  - Predicted: " << branchesPredicted << "\n";
    errs() << "  - Skipped: " << branchesSkipped << "\n";
    
    // 关闭调试日志文件
    if (debugFile) {
        debugFile->close();
    }
    
    return PreservedAnalyses::none();
}