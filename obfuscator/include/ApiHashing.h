
#ifndef _HASHING_INCLUDES_
#define _HASHING_INCLUDES_

#include "Utils.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class ApiHashingPass : public PassInfoMixin<ApiHashingPass> {
public:
  explicit ApiHashingPass(bool Flag = true) : flag(Flag) {}

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
private:
  bool flag = true;
  StringRef getLibraryForFunction(StringRef Name);
};

} // namespace llvm


#endif

