#include "llvm/Transforms/Instrumentation/ManualFeaturesCFGPass.h"

// 包含所有必要的头文件
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
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
#include <algorithm>
#include <vector>

using namespace llvm;

// --- 命令行选项 ---
namespace {
// 主开关：只有当这个标志为true时，Pass才会执行任何操作
static cl::opt<bool> EnableManualFeaturesJsonOutput(
    "enable-manual-features-json", cl::init(false), cl::Hidden,
    cl::desc("Enable output of CFG with handcrafted features as JSON"));
} // end anonymous namespace


// --- 辅助数据结构与函数 ---
namespace {

// 一个丰富的特征结构体，完美融合了您的所有需求
struct ManualFeatures {
    // === 基本块自身特征 ===
    unsigned num_instr = 0;
    unsigned num_phis = 0;
    unsigned num_calls = 0;
    unsigned num_loads = 0;
    unsigned num_stores = 0;
    unsigned num_preds = 0;
    unsigned num_succ = 0;
    bool ends_with_unreachable = false;
    bool ends_with_return = false;
    bool ends_with_cond_branch = false;
    bool ends_with_branch = false;

    // === 语义特征 ===
    unsigned num_alloca = 0;
    unsigned num_malloc = 0;
    unsigned num_free = 0;
    unsigned num_memcpy = 0;
    unsigned num_io = 0;
    unsigned num_fp = 0;
    unsigned num_int = 0;
    unsigned num_logic = 0;
    unsigned num_control = 0;
    unsigned num_mem = 0;
    unsigned num_other = 0;
    unsigned num_add = 0, num_sub = 0, num_mul = 0, num_div = 0, num_rem = 0;
    unsigned num_and = 0, num_or = 0, num_xor = 0;
    unsigned num_shl = 0, num_lshr = 0, num_ashr = 0;
    unsigned num_icmp = 0, num_fcmp = 0;
    unsigned num_select = 0, num_cast = 0, num_gep = 0, num_call_indirect = 0;

    // === 分支指令特定特征 (仅当块以条件分支结尾时有效) ===
    // 浮点比较
    bool is_fcmp = false;
    bool is_fcmp_eq = false;
    bool is_fcmp_ne = false;
    bool is_fcmp_nan = false;
    bool is_fcmp_not_nan = false;
    // 整数比较
    bool is_icmp = false;
    bool is_icmp_cnst = false;
    bool is_icmp_cnst_one = false;
    bool is_icmp_lt_one = false;
    bool is_icmp_cnst_zero = false;
    bool is_icmp_eq_zero = false;
    bool is_icmp_ne_zero = false;
    bool is_icmp_gt_zero = false;
    bool is_icmp_lt_zero = false;
    bool is_icmp_cnst_minus_one = false;
    bool is_icmp_eq_minus_one = false;
    bool is_icmp_ne_minus_one = false;
    bool is_icmp_gt_minus_one = false;
    bool is_icmp_lib = false;
    bool is_icmp_eq_lib = false;
    bool is_icmp_ne_lib = false;
    // 指针比较
    bool is_pointer_compare = false;
    bool is_pointer_compare_eq = false;
    bool is_pointer_compare_ne = false;
    
    // === 控制流与循环特征 ===
    unsigned control_split_depth = 0;
    unsigned dominator_depth = 0;
    // bool is_block_in_loop = false; // 已去除，等价于 loop_depth > 0
    unsigned loop_depth = 0;
    bool is_loop_header = false;
    bool is_loop_exiting = false;
    // 【核心修改】以下特征已被移除
    // bool loop_back_edge = false;
    // bool loop_entering_edge = false;
    // bool loop_exiting_edge = false;


    // 清空所有特征
    void clear() { *this = ManualFeatures(); }

    // 将所有特征转换为一个整数向量
    std::vector<int> toVector() const {
        return {
            // 基本块特征
            (int)num_instr, (int)num_phis, (int)num_calls, (int)num_loads, (int)num_stores,
            (int)num_preds, (int)num_succ, (int)ends_with_unreachable, (int)ends_with_return,
            (int)ends_with_cond_branch, (int)ends_with_branch,
            // 语义特征
            (int)num_alloca, (int)num_malloc, (int)num_free, (int)num_memcpy, (int)num_io,
            (int)num_fp, (int)num_int, (int)num_logic, (int)num_control, (int)num_mem, (int)num_other,
            (int)num_add, (int)num_sub, (int)num_mul, (int)num_div, (int)num_rem, (int)num_and,
            (int)num_or, (int)num_xor, (int)num_shl, (int)num_lshr, (int)num_ashr, (int)num_icmp,
            (int)num_fcmp, (int)num_select, (int)num_cast, (int)num_gep, (int)num_call_indirect,
            // 分支比较特征
            (int)is_fcmp, (int)is_fcmp_eq, (int)is_fcmp_ne, (int)is_fcmp_nan, (int)is_fcmp_not_nan,
            (int)is_icmp, (int)is_icmp_cnst, (int)is_icmp_cnst_one, (int)is_icmp_lt_one,
            (int)is_icmp_cnst_zero, (int)is_icmp_eq_zero, (int)is_icmp_ne_zero, (int)is_icmp_gt_zero, (int)is_icmp_lt_zero,
            (int)is_icmp_cnst_minus_one, (int)is_icmp_eq_minus_one, (int)is_icmp_ne_minus_one, (int)is_icmp_gt_minus_one,
            (int)is_icmp_lib, (int)is_icmp_eq_lib, (int)is_icmp_ne_lib,
            (int)is_pointer_compare, (int)is_pointer_compare_eq, (int)is_pointer_compare_ne,
            // 控制流与循环特征
            (int)control_split_depth, (int)dominator_depth,
            (int)loop_depth, (int)is_loop_header, (int)is_loop_exiting
        };
    }
};

// JSON转义函数
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

// 提取PGO权重
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

// 核心的特征提取函数
void fillManualFeatures(BasicBlock &BB, ManualFeatures &Features, Function &F,
                        LoopInfo &LI, DominatorTree &DT, PostDominatorTree &PDT) {
    
    Features.num_preds = std::distance(pred_begin(&BB), pred_end(&BB));
    Features.num_succ = BB.getTerminator()->getNumSuccessors();

    Instruction *T = BB.getTerminator();
    if (isa<UnreachableInst>(T)) Features.ends_with_unreachable = true;
    if (isa<ReturnInst>(T)) Features.ends_with_return = true;
    if (auto *BI = dyn_cast<BranchInst>(T)) {
        Features.ends_with_branch = BI->isUnconditional();
        Features.ends_with_cond_branch = BI->isConditional();
    }

    // 统计指令类型分布和语义特征
    for (Instruction &inst : BB) {
        Features.num_instr++;
        if (auto *CI = dyn_cast<CallInst>(&inst)) {
            Features.num_calls++;
            if (Function *CF = CI->getCalledFunction()) {
                StringRef name = CF->getName();
                if (name == "malloc") Features.num_malloc++;
                if (name == "free") Features.num_free++;
                if (name == "memcpy" || name == "memmove") Features.num_memcpy++;
                if (name == "printf" || name == "puts" || name == "scanf" || name == "gets") Features.num_io++;
            } else {
                Features.num_call_indirect++;
            }
        }
        if (isa<PHINode>(inst)) Features.num_phis++;
        if (isa<AllocaInst>(inst)) Features.num_alloca++;
        if (isa<LoadInst>(inst)) { Features.num_loads++; Features.num_mem++; }
        if (isa<StoreInst>(inst)) { Features.num_stores++; Features.num_mem++; }
        if (isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst)) Features.num_mem++;
        if (inst.isTerminator()) Features.num_control++;
        if (inst.getType()->isFloatingPointTy()) Features.num_fp++;
        if (isa<BinaryOperator>(inst)) {
            if(inst.getType()->isIntegerTy()) Features.num_int++;
            switch (inst.getOpcode()) {
                case Instruction::Add: case Instruction::FAdd: Features.num_add++; break;
                case Instruction::Sub: case Instruction::FSub: Features.num_sub++; break;
                case Instruction::Mul: case Instruction::FMul: Features.num_mul++; break;
                case Instruction::UDiv: case Instruction::SDiv: case Instruction::FDiv: Features.num_div++; break;
                case Instruction::URem: case Instruction::SRem: case Instruction::FRem: Features.num_rem++; break;
                case Instruction::And: Features.num_and++; Features.num_logic++; break;
                case Instruction::Or:  Features.num_or++;  Features.num_logic++; break;
                case Instruction::Xor: Features.num_xor++; Features.num_logic++; break;
                case Instruction::Shl:  Features.num_shl++; break;
                case Instruction::LShr: Features.num_lshr++; break;
                case Instruction::AShr: Features.num_ashr++; break;
                default: Features.num_other++; break;
            }
    } else if (isa<ICmpInst>(inst)) { Features.num_icmp++; }
    else if (isa<FCmpInst>(inst)) { Features.num_fcmp++; }
        else if (isa<SelectInst>(inst)) Features.num_select++;
        else if (isa<CastInst>(inst)) Features.num_cast++;
        else if (isa<GetElementPtrInst>(inst)) Features.num_gep++;
        else if (!isa<CallInst>(inst) && !isa<PHINode>(inst) && !isa<AllocaInst>(inst) && !isa<LoadInst>(inst) && !isa<StoreInst>(inst) && !inst.isTerminator()) {
            Features.num_other++;
        }
    }

    // 分支指令特定特征
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
        if (BI->isConditional()) {
            Value *Condition = BI->getCondition();
            if (auto *Cmp = dyn_cast<CmpInst>(Condition)) {
                if (Cmp->getOperand(0)->getType()->isPointerTy()) {
                    Features.is_pointer_compare = true;
                    if (Cmp->getPredicate() == CmpInst::ICMP_EQ) Features.is_pointer_compare_eq = true;
                    if (Cmp->getPredicate() == CmpInst::ICMP_NE) Features.is_pointer_compare_ne = true;
                }
                else if (auto *FCI = dyn_cast<FCmpInst>(Cmp)) {
                    Features.is_fcmp = true;
                    if (FCI->getPredicate() == FCmpInst::FCMP_OEQ || FCI->getPredicate() == FCmpInst::FCMP_UEQ) Features.is_fcmp_eq = true;
                    if (FCI->getPredicate() == FCmpInst::FCMP_ONE || FCI->getPredicate() == FCmpInst::FCMP_UNE) Features.is_fcmp_ne = true;
                    if (FCI->getPredicate() == FCmpInst::FCMP_ORD) Features.is_fcmp_not_nan = true;
                    if (FCI->getPredicate() == FCmpInst::FCMP_UNO) Features.is_fcmp_nan = true;
                }
                else if (auto *ICI = dyn_cast<ICmpInst>(Cmp)) {
                    Features.is_icmp = true;
                    if (isa<Constant>(ICI->getOperand(1))) {
                        Features.is_icmp_cnst = true;
                        if (auto *CI = dyn_cast<ConstantInt>(ICI->getOperand(1))) {
                            if (CI->isZero()) {
                                Features.is_icmp_cnst_zero = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_EQ) Features.is_icmp_eq_zero = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_NE) Features.is_icmp_ne_zero = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_SGT) Features.is_icmp_gt_zero = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_SLT) Features.is_icmp_lt_zero = true;
                            } else if (CI->isOne()) {
                                Features.is_icmp_cnst_one = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_SLT) Features.is_icmp_lt_one = true;
                            } else if (CI->isMinusOne()) {
                                Features.is_icmp_cnst_minus_one = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_EQ) Features.is_icmp_eq_minus_one = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_NE) Features.is_icmp_ne_minus_one = true;
                                if (ICI->getPredicate() == ICmpInst::ICMP_SGT) Features.is_icmp_gt_minus_one = true;
                            }
                        }
                    }
                    // 检查ICmpInst的两个操作数是否为lib函数调用
                    for (int opIdx = 0; opIdx < 2; ++opIdx) {
                        if (auto *Call = dyn_cast<CallInst>(ICI->getOperand(opIdx))) {
                            if (Function *F = Call->getCalledFunction()) {
                                StringRef Name = F->getName();
                                if (Name == "strcmp" || Name == "memcmp" || Name == "strncmp" || Name == "strcasecmp" || Name == "strncasecmp") {
                                    Features.is_icmp_lib = true;
                                    if(ICI->isEquality()) Features.is_icmp_eq_lib = true;
                                    else Features.is_icmp_ne_lib = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // 控制流与循环特征
    DomTreeNode *DomNode = DT.getNode(&BB);
    unsigned depth = 0;
    while (DomNode && DomNode->getIDom()) {
        depth++;
        DomNode = DomNode->getIDom();
    }
    Features.control_split_depth = depth;
    Features.dominator_depth = depth;
    
    Features.loop_depth = LI.getLoopDepth(&BB);
    if (LI.isLoopHeader(&BB)) Features.is_loop_header = true;
    // is_loop_exiting: 该块是否为其所在循环的exiting block
    if (auto *L = LI.getLoopFor(&BB)) {
        Features.is_loop_exiting = L->isLoopExiting(&BB);
    } else {
        Features.is_loop_exiting = false;
    }
}


// 为单个函数生成JSON字符串
std::string generateFunctionJson(Function &F, FunctionAnalysisManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
    
    DenseMap<const BasicBlock*, unsigned> BBIndexMap;
    unsigned NextIndex = 0;
    for (const BasicBlock &BB : F) {
        BBIndexMap[&BB] = NextIndex++;
    }

    std::ostringstream OS;
    OS << "  {\n";
    OS << "    \"name\": \"" << escapeForJson(F.getName().str()) << "\",\n";
    OS << "    \"nodes\": [\n";
    
    bool FirstNode = true;
    for (BasicBlock &BB : F) {
        OS << (FirstNode ? "" : ",\n") << "      {\n";
        OS << "        \"index\": " << BBIndexMap.lookup(&BB) << ",\n";
        
        ManualFeatures Features;
        fillManualFeatures(BB, Features, F, LI, DT, PDT);
        std::vector<int> FeatureVector = Features.toVector();

        OS << "        \"features\": [";
        for (size_t i = 0; i < FeatureVector.size(); ++i) {
            OS << FeatureVector[i] << (i == FeatureVector.size() - 1 ? "" : ", ");
        }
        OS << "],\n";

        // PGO数据和CFG结构信息的提取逻辑
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
        OS << "]\n"; // 最后一个字段后没有逗号

        OS << "      }";
        FirstNode = false;
    }

    OS << "\n    ]\n";
    OS << "  }";
    return OS.str();
}

} // end anonymous namespace

// --- Pass Implementation ---

PreservedAnalyses ManualFeaturesCFGPass::run(Module &M, ModuleAnalysisManager &AM) {
    if (!EnableManualFeaturesJsonOutput) {
        return PreservedAnalyses::all();
    }

    auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    std::vector<std::string> functionJsonStrings;

    // 遍历模块中的每一个函数
    for (Function &F : M) {
        if (F.isDeclaration() || F.empty()) continue;
        
        // --- 【核心过滤逻辑在这里】 ---
        // 1. 初始化一个标志位为 false
        bool hasCondBranch = false;
        
        // 2. 遍历函数中的所有基本块
        for (BasicBlock &BB : F) {
                auto *TI = BB.getTerminator();
                if (TI && (isa<BranchInst>(TI) || isa<SwitchInst>(TI))) {
                    hasCondBranch = true;
                    break; 
                }
            }
        
        // 3. 检查标志位。如果整个函数都没有找到任何条件分支，
        //    hasCondBranch 将保持为 false，这个 continue 语句会被执行，
        //    从而跳过当前函数，不为它生成任何JSON。
        if (!hasCondBranch) continue;
        
        // 只有当函数包含至少一个条件分支时，才会执行到这里，为其生成JSON。
        functionJsonStrings.push_back(generateFunctionJson(F, FAM));
    }

    if (!functionJsonStrings.empty()) {
        std::string fullPath = M.getSourceFileName();
        if (fullPath.empty()) fullPath = M.getModuleIdentifier();
        
        std::string safeFilename = fullPath;
        std::replace(safeFilename.begin(), safeFilename.end(), '/', '_');
        std::replace(safeFilename.begin(), safeFilename.end(), '\\', '_');
        std::replace(safeFilename.begin(), safeFilename.end(), '.', '_');
        // 移除可能的前导 `..` 产生的下划线
        while (safeFilename.size() >= 2 && safeFilename.substr(0, 2) == "__") {
            safeFilename = safeFilename.substr(1);
        }
        std::string OutputFilename = "features" + safeFilename + ".json";
        
        std::error_code EC;
        raw_fd_ostream OutFile(OutputFilename, EC, sys::fs::OF_Text);
        
        if (!EC) {
            OutFile << "[\n";
            for (size_t i = 0; i < functionJsonStrings.size(); ++i) {
                OutFile << functionJsonStrings[i] << (i < functionJsonStrings.size() - 1 ? ",\n" : "");
            }
            OutFile << "\n]\n";
            OutFile.flush();
        } else {
            errs() << "Error opening output file " << OutputFilename << ": " << EC.message() << "\n";
        }
    }
    
    return PreservedAnalyses::all();
}