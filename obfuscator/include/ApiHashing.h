#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/StringSet.h"

#include "Utils.h"

#include <map>
#include <vector>
#include <string>

namespace llvm {

class ApiHashingPass : public PassInfoMixin<ApiHashingPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

private:
    StringRef getLibraryForFunction(StringRef Name);
    bool shouldSkipDllForThreadPool(std::string ApiName);
    llvm::StringSet<> parseExcludedFuncs();
    bool finishReplaCBngCallBase(CallBase *CB);
};

} // namespace llvm