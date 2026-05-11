#ifndef _FLATTENING_INCLUDES_
#define _FLATTENING_INCLUDES_

#include "Utils.h"

#include <array>
#include <vector>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/IR/NoFolder.h"


namespace llvm {

class FlatteningPass : public PassInfoMixin<FlatteningPass> {
public:
  explicit FlatteningPass(bool Flag = true) : flag(Flag) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool flag = true;
  bool flatten(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif