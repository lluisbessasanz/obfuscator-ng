
#ifndef _SPLIT_INCLUDES_
#define _SPLIT_INCLUDES_

#include "Utils.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

namespace llvm {

class SplitBBPass : public PassInfoMixin<SplitBBPass> {
public:
  explicit SplitBBPass(bool Flag = true) : flag(Flag) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
private:
  bool flag = true;
  bool split(Function &F);
  bool containsPHI(BasicBlock &BB);
  void shuffle(std::vector<int> &vec);
};

} // namespace llvm


#endif

