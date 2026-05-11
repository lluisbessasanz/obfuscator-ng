#include "SubstitutionPass.h"
#include "Flattening.h"
#include "SplitBasicBlock.h"
#include "ApiHashing.h"
#include "ArrayObfuscation.h"
#include "SwapOps.h"
#include "ConstObfuscation.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION,
      "obfuscator-ng",
      "0.1",
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, FunctionPassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "sub") {
                FPM.addPass(SubstitutionPass());
                return true;
              } else if (Name == "fla") {
                FPM.addPass(FlatteningPass());
                return true;
              } else if (Name == "splitbb"){
                FPM.addPass(SplitBBPass());
                return true;
              } else if (Name == "swapops") {
                FPM.addPass(SwapOpsPass());
                return true;
              } else if (Name == "constobf") {
                FPM.addPass(ConstObfuscationPass());
                return true;
              }
              return false;
        });
        PB.registerPipelineParsingCallback(
            [](StringRef Name,
               ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "arrenc") {
                MPM.addPass(ArrayObfuscationPass());
                return true;
              } else if (Name == "api_hashing") {
                MPM.addPass(ApiHashingPass());
                return true;
              } 

              return false;
        });
      }};
}
