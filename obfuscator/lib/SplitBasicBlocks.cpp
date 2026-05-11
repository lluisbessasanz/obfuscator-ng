//===- SplitBasicBlock.cpp - SplitBasicBlokc Obfuscation pass--------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the split basic block pass
//
//===----------------------------------------------------------------------===//

#include "SplitBasicBlock.h"

#define DEBUG_TYPE "split"

// Stats
STATISTIC(Split, "Basicblock splitted");

static llvm::cl::opt<int> SplitNum(
    "split_num",
    llvm::cl::desc("Split <split_num> time each BB"),
    llvm::cl::value_desc("number of times"),
    llvm::cl::init(3),
    llvm::cl::Optional);

llvm::PreservedAnalyses llvm::SplitBBPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (hasUnsupportedIR(F)){
    return PreservedAnalyses::all();
  }

  // Check if the number of applications is correct
  if (!((SplitNum > 1) && (SplitNum <= 10))) {
    errs()<<"Split application basic block percentage\
            -split_num=x must be 1 < x <= 10";
    return PreservedAnalyses::all();
  }

  // Do we obfuscate
  if (!toObfuscate(flag, F, "splitbb")) {
    return PreservedAnalyses::all();
  }

  bool Changed = split(F);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool llvm::SplitBBPass::split(Function &F) {
  std::vector<BasicBlock *> origBB;
  int splitN = SplitNum;

  // Save all basic blocks
  for (Function::iterator I = F.begin(), IE = F.end(); I != IE; ++I) {
    origBB.push_back(&*I);
  }

  bool Changed = false;

  for (std::vector<BasicBlock *>::iterator I = origBB.begin(),
                                           IE = origBB.end();
       I != IE; ++I) {
    BasicBlock &curr = **I;

    // No need to split a 1 inst bb
    // Or ones containing a PHI node
    if (curr.size() < 2 || containsPHI(curr)) {
      continue;
    }

    // Check splitN and current BB size
    if ((size_t)splitN > curr.size()) {
      splitN = curr.size() - 1;
    }

    // Generate splits point
    std::vector<int> test;
    for (unsigned i = 1; i < curr.size(); ++i) {
      test.push_back(i);
    }

    // Shuffle
    if (test.size() != 1) {
      shuffle(test);
      std::sort(test.begin(), test.begin() + splitN);
    }

    // Split
    BasicBlock::iterator it = curr.begin();
    BasicBlock *toSplit = &curr;
    int last = 0;
    for (int i = 0; i < splitN; ++i) {
      for (int j = 0; j < test[i] - last; ++j) {
        ++it;
      }
      last = test[i];
      if(toSplit->size() < 2)
        continue;
      toSplit = toSplit->splitBasicBlock(it, toSplit->getName() + ".split");
      Changed = true;
    }

    ++Split;
  }
  return Changed;
}

bool llvm::SplitBBPass::containsPHI(BasicBlock &BB) {
  for (BasicBlock::iterator I = BB.begin(), IE = BB.end(); I != IE; ++I) {
    if (isa<PHINode>(I)) {
      return true;
    }
  }
  return false;
}

void llvm::SplitBBPass::shuffle(std::vector<int> &vec) {
  int n = vec.size();
  for (int i = n - 1; i > 0; --i) {
    std::swap(vec[i], vec[randomUint64() % (i + 1)]);
  }
}

