#include "ConstObfuscation.h"

using namespace llvm;

static bool looksLikePackedAscii(uint64_t V, unsigned Bytes) {
    unsigned Printable = 0;

    for (unsigned i = 0; i < Bytes; ++i) {
        unsigned char C = (V >> (8 * i)) & 0xff;

        if (C == 0)
            continue;

        if (C >= 0x20 && C <= 0x7e)
            Printable++;
        else
            return false;
    }

    return Printable >= 3;
}

PreservedAnalyses ConstObfuscationPass::run(Function &F, FunctionAnalysisManager &AM) {
    bool Changed = false;

    SmallVector<std::pair<CallBase *, unsigned>, 64> Worklist;

    errs() << "[constobf] visiting function: " << F.getName() << "\n";

    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            auto *CB = dyn_cast<CallBase>(&I);
            if (!CB)
                continue;

            if (CB->isInlineAsm())
                continue;

            for (unsigned ArgNo = 0; ArgNo < CB->arg_size(); ++ArgNo) {
                auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(ArgNo));
                if (!CI)
                    continue;

                Type *Ty = CI->getType();
                if (!isSupportedInt(Ty))
                    continue;

                unsigned Bits = Ty->getIntegerBitWidth();
                unsigned Bytes = Bits / 8;
                uint64_t Original = CI->getZExtValue();

                //if (!looksLikePackedAscii(Original, Bytes))
                //    continue;
                
                Worklist.push_back({CB, ArgNo});
            }
        }
    }

    for (auto &[CB, ArgNo] : Worklist) {
        auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(ArgNo));
        if (!CI)
            continue;

        if (CB->paramHasAttr(ArgNo, Attribute::ImmArg))
            continue;

        Type *Ty = CI->getType();
        unsigned Bits = Ty->getIntegerBitWidth();

        uint64_t Original = CI->getZExtValue();
        uint64_t Mask = maskForBits(Bits);

        uint64_t Key = llvm::randBits(Bits) & Mask;
        if (Key == 0)
            Key = 0xA5 & Mask;

        uint64_t Enc = (Original ^ Key) & Mask;

        IRBuilder<NoFolder> B(CB);

        Value *EncVal = ConstantInt::get(Ty, Enc);
        Value *KeyVal = ConstantInt::get(Ty, Key);
        Value *Dec = B.CreateXor(EncVal, KeyVal, "const.dec");

        CB->setArgOperand(ArgNo, Dec);

        Changed = true;

    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}


bool llvm::ConstObfuscationPass::isSupportedInt(Type *Ty) {
    return Ty->isIntegerTy(8)  ||
           Ty->isIntegerTy(16) ||
           Ty->isIntegerTy(32) ||
           Ty->isIntegerTy(64);
}

uint64_t llvm::ConstObfuscationPass::maskForBits(unsigned Bits) {
    if (Bits >= 64)
        return ~0ULL;
    return (1ULL << Bits) - 1ULL;
}