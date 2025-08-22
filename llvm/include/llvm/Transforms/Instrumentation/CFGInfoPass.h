// CFGInfoPass.h (最终修正版)

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H

#include "llvm/IR/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include <mutex>
#include <string>
#include <vector>

// 使用前向声明，而不是包含完整的头文件，让头文件更轻量
namespace llvm {
  class Function;
  class BasicBlock;
  
}

namespace llvm {
  class CFGInfoPass : public PassInfoMixin<CFGInfoPass> {
  private:
    std::string Filename = "cfg_graph.json";
    DenseMap<BasicBlock*, unsigned> BBIndexMap;
    unsigned NextIndex = 0;
    
    static std::mutex FileMutex;
    static std::vector<std::string> FunctionJsonBuffer;

    std::vector<std::string> getRawInstructions(BasicBlock &BB);
    std::string generateFunctionJson(Function &F, FunctionAnalysisManager &AM);

  public:
    bool EnableJsonOutput;
    bool EnableCFGInfoUserOnly;
    bool FilterByLoop;
    
    CFGInfoPass();
    ~CFGInfoPass();

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  };
} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_CFGINFOPASS_H