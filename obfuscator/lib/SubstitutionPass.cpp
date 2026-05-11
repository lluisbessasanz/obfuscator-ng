//===- Substitution.cpp - Substitution Obfuscation
// pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements operators substitution's pass
//
//===----------------------------------------------------------------------===//

#include "SubstitutionPass.h"
#include "Utils.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <random>


#define DEBUG_TYPE "substitution"

static llvm::cl::opt<int> ObfTimes(
    "sub_loop",
    llvm::cl::desc("Choose how many times the substitution pass loops on a function"),
    llvm::cl::value_desc("number of times"),
    llvm::cl::init(1),
    llvm::cl::Optional);

STATISTIC(Add, "Add substituted");
STATISTIC(Sub, "Sub substituted");
STATISTIC(And, "And substituted");
STATISTIC(Or,  "Or substituted");
STATISTIC(Xor, "Xor substituted");

llvm::SubstitutionPass::SubstitutionPass(bool Flag) : Flag(Flag) {
  FuncAdd[0] = &SubstitutionPass::addNeg;
  FuncAdd[1] = &SubstitutionPass::addDoubleNeg;
  FuncAdd[2] = &SubstitutionPass::addRand;
  FuncAdd[3] = &SubstitutionPass::addRand2;

  FuncSub[0] = &SubstitutionPass::subNeg;
  FuncSub[1] = &SubstitutionPass::subRand;
  FuncSub[2] = &SubstitutionPass::subRand2;

  FuncAnd[0] = &SubstitutionPass::andSubstitution;
  FuncAnd[1] = &SubstitutionPass::andSubstitutionRand;

  FuncOr[0] = &SubstitutionPass::orSubstitution;
  FuncOr[1] = &SubstitutionPass::orSubstitutionRand;

  FuncXor[0] = &SubstitutionPass::xorSubstitution;
  FuncXor[1] = &SubstitutionPass::xorSubstitutionRand;
}


unsigned llvm::SubstitutionPass::randomIndex(unsigned UpperBound) {
  if (UpperBound == 0)
    return 0;
  static std::mt19937 Gen{std::random_device{}()};
  std::uniform_int_distribution<unsigned> Dist(0, UpperBound - 1);
  return Dist(Gen);
}

llvm::PreservedAnalyses llvm::SubstitutionPass::run(llvm::Function &F,
                                        llvm::FunctionAnalysisManager &AM) {
  (void)AM;


  if (ObfTimes <= 0) {
    errs() << "Substitution application number -sub_loop=x must be x > 0\n";
    return PreservedAnalyses::all();
  }

  if (!toObfuscate(Flag, F, "sub"))
    return PreservedAnalyses::all();

  bool Changed = substitute(F);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool llvm::SubstitutionPass::substitute(Function &F) {
  bool Changed = false;

  for (int Times = ObfTimes; Times > 0; --Times) {
    std::vector<BinaryOperator *> WorkList;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *BO = dyn_cast<BinaryOperator>(&I);
        if (!BO)
          continue;
        if (!BO->isBinaryOp())
          continue;
        WorkList.push_back(BO);
      }
    }

    for (BinaryOperator *BO : WorkList) {
      // Skip instructions erased by earlier rewrites in the same round.
      if (!BO->getParent())
        continue;

      switch (BO->getOpcode()) {
      case Instruction::Add:
        (this->*FuncAdd[randomIndex(NumberAddSubst)])(BO);
        ++Add;
        Changed = true;
        break;
      case Instruction::Sub:
        (this->*FuncSub[randomIndex(NumberSubSubst)])(BO);
        ++Sub;
        Changed = true;
        break;
      case Instruction::And:
        (this->*FuncAnd[randomIndex(NumberAndSubst)])(BO);
        ++And;
        Changed = true;
        break;
      case Instruction::Or:
        (this->*FuncOr[randomIndex(NumberOrSubst)])(BO);
        ++Or;
        Changed = true;
        break;
      case Instruction::Xor:
        (this->*FuncXor[randomIndex(NumberXorSubst)])(BO);
        ++Xor;
        Changed = true;
        break;
      default:
        break;
      }
    }
  }

  return Changed;
}

// a = b + c  => b - (-c)
void llvm::SubstitutionPass::addNeg(BinaryOperator *BO) {
  auto *Neg = BinaryOperator::CreateNeg(BO->getOperand(1), "", BO);
  auto *Op = BinaryOperator::Create(Instruction::Sub, BO->getOperand(0), Neg, "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = b + c  => -(-b + (-c))
void llvm::SubstitutionPass::addDoubleNeg(BinaryOperator *BO) {
  auto *Op0 = BinaryOperator::CreateNeg(BO->getOperand(0), "", BO);
  auto *Op1 = BinaryOperator::CreateNeg(BO->getOperand(1), "", BO);
  auto *Sum = BinaryOperator::Create(Instruction::Add, Op0, Op1, "", BO);
  auto *Op  = BinaryOperator::CreateNeg(Sum, "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = b + c  => r = rand(); a = b + r; a = a + c; a = a - r
void llvm::SubstitutionPass::addRand(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C   = ConstantInt::get(Ty, randomUint64());
  auto *Op1 = BinaryOperator::Create(Instruction::Add, BO->getOperand(0), C, "", BO);
  auto *Op2 = BinaryOperator::Create(Instruction::Add, Op1, BO->getOperand(1), "", BO);
  auto *Op3 = BinaryOperator::Create(Instruction::Sub, Op2, C, "", BO);

  BO->replaceAllUsesWith(Op3);
  BO->eraseFromParent();
}

// a = b + c  => r = rand(); a = b - r; a = a + c; a = a + r
void llvm::SubstitutionPass::addRand2(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C   = ConstantInt::get(Ty, randomUint64());
  auto *Op1 = BinaryOperator::Create(Instruction::Sub, BO->getOperand(0), C, "", BO);
  auto *Op2 = BinaryOperator::Create(Instruction::Add, Op1, BO->getOperand(1), "", BO);
  auto *Op3 = BinaryOperator::Create(Instruction::Add, Op2, C, "", BO);

  BO->replaceAllUsesWith(Op3);
  BO->eraseFromParent();
}

// a = b - c  => a = b + (-c)
void llvm::SubstitutionPass::subNeg(BinaryOperator *BO) {
  auto *Neg = BinaryOperator::CreateNeg(BO->getOperand(1), "", BO);
  auto *Op  = BinaryOperator::Create(Instruction::Add, BO->getOperand(0), Neg, "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// b - c  => r = rand(); a = b + r; a = a - c; a = a - r
void llvm::SubstitutionPass::subRand(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C   = ConstantInt::get(Ty, randomUint64());
  auto *Op1 = BinaryOperator::Create(Instruction::Add, BO->getOperand(0), C, "", BO);
  auto *Op2 = BinaryOperator::Create(Instruction::Sub, Op1, BO->getOperand(1), "", BO);
  auto *Op3 = BinaryOperator::Create(Instruction::Sub, Op2, C, "", BO);

  BO->replaceAllUsesWith(Op3);
  BO->eraseFromParent();
}

// b - c => r = rand(); a = b - r; a = a - c; a = a + r
void llvm::SubstitutionPass::subRand2(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C   = ConstantInt::get(Ty, randomUint64());
  auto *Op1 = BinaryOperator::Create(Instruction::Sub, BO->getOperand(0), C, "", BO);
  auto *Op2 = BinaryOperator::Create(Instruction::Sub, Op1, BO->getOperand(1), "", BO);
  auto *Op3 = BinaryOperator::Create(Instruction::Add, Op2, C, "", BO);

  BO->replaceAllUsesWith(Op3);
  BO->eraseFromParent();
}

// a = b & c => (b ^ ~c) & b
void llvm::SubstitutionPass::andSubstitution(BinaryOperator *BO) {
  auto *NotC = BinaryOperator::CreateNot(BO->getOperand(1), "", BO);
  auto *X    = BinaryOperator::Create(Instruction::Xor, BO->getOperand(0), NotC, "", BO);
  auto *Op   = BinaryOperator::Create(Instruction::And, X, BO->getOperand(0), "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = a & b <=> !(!a | !b) && (r | !r)
void llvm::SubstitutionPass::andSubstitutionRand(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C    = ConstantInt::get(Ty, randomUint64());
  auto *NotA = BinaryOperator::CreateNot(BO->getOperand(0), "", BO);
  auto *NotB = BinaryOperator::CreateNot(BO->getOperand(1), "", BO);
  auto *NotR = BinaryOperator::CreateNot(C, "", BO);

  auto *Or1    = BinaryOperator::Create(Instruction::Or, NotA, NotB, "", BO);
  auto *Or2    = BinaryOperator::Create(Instruction::Or, C, NotR, "", BO);
  auto *NotOr1 = BinaryOperator::CreateNot(Or1, "", BO);
  auto *Op     = BinaryOperator::Create(Instruction::And, NotOr1, Or2, "", BO);

  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = b | c => a = (b & c) | (b ^ c)
void llvm::SubstitutionPass::orSubstitution(BinaryOperator *BO) {
  auto *A  = BinaryOperator::Create(Instruction::And, BO->getOperand(0), BO->getOperand(1), "", BO);
  auto *B  = BinaryOperator::Create(Instruction::Xor, BO->getOperand(0), BO->getOperand(1), "", BO);
  auto *Op = BinaryOperator::Create(Instruction::Or, A, B, "", BO);

  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// c = a | b => r = rand();  (((!a & r) | (a & !r)) ^ ((!b & r) | (b & !r))) | (!(!a | !b) & (r | !r)) 
void llvm::SubstitutionPass::orSubstitutionRand(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C    = ConstantInt::get(Ty, randomUint64());
  auto *NotA = BinaryOperator::CreateNot(BO->getOperand(0), "", BO);
  auto *NotB = BinaryOperator::CreateNot(BO->getOperand(1), "", BO);
  auto *NotR = BinaryOperator::CreateNot(C, "", BO);

  auto *P1 = BinaryOperator::Create(Instruction::And, NotA, C, "", BO);
  auto *P2 = BinaryOperator::Create(Instruction::And, BO->getOperand(0), NotR, "", BO);
  auto *P3 = BinaryOperator::Create(Instruction::And, NotB, C, "", BO);
  auto *P4 = BinaryOperator::Create(Instruction::And, BO->getOperand(1), NotR, "", BO);

  auto *L = BinaryOperator::Create(Instruction::Or, P1, P2, "", BO);
  auto *R = BinaryOperator::Create(Instruction::Or, P3, P4, "", BO);
  auto *X = BinaryOperator::Create(Instruction::Xor, L, R, "", BO);

  auto *NAorNB   = BinaryOperator::Create(Instruction::Or, NotA, NotB, "", BO);
  auto *NotNAorNB = BinaryOperator::CreateNot(NAorNB, "", BO);
  auto *RorNotR   = BinaryOperator::Create(Instruction::Or, C, NotR, "", BO);
  auto *A2        = BinaryOperator::Create(Instruction::And, NotNAorNB, RorNotR, "", BO);

  auto *Op = BinaryOperator::Create(Instruction::Or, X, A2, "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = a ^ b => (!a & b) || (a & !b)
void llvm::SubstitutionPass::xorSubstitution(BinaryOperator *BO) {
  auto *NotA = BinaryOperator::CreateNot(BO->getOperand(0), "", BO);
  auto *L    = BinaryOperator::Create(Instruction::And, BO->getOperand(1), NotA, "", BO);

  auto *NotB = BinaryOperator::CreateNot(BO->getOperand(1), "", BO);
  auto *R    = BinaryOperator::Create(Instruction::And, BO->getOperand(0), NotB, "", BO);

  auto *Op = BinaryOperator::Create(Instruction::Or, L, R, "", BO);
  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}

// a = a ^ b => (a ^ r) ^ (b ^ r)
void llvm::SubstitutionPass::xorSubstitutionRand(BinaryOperator *BO) {
  auto *Ty = dyn_cast<IntegerType>(BO->getType());
  if (!Ty)
    return;

  auto *C    = ConstantInt::get(Ty, randomUint64());
  auto *NotA = BinaryOperator::CreateNot(BO->getOperand(0), "", BO);
  auto *L1   = BinaryOperator::Create(Instruction::And, C, NotA, "", BO);

  auto *NotR = BinaryOperator::CreateNot(C, "", BO);
  auto *L2   = BinaryOperator::Create(Instruction::And, BO->getOperand(0), NotR, "", BO);

  auto *NotB = BinaryOperator::CreateNot(BO->getOperand(1), "", BO);
  auto *R1   = BinaryOperator::Create(Instruction::And, NotB, C, "", BO);
  auto *R2   = BinaryOperator::Create(Instruction::And, BO->getOperand(1), NotR, "", BO);

  auto *L  = BinaryOperator::Create(Instruction::Or, L1, L2, "", BO);
  auto *R  = BinaryOperator::Create(Instruction::Or, R1, R2, "", BO);
  auto *Op = BinaryOperator::Create(Instruction::Xor, L, R, "", BO);

  BO->replaceAllUsesWith(Op);
  BO->eraseFromParent();
}
