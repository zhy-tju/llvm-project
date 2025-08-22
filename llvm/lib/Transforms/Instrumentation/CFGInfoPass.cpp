#include "llvm/Transforms/Instrumentation/CFGInfoPass.h"

// 包含所有必要的头文件
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <algorithm> // for std::replace

using namespace llvm;

// --- Command-Line Options ---
namespace {

// 主开关：只有当这个标志为true时，Pass才会执行任何操作
static cl::opt<bool> EnableCFGInfoJsonOutput(
    "enable-cfginfo-json", cl::init(false), cl::Hidden,
    cl::desc("Enable output of CFG info as JSON"));

// 过滤选项
static cl::opt<bool> CfgFilterByLoop(
    "cfg-filter-by-loop", cl::init(false), cl::Hidden,
    cl::desc("Filter functions based on the presence of loops instead of branches."));

static cl::opt<bool> EnableCFGInfoUserOnly(
    "enable-cfginfo-useronly", cl::init(false), cl::Hidden,
    cl::desc("Only output CFG for user-defined functions"));

} // end anonymous namespace


// --- 辅助函数 (现在是文件内的静态函数，不再是全局) ---
namespace {

std::string escapeForJson(const std::string &input) {
    std::ostringstream escaped;
    for (char c : input) {
        switch (c) {
        case '"':  escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b";  break;
        case '\f': escaped << "\\f";  break;
        case '\n': escaped << "\\n";  break;
        case '\r': escaped << "\\r";  break;
        case '\t': escaped << "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                escaped << "\\u" << std::setw(4) << std::setfill('0') << std::hex
                        << static_cast<int>(static_cast<unsigned char>(c));
            } else {
                escaped << c;
            }
        }
    }
    return escaped.str();
}

std::vector<uint64_t> getBranchProfileCounts(Instruction &I) {
    std::vector<uint64_t> weights;
    if (MDNode *MD = I.getMetadata(LLVMContext::MD_prof)) {
        if (MD->getNumOperands() > 1) {
            for (unsigned i = 1; i < MD->getNumOperands(); ++i) {
                if (auto *CI = mdconst::dyn_extract<ConstantInt>(MD->getOperand(i))) {
                    weights.push_back(CI->getZExtValue());
                }
            }
        }
    }
    return weights;
}

std::string getSafeInstructionString(Instruction &I) {
    std::string InstStr;
    raw_string_ostream RSO(InstStr);
    I.print(RSO, false);
    StringRef Str = RSO.str();
    size_t Start = Str.find_first_not_of(" \t\n\r");
    if (Start == StringRef::npos) return "";
    size_t End = Str.find_last_not_of(" \t\n\r");
    return escapeForJson(Str.substr(Start, End - Start + 1).str());
}

std::vector<std::string> getRawInstructions(BasicBlock &BB) {
    std::vector<std::string> Instructions;
    Instructions.reserve(BB.size());
    for (Instruction &I : BB) {
        if (I.isDebugOrPseudoInst()) continue;
        Instructions.push_back(getSafeInstructionString(I));
    }
    return Instructions;
}

std::vector<std::string> getCalledFunctionNames(BasicBlock &BB) {
    std::vector<std::string> calledFunctions;
    for (Instruction &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (Function *F = CB->getCalledFunction()) {
                calledFunctions.push_back(escapeForJson(F->getName().str()));
            } else if (CB->getCalledOperand()) {
                std::string CalledName;
                raw_string_ostream RSO(CalledName);
                CB->getCalledOperand()->printAsOperand(RSO, false);
                calledFunctions.push_back(escapeForJson(RSO.str()));
            }
        }
    }
    return calledFunctions;
}

std::string generateFunctionJson(Function &F, FunctionAnalysisManager &FAM) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    
    DenseMap<const BasicBlock*, unsigned> BBIndexMap;
    unsigned NextIndex = 0;
    for (const BasicBlock &BB : F) {
        BBIndexMap[&BB] = NextIndex++;
    }

    std::ostringstream OS;
    OS << "  {\n";
    OS << "    \"name\": \"" << escapeForJson(F.getName().str()) << "\",\n";

    std::string demangledName = demangle(F.getName());
    if (demangledName != F.getName().str()) {
        OS << "    \"demangled_name\": \"" << escapeForJson(demangledName) << "\",\n";
    } else {
        OS << "    \"demangled_name\": null,\n";
    }

    OS << "    \"nodes\": [\n";
    
    bool FirstNode = true;
    for (BasicBlock &BB_ref : F) {
        // We need a non-const reference for some operations
        BasicBlock& BB = const_cast<BasicBlock&>(BB_ref);

        OS << (FirstNode ? "" : ",\n") << "      {\n";
        OS << "        \"index\": " << BBIndexMap[&BB] << ",\n";
        
        auto Instructions = getRawInstructions(BB);
        OS << "        \"instructions\": [\n";
        for (size_t i = 0; i < Instructions.size(); ++i) {
            OS << "          \"" << Instructions[i] << "\""
               << (i == Instructions.size() - 1 ? "" : ",") << "\n";
        }
        OS << "        ],\n";

        auto CalledFunctions = getCalledFunctionNames(BB);
        OS << "        \"called_functions\": [";
        for (size_t i = 0; i < CalledFunctions.size(); ++i) {
            OS << (i > 0 ? ", " : "") << "\"" << CalledFunctions[i] << "\"";
        }
        OS << "],\n";

        bool hasBranchProfile = false;
        std::string branchType;
        std::vector<uint64_t> branchProfileCounts;
        if (auto *TI = BB.getTerminator()) {
             if (isa<BranchInst>(TI) || isa<SwitchInst>(TI)) {
                branchProfileCounts = getBranchProfileCounts(*TI);
                if (!branchProfileCounts.empty()) {
                    hasBranchProfile = true;
                    branchType = isa<BranchInst>(TI) ? "branch" : "switch";
                }
             }
        }
        OS << "        \"branch_profile_counts\": ";
        if (hasBranchProfile) {
            OS << "{ \"type\": \"" << branchType << "\", \"weights\": [";
            for (size_t i = 0; i < branchProfileCounts.size(); ++i) {
                OS << (i > 0 ? ", " : "") << branchProfileCounts[i];
            }
            OS << "] }";
        } else {
            OS << "null";
        }
        OS << ",\n";
        
        DomTreeNode *DomNode = DT.getNode(&BB);
        OS << "        \"dominator\": ";
        if (DomNode && DomNode->getIDom()) {
            OS << BBIndexMap.lookup(DomNode->getIDom()->getBlock());
        } else {
            OS << "null";
        }
        OS << ",\n";

        OS << "        \"predecessors\": [";
        bool FirstPred = true;
        for (auto *Pred : predecessors(&BB)) {
            if (!FirstPred) OS << ", ";
            OS << BBIndexMap.lookup(Pred);
            FirstPred = false;
        }
        OS << "],\n";

        OS << "        \"successors\": [";
        bool FirstSucc = true;
        for (auto *Succ : successors(&BB)) {
            if (!FirstSucc) OS << ", ";
            OS << BBIndexMap.lookup(Succ);
            FirstSucc = false;
        }
        OS << "],\n";

        OS << "        \"dominated_blocks\": [";
        if (DomNode) {
            bool FirstDom = true;
            for (DomTreeNode *Child : DomNode->children()) {
                if (!FirstDom) OS << ", ";
                OS << BBIndexMap.lookup(Child->getBlock());
                FirstDom = false;
            }
        }
        OS << "]\n";

        OS << "      }";
        FirstNode = false;
    }

    OS << "\n    ]\n";
    OS << "  }";
    return OS.str();
}

} // end anonymous namespace

// --- Pass Implementation ---

PreservedAnalyses CFGInfoPass::run(Module &M, ModuleAnalysisManager &AM) {
    // 【核心】首先检查主开关。如果未启用，则不执行任何操作。
    if (!EnableCFGInfoJsonOutput) {
        return PreservedAnalyses::all();
    }

    // 获取FunctionAnalysisManager，以便后续为每个函数获取分析结果
    auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    
    std::vector<std::string> functionJsonStrings;

    // 遍历模块中的每一个函数
    for (Function &F : M) {
        if (F.isDeclaration() || F.empty()) {
            continue;
        }

        // --- 过滤逻辑 ---
        bool shouldProcess = false;
        if (CfgFilterByLoop) {
            auto &LI = FAM.getResult<LoopAnalysis>(F);
            if (!LI.empty()) {
                shouldProcess = true;
            }
        } else { // 默认按分支过滤
            for (BasicBlock &BB : F) {
                auto *TI = BB.getTerminator();
                if (TI && (isa<BranchInst>(TI) || isa<SwitchInst>(TI))) {
                    shouldProcess = true;
                    break;
                }
            }
        }
        
        if (!shouldProcess) continue;

        if (EnableCFGInfoUserOnly) {
            std::string demangledName = demangle(F.getName());
            if (demangledName != F.getName().str() && StringRef(demangledName).starts_with("std::")) {
                continue;
            }
        }
        
        // 生成该函数的JSON
        functionJsonStrings.push_back(generateFunctionJson(F, FAM));
    }

    // --- 在处理完一个模块的所有函数后，立即写入文件 ---
    if (!functionJsonStrings.empty()) {
        std::string fullPath = M.getSourceFileName();
        if (fullPath.empty()) {
            fullPath = M.getModuleIdentifier();
        }
        
        // --- 【核心修正】使用完整的相对路径来创建唯一的文件名 ---
        std::string safeFilename = fullPath;
        
        // 替换所有可能导致问题的字符
        std::replace(safeFilename.begin(), safeFilename.end(), '/', '_');
        std::replace(safeFilename.begin(), safeFilename.end(), '\\', '_');
        std::replace(safeFilename.begin(), safeFilename.end(), '.', '_');
        
        // 移除可能的前导 `..` 产生的下划线
        while (safeFilename.size() >= 2 && safeFilename.substr(0, 2) == "__") {
            safeFilename = safeFilename.substr(1);
        }
        
        std::string OutputFilename = "cfg" + safeFilename + ".json";
        
        std::error_code EC;
        // 默认输出到当前工作目录
        raw_fd_ostream OutFile(OutputFilename, EC, sys::fs::OF_Text);
        
        if (EC) {
            errs() << "Error opening output file " << OutputFilename << ": " << EC.message() << "\n";
        } else {
            OutFile << "[\n";
            for (size_t i = 0; i < functionJsonStrings.size(); ++i) {
                OutFile << functionJsonStrings[i];
                if (i < functionJsonStrings.size() - 1) {
                    OutFile << ",\n";
                }
            }
            OutFile << "\n]\n";
            OutFile.flush();
            // (可选) 打印日志，确认文件已生成
            errs() << "CFG Info for module " << M.getSourceFileName() << " written to " << OutputFilename << "\n";
        }
    }
    
    // Pass没有修改IR，所以返回all preserved
    return PreservedAnalyses::all();
}