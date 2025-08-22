#include "llvm/Transforms/Instrumentation/CFGInfoPass.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h" // This header defines TerminatorInst
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>

using namespace llvm;

// --- Command-Line Options ---
namespace {
static cl::opt<bool> EnableCFGInfoJsonOutput(
    "enable-cfginfo-json", cl::init(false), cl::Hidden,
    cl::desc("Enable output of CFG info as JSON"));

static cl::opt<bool> EnableCFGInfoUserOnly(
    "enable-cfginfo-useronly", cl::init(false), cl::Hidden,
    cl::desc("Only output CFG for user-defined functions"));

// 【核心新增】定义新的命令行选项
static cl::opt<bool> CfgFilterByLoop(
    "cfg-filter-by-loop", cl::init(false), cl::Hidden,
    cl::desc("Filter functions based on the presence of loops instead of branches."));
} // namespace

// --- Static Member Initialization ---
std::mutex CFGInfoPass::FileMutex;
std::vector<std::string> CFGInfoPass::FunctionJsonBuffer;

// --- Helper Function Definitions ---

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
    MDNode *MD = I.getMetadata(LLVMContext::MD_prof);
    if (MD && MD->getNumOperands() > 1) {
        for (unsigned i = 1; i < MD->getNumOperands(); ++i) {
            if (auto *CI = mdconst::dyn_extract<ConstantInt>(MD->getOperand(i))) {
                weights.push_back(CI->getZExtValue());
            }
        }
    }
    return weights;
}

std::string getSafeInstructionString(Instruction &I) {
    std::string InstStr;
    raw_string_ostream RSO(InstStr);
    I.print(RSO, /*IsForDebug=*/false);

    StringRef Str = RSO.str();
    size_t Start = Str.find_first_not_of(" \t\n\r");
    if (Start == StringRef::npos) return "";
    size_t End = Str.find_last_not_of(" \t\n\r");
    return escapeForJson(Str.substr(Start, End - Start + 1).str());
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

std::vector<std::string> CFGInfoPass::getRawInstructions(BasicBlock &BB) {
    std::vector<std::string> Instructions;
    Instructions.reserve(BB.size());
    for (Instruction &I : BB) {
        if (I.isDebugOrPseudoInst()) continue;
        Instructions.push_back(getSafeInstructionString(I));
    }
    return Instructions;
}


// --- Pass Implementation ---

CFGInfoPass::CFGInfoPass() {
    EnableJsonOutput = EnableCFGInfoJsonOutput;
    EnableCFGInfoUserOnly = EnableCFGInfoUserOnly;
    // 【核心新增】在构造函数中读取命令行参数的值
    FilterByLoop = CfgFilterByLoop;
}

CFGInfoPass::~CFGInfoPass() {
    if (!EnableJsonOutput || FunctionJsonBuffer.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(FileMutex);
    std::error_code EC;
    raw_fd_ostream OutFile(Filename, EC, sys::fs::OF_Text);
    if (EC) {
        errs() << "Error opening output file for final write: " << EC.message() << "\n";
        return;
    }

    OutFile << "[\n";
    for (size_t i = 0; i < FunctionJsonBuffer.size(); ++i) {
        OutFile << FunctionJsonBuffer[i];
        if (i < FunctionJsonBuffer.size() - 1) {
            OutFile << ",\n";
        }
    }
    OutFile << "\n]\n";
    OutFile.flush();
}

// Pass主入口点，包含了新的过滤逻辑
PreservedAnalyses CFGInfoPass::run(Function &F, FunctionAnalysisManager &AM) {
    if (!EnableJsonOutput) {
        return PreservedAnalyses::all();
    }
    
    // 跳过没有函数体的函数声明
    if (F.isDeclaration() || F.empty()) {
        return PreservedAnalyses::all();
    }
    
    // 【核心修改】根据命令行选项，执行不同的过滤逻辑
    bool shouldProcess = false;
    if (FilterByLoop) {
        // --- 逻辑二：按循环过滤 ---
        auto &LI = AM.getResult<LoopAnalysis>(F);
        if (!LI.empty()) {
            shouldProcess = true;
        }
    } else {
        // --- 逻辑一（默认）：按分支过滤 ---
        for (BasicBlock &BB : F) {
            auto *TI = BB.getTerminator();
            // 只要找到一个分支或Switch指令，就满足条件
            if (TI && (isa<BranchInst>(TI) || isa<SwitchInst>(TI))) {
                shouldProcess = true;
                break; // 找到一个即可，无需继续遍历
            }
        }
    }
    
    // 如果函数不满足所选的过滤条件，则直接返回
    if (!shouldProcess) {
        return PreservedAnalyses::all();
    }

    // (可选的) 用户函数过滤逻辑
    if (EnableCFGInfoUserOnly) {
        std::string demangledName = demangle(F.getName());
        if (demangledName != F.getName() && StringRef(demangledName).starts_with("std::")) {
            return PreservedAnalyses::all();
        }
    }

    // 满足条件，生成JSON并存入缓冲区
    std::string functionJson = generateFunctionJson(F, AM);
    std::lock_guard<std::mutex> lock(FileMutex);
    FunctionJsonBuffer.push_back(std::move(functionJson));

    return PreservedAnalyses::all();
}

// 为单个函数生成JSON字符串 (此函数逻辑不变)
std::string CFGInfoPass::generateFunctionJson(Function &F, FunctionAnalysisManager &AM) {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    
    BBIndexMap.clear();
    NextIndex = 0;
    for (BasicBlock &BB : F) {
        BBIndexMap[&BB] = NextIndex++;
    }

    std::ostringstream OS;
    OS << "  {\n";
    OS << "    \"name\": \"" << escapeForJson(F.getName().str()) << "\",\n";

    std::string demangledName = demangle(F.getName());
    if (demangledName != F.getName()) {
        OS << "    \"demangled_name\": \"" << escapeForJson(demangledName) << "\",\n";
    } else {
        OS << "    \"demangled_name\": null,\n";
    }

    OS << "    \"nodes\": [\n";
    
    bool FirstNode = true;
    for (BasicBlock &BB : F) {
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