//===- SubstitutionIncludes.h - Substitution Obfuscation pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the substitution pass
//
//===----------------------------------------------------------------------===//

#ifndef _SUBSTITUTIONS_H_
#define _SUBSTITUTIONS_H_

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"

namespace llvm {

class SubstitutionPass : public PassInfoMixin<SubstitutionPass> {
public:
  explicit SubstitutionPass(bool Flag = true);

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  static constexpr unsigned NumberAddSubst = 4;
  static constexpr unsigned NumberSubSubst = 3;
  static constexpr unsigned NumberAndSubst = 2;
  static constexpr unsigned NumberOrSubst  = 2;
  static constexpr unsigned NumberXorSubst = 2;

  void (SubstitutionPass::*FuncAdd[NumberAddSubst])(BinaryOperator *BO);
  void (SubstitutionPass::*FuncSub[NumberSubSubst])(BinaryOperator *BO);
  void (SubstitutionPass::*FuncAnd[NumberAndSubst])(BinaryOperator *BO);
  void (SubstitutionPass::*FuncOr[NumberOrSubst])(BinaryOperator *BO);
  void (SubstitutionPass::*FuncXor[NumberXorSubst])(BinaryOperator *BO);

  bool Flag = true;

  bool substitute(Function &F);

  static unsigned randomIndex(unsigned UpperBound);

  void addNeg(BinaryOperator *BO);
  void addDoubleNeg(BinaryOperator *BO);
  void addRand(BinaryOperator *BO);
  void addRand2(BinaryOperator *BO);

  void subNeg(BinaryOperator *BO);
  void subRand(BinaryOperator *BO);
  void subRand2(BinaryOperator *BO);

  void andSubstitution(BinaryOperator *BO);
  void andSubstitutionRand(BinaryOperator *BO);

  void orSubstitution(BinaryOperator *BO);
  void orSubstitutionRand(BinaryOperator *BO);

  void xorSubstitution(BinaryOperator *BO);
  void xorSubstitutionRand(BinaryOperator *BO);
};

} // namespace llvm

#endif

