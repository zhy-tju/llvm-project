#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_MANUALFEATURESCFGPASS_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_MANUALFEATURESCFGPASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

// 我们将这个新Pass命名为 ManualFeaturesCFGPass
class ManualFeaturesCFGPass : public PassInfoMixin<ManualFeaturesCFGPass> {
public:
    // Pass的入口点是 Module
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    // 这是新版Pass管理器所必需的静态函数
    static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_MANUALFEATURESCFGPASS_H