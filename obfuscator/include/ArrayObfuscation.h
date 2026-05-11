
#ifndef _STRENC_H_
#define _STRENC_H_

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/NoFolder.h"

#include "Utils.h"

#include <cstdint>


namespace llvm{

struct decodeInfo {
    llvm::GlobalVariable *GV = nullptr;
    llvm::Type *Ty = nullptr;
    llvm::SmallVector<llvm::Constant *, 8> Indices;
    uint32_t Bits = 0;
    uint64_t PathHash = 0;
};

class ArrayObfuscationPass : public PassInfoMixin<ArrayObfuscationPass> {
public:
  explicit ArrayObfuscationPass(bool Flag = true) : flag(Flag) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

private:
  void collectUsingInstructions(GlobalVariable *GV, SmallVectorImpl<Instruction *> &Uses);
  void insertDecodeEncodeAroundUses(Module &M, ArrayRef<GlobalVariable *> Arrays,
                                    uint32_t Key);  bool flag = true;
  void debugPointerArray(GlobalVariable &GV);
  bool traverseGlobals(LLVMContext &Ctx, ArrayRef<GlobalVariable *> Arrays, std::vector<GlobalVariable *>  &Out, llvm::DenseSet<GlobalVariable *> &Visited, const uint32_t Key);
  void collectGlobalRefsFromConstant(Constant *C,SmallVectorImpl<GlobalVariable *> &Refs);
  void makeDecodedRuntimeGlobal(GlobalVariable *GV);
  bool isSupportedIntegerType(Type *Ty);
  Constant *encodeConstantArrayRecursive(LLVMContext &Ctx, Constant *C, uint32_t Key, bool &Changed, uint64_t PathHash);
  void emitDecodeForAggregateRecursive(llvm::GlobalVariable *GV, llvm::Type *CurrentTy, llvm::SmallVectorImpl<llvm::Constant *> &Indices, uint32_t Key, const llvm::DataLayout &DL, uint64_t PathHash, std::vector<decodeInfo> &Out);
  uint64_t buildPerByteMask(Type *Ty, uint8_t Seed, uint64_t ElementIndex);
  uint8_t deriveKeyByte(uint8_t Seed, uint64_t PathHash, unsigned ByteIndex); 
};

} // namespace

#endif
