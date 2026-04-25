#include "Flattening.h"

#define DEBUG_TYPE "flattening"

// Stats
STATISTIC(Flattened, "Functions flattened");

llvm::PreservedAnalyses llvm::FlatteningPass::run(llvm::Function &F, llvm::FunctionAnalysisManager &AM) {
  // Do we obfuscate
  if (!toObfuscate(flag, F, "fla"))
    return PreservedAnalyses::all();

  bool Changed = flatten(F, AM);
  if (Changed)
    ++Flattened;

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool llvm::FlatteningPass::flatten(llvm::Function &F, llvm::FunctionAnalysisManager &AM) {
  std::vector<llvm::BasicBlock *> origBB;
  llvm::BasicBlock *loopEntry;
  llvm::BasicBlock *loopEnd;
  LoadInst *load;
  SwitchInst *switchI;
  AllocaInst *switchVar;

  // SCRAMBLER
  char scrambling_key[16];
  getRandomBytes(scrambling_key, 16);
  // END OF SCRAMBLER

  // Lower switch
  LowerSwitchPass Lower;
  Lower.run(F, AM);

  // Save all original BB
  for (Function::iterator itBB = F.begin(); itBB != F.end(); ++itBB) {
    origBB.push_back(&*itBB);
    if (isa<InvokeInst>(&*itBB->getTerminator())) {
      return false;
    }
  }

  // Nothing to flatten
  if (origBB.size() <= 1) {
    return false;
  }

  // Remove first BB
  origBB.erase(origBB.begin());

  // Get a pointer on the first BB
  Function::iterator itBB = F.begin();  //++tmp;
  BasicBlock *insert = &*itBB;

  // If main begin with an if
  BranchInst *br = NULL;
  if (isa<BranchInst>(insert->getTerminator())) {
    br = cast<BranchInst>(insert->getTerminator());
  }

  if ((br != NULL && br->isConditional()) || insert->getTerminator()->getNumSuccessors() > 1) {
    BasicBlock::iterator i = insert->end();
    --i;

    if (insert->size() > 1) {
      --i;
    }

    /*
    splitBasicBlock (iterator I, const Twine &BBName="")
   	Split the basic block into two basic blocks at the specified instruction. 
    */
    BasicBlock *tmpBB = insert->splitBasicBlock(i, "first");
    origBB.insert(origBB.begin(), tmpBB);
  }

  // Remove jump
  insert->getTerminator()->eraseFromParent();

  // Create switch variable and set as it
  llvm::IRBuilder<> Builder(insert);

  /*
  CreateAllocationSize (Type *DestTy, AllocaInst *AI)
 	Get allocation size of an alloca as a runtime Value* (handles both static and dynamic allocas and vscale factor). 
  */
  switchVar = Builder.CreateAlloca(
      Type::getInt32Ty(F.getContext()),
      nullptr,
      "switchVar");  

  uint32_t Scrambled = scramble32(0, scrambling_key);

  auto *InitVal = ConstantInt::get(
    Type::getInt32Ty(F.getContext()),
    Scrambled);

  Builder.CreateStore(InitVal, switchVar);
  // Create main loop
  loopEntry = BasicBlock::Create(F.getContext(), "loopEntry", &F, insert);
  loopEnd = BasicBlock::Create(F.getContext(), "loopEnd", &F, insert);

  IRBuilder<> LoopBuilder(loopEntry);
  load = LoopBuilder.CreateLoad(
      Type::getInt32Ty(F.getContext()),
      switchVar,
      "switchVar");

      // Move first BB on top
  insert->moveBefore(loopEntry);
  BranchInst::Create(loopEntry, insert);

  // loopEnd jump to loopEntry
  BranchInst::Create(loopEntry, loopEnd);

  BasicBlock *swDefault = BasicBlock::Create(F.getContext(), "switchDefault", &F, loopEnd);
  BranchInst::Create(loopEnd, swDefault);

  // Create switch instruction itself and set condition
  switchI = SwitchInst::Create(load, swDefault, 0, loopEntry);

  // Remove branch jump from 1st BB and make a jump to the while
  insert->getTerminator()->eraseFromParent();
  BranchInst::Create(loopEntry, insert);

  // Put all BB in the switch
  for (std::vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
       ++b) {
    BasicBlock *i = *b;
    ConstantInt *numCase = NULL;

    // Move the BB inside the switch (only visual, no code logic)
    i->moveBefore(loopEnd);

    // Add case to switch
    numCase = cast<ConstantInt>(ConstantInt::get(
        switchI->getCondition()->getType(),
        scramble32(switchI->getNumCases(), scrambling_key)));
    switchI->addCase(numCase, i);
  }

  // Recalculate switchVar
  for (std::vector<BasicBlock *>::iterator b = origBB.begin(); b != origBB.end();
       ++b) {
    BasicBlock *i = *b;
    ConstantInt *numCase = NULL;

    // Ret BB
    if (i->getTerminator()->getNumSuccessors() == 0) {
      continue;
    }

    // If it's a non-conditional jump
    if (i->getTerminator()->getNumSuccessors() == 1) {
      // Get successor and delete terminator
      BasicBlock *succ = i->getTerminator()->getSuccessor(0);
      i->getTerminator()->eraseFromParent();

      // Get next case
      numCase = switchI->findCaseDest(succ);

      // If next case == default case (switchDefault)
      if (numCase == NULL) {
        numCase = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble32(
                                 switchI->getNumCases() - 1, scrambling_key)));
      }

      // Update switchVar and jump to the end of loop
      IRBuilder<> B(i);
      B.CreateStore(numCase, switchVar);
      BranchInst::Create(loopEnd, i);
      continue;
    }

    // If it's a conditional jump
    if (i->getTerminator()->getNumSuccessors() == 2) {
      // Get next cases
      ConstantInt *numCaseTrue =
          switchI->findCaseDest(i->getTerminator()->getSuccessor(0));
      ConstantInt *numCaseFalse =
          switchI->findCaseDest(i->getTerminator()->getSuccessor(1));

      // Check if next case == default case (switchDefault)
      if (numCaseTrue == NULL) {
        numCaseTrue = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble32(
                                 switchI->getNumCases() - 1, scrambling_key)));
      }

      if (numCaseFalse == NULL) {
        numCaseFalse = cast<ConstantInt>(
            ConstantInt::get(switchI->getCondition()->getType(),
                             scramble32(
                                 switchI->getNumCases() - 1, scrambling_key)));
      }

      // Create a SelectInst
      BranchInst *br = cast<BranchInst>(i->getTerminator());
      SelectInst *sel =
          SelectInst::Create(br->getCondition(), numCaseTrue, numCaseFalse, "",
                             i->getTerminator());

      // Erase terminator
      i->getTerminator()->eraseFromParent();

      // Update switchVar and jump to the end of loop
      IRBuilder<> B(i);
      B.CreateStore(sel, switchVar);
      BranchInst::Create(loopEnd, i);
      continue;
    }
  }

  fixStack(F);

  return true;
}