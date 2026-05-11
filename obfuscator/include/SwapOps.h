#ifndef _SWAPOPS_H_
#define _SWAPOPS_H_

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "Utils.h"

class SwapOpsPass : public llvm::PassInfoMixin<SwapOpsPass> {
public:
  explicit SwapOpsPass(bool Flag = true) : Flag(Flag) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);

private:
  bool Flag = true;
};

#endif