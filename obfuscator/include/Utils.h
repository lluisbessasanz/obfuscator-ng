#ifndef __UTILS_OBF__
#define __UTILS_OBF__

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"
#include <string>
#include <random>


#define LOAD32H(x, y)                                                          \
  {                                                                            \
    (x) = ((uint32_t)((y)[0] & 0xFF) << 24) |                                  \
          ((uint32_t)((y)[1] & 0xFF) << 16) |                                  \
          ((uint32_t)((y)[2] & 0xFF) << 8) | ((uint32_t)((y)[3] & 0xFF) << 0); \
  }

namespace llvm {

std::string readAnnotate(Function &F);
bool toObfuscate(bool Flag, Function &F, const std::string &Attribute);
unsigned scramble32(const unsigned in, const char key[16]);
void fixStack(Function &F);
static bool valueEscapes(llvm::Instruction &Inst);

uint64_t randomUint64();
uint32_t crc32(const void *data, size_t length);
bool hasUnsupportedIR(Function &F);
uint64_t randBits(unsigned Bits);
Constant *rewriteConstant(Constant *C, Type *Ty);


} 

#endif
