#include "SwapOps.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <random>

using namespace llvm;

#define DEBUG_TYPE "swapops"

STATISTIC(SwapCounter, "Number of operands swapped");

static cl::opt<unsigned> SwapSeed(
    "swap_seed",
    cl::desc("Seed for swapops pseudo-random decisions"),
    cl::value_desc("seed"),
    cl::init(0));

static cl::opt<unsigned> SwapProb(
    "swap_prob",
    cl::desc("Swap probability percentage, 0..100"),
    cl::value_desc("percent"),
    cl::init(50));

PreservedAnalyses SwapOpsPass::run(Function &F,
                                   FunctionAnalysisManager &) {

  if (!Flag)
    return PreservedAnalyses::all();

  if (F.isDeclaration())
    return PreservedAnalyses::all();

  if (SwapProb > 100) {
    errs() << "[swapops] -swap_prob must be between 0 and 100\n";
    return PreservedAnalyses::all();
  }

  bool Changed = false;

  // Deterministic-ish seed per function.
  std::seed_seq SeedSeq({
      SwapSeed.getValue(),
      crc32(F.getName().data(), F.getName().size()),
      static_cast<unsigned>(F.size())
  });

  std::mt19937 RNG(SeedSeq);
  std::uniform_int_distribution<unsigned> Dist(1, 100);

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!I.isCommutative())
        continue;

      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO)
        continue;

      if (Dist(RNG) <= SwapProb) {
        BO->swapOperands();
        Changed = true;
        ++SwapCounter;
      }
    }
  }

  return Changed ? PreservedAnalyses::none()
                 : PreservedAnalyses::all();
}