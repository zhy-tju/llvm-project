#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

// Pass现在是一个ModulePass，它会对每个源文件（模块）运行一次
class CFGInfoPass : public PassInfoMixin<CFGInfoPass> {
public:
    // Pass的入口点现在是 run(Module&, ModuleAnalysisManager&)
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    // 我们可以通过一个静态布尔值来确保Pass只被CMakeLists中的一个地方添加
    // 这在新的Pass管理器中是必需的
    static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H