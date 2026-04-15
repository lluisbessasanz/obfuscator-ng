#ifndef __UTILS_OBF__
#define __UTILS_OBF__

#include "llvm/IR/Function.h"
#include <string>

namespace llvm {

std::string readAnnotate(Function &F);
bool toObfuscate(bool Flag, Function &F, const std::string &Attribute);

} 

#endif
