#ifndef _CONSTOBF_INCLUDES_
#define _CONSTOBF_INCLUDES_

#include "llvm/IR/PassManager.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Attributes.h"

#include "Utils.h"


namespace llvm {

class ConstObfuscationPass : public PassInfoMixin<ConstObfuscationPass> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
    bool isSupportedInt(Type *Ty);
    uint64_t maskForBits(unsigned Bits);

};

} // namespace llvm

#endif