#include "Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

std::string llvm::readAnnotate(Function &F) {
  std::string Annotation;

  Module *M = F.getParent();
  if (!M)
    return Annotation;

  GlobalVariable *Glob = M->getGlobalVariable("llvm.global.annotations");
  if (!Glob || !Glob->hasInitializer())
    return Annotation;

  auto *CA = dyn_cast<ConstantArray>(Glob->getInitializer());
  if (!CA)
    return Annotation;

  for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
    auto *StructAn = dyn_cast<ConstantStruct>(CA->getOperand(I));
    if (!StructAn || StructAn->getNumOperands() < 2)
      continue;

    auto *Expr = dyn_cast<ConstantExpr>(StructAn->getOperand(0));
    if (!Expr)
      continue;

    if (Expr->getOpcode() != Instruction::BitCast)
      continue;

    if (Expr->getOperand(0) != &F)
      continue;

    auto *Note = dyn_cast<ConstantExpr>(StructAn->getOperand(1));
    if (!Note || Note->getOpcode() != Instruction::GetElementPtr)
      continue;

    auto *AnnoteStr = dyn_cast<GlobalVariable>(Note->getOperand(0));
    if (!AnnoteStr || !AnnoteStr->hasInitializer())
      continue;

    auto *Data =
        dyn_cast<ConstantDataSequential>(AnnoteStr->getInitializer());
    if (!Data || !Data->isString())
      continue;

    Annotation += Data->getAsString().lower();
    Annotation += " ";
  }

  return Annotation;
}

bool llvm::toObfuscate(bool Flag, Function &F, const std::string &Attribute) {
  if (F.isDeclaration())
    return false;

  if (F.hasAvailableExternallyLinkage())
    return false;

  std::string Attr = Attribute;
  std::string NoAttr = "no" + Attr;
  std::string Annotation = readAnnotate(F);

  if (Annotation.find(NoAttr) != std::string::npos)
    return false;

  if (Annotation.find(Attr) != std::string::npos)
    return true;

  return Flag;
}
