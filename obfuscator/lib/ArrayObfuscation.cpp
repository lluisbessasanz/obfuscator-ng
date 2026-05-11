#include "ArrayObfuscation.h"

static llvm::cl::opt<unsigned> arrenc_seed(
    "arrenc_seed",
    llvm::cl::desc("Encription Key for constants"),
    llvm::cl::value_desc("-arrenc_seed num"),
    llvm::cl::init(0));





void llvm::ArrayObfuscationPass::debugPointerArray(GlobalVariable &GV) {
    if (!GV.hasInitializer())
        return;

    auto *ArrTy = dyn_cast<ArrayType>(GV.getValueType());
    if (!ArrTy)
        return;

    Type *ElemTy = ArrTy->getElementType();

    if (!ElemTy->isPointerTy()){
        return;
    }

    auto *CA = dyn_cast<ConstantArray>(GV.getInitializer());
    if (!CA)
        return;

    for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
        Constant *Op = CA->getOperand(I)->stripPointerCasts();

        auto *TargetGV = dyn_cast<GlobalVariable>(Op);
        if (!TargetGV) {
            continue;
        }


        if (!TargetGV->hasInitializer())
            continue;

        auto *TargetCDA =
            dyn_cast<ConstantDataArray>(TargetGV->getInitializer());

        if (!TargetCDA)
            continue;

        Type *TargetElemTy = TargetCDA->getElementType();

    }
}
llvm::PreservedAnalyses llvm::ArrayObfuscationPass::run(
    Module &M,
    ModuleAnalysisManager &AM
) {

    LLVMContext &Ctx = M.getContext();

    std::vector<GlobalVariable *> Worklist;
    std::vector<GlobalVariable *> Out;
    llvm::DenseSet<GlobalVariable *> Visited;

    for (GlobalVariable &GV : M.globals()) {
        debugPointerArray(GV);

        if (!GV.hasInitializer())
            continue;

        Constant *Init = GV.getInitializer();

        /*
         * Case 1:
         * Directly encodable integer arrays:
         *
         *   @x = global [N x i8]  ...
         *   @x = global [N x i16] ...
         *   @x = global [N x i32] ...
         *   @x = global [N x i64] ...
         */
        if (auto *CDA = dyn_cast<ConstantDataArray>(Init)) {
            auto *ArrTy = dyn_cast<ArrayType>(GV.getValueType());
            if (!ArrTy)
                continue;

            Type *ElemTy = CDA->getElementType();

            bool IsIntegerArray =
                ElemTy->isIntegerTy(8)  ||
                ElemTy->isIntegerTy(16) ||
                ElemTy->isIntegerTy(32) ||
                ElemTy->isIntegerTy(64);

            if (!IsIntegerArray)
                continue;

            Worklist.push_back(&GV);
            continue;
        }

        /*
         * Case 2:
         * Aggregate constants that may contain pointers to string globals:
         *
         *   @ptrs = global [N x ptr] [...]
         *   @cmds = constant [N x %struct.CMD] [...]
         *   @cfg  = constant %struct.T { ptr @str, ... }
         *
         * These are not directly encoded themselves. They are scanned
         * recursively in traverseGlobals().
         */
        if (isa<ConstantAggregate>(Init) ||
            isa<ConstantArray>(Init)     ||
            isa<ConstantStruct>(Init)    ||
            isa<ConstantVector>(Init)    ||
            isa<ConstantExpr>(Init)) {
            Worklist.push_back(&GV);
            continue;
        }
    }

    if (Worklist.empty()) {
        errs() << "[arrenc] no globals selected for traversal\n";
        return PreservedAnalyses::all();
    }

    const uint32_t Key = static_cast<uint32_t>(arrenc_seed);

    bool Changed = traverseGlobals(
        Ctx,
        Worklist,
        Out,
        Visited,
        Key
    );

    if (Out.empty()) {
        errs() << "[arrenc] no concrete integer arrays encoded; decode-on-use skipped\n";
        return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }

    insertDecodeEncodeAroundUses(M, Out, Key);

    return PreservedAnalyses::none();
}

/*
 * Transitively collect every Instruction that directly or indirectly uses GV
 * (through GEP / bitcast / ConstantExpr chains). Each such instruction is a
 * potential use site that needs decode-before / encode-after wrapping.
 */
void llvm::ArrayObfuscationPass::collectUsingInstructions(
    GlobalVariable *GV,
    SmallVectorImpl<Instruction *> &Uses
) {
    SmallVector<Value *, 16> Worklist;
    SmallPtrSet<Value *, 16> Seen;

    Worklist.push_back(GV);

    while (!Worklist.empty()) {
        Value *V = Worklist.pop_back_val();
        if (!Seen.insert(V).second)
            continue;

        for (User *U : V->users()) {
            if (auto *I = dyn_cast<Instruction>(U)) {
                /*
                 * Concrete instruction — this is an actual use site.
                 */
                Uses.push_back(I);
            } else if (isa<ConstantExpr>(U) || isa<GEPOperator>(U)) {
                /*
                 * Constant expression (e.g. GEP in a global initialiser or
                 * address-of-element used as a call argument): keep walking.
                 */
                Worklist.push_back(U);
            }
        }
    }
}

/*
 * For each encoded GlobalVariable, insert a decode call immediately before
 * every instruction that uses it, and a matching re-encode call immediately
 * after it.  Because the XOR cipher is self-inverse, decode == encode; both
 * use the same __deobf_data runtime call.
 *
 * Layout around a use site:
 *
 *   call void @__deobf_data(ptr %desc, i32 %count)   ; ← decode
 *   <original instruction that reads / writes the GV>
 *   call void @__deobf_data(ptr %desc, i32 %count)   ; ← re-encode
 *
 * The descriptor table is a per-GV private constant so each global gets its
 * own descriptor rather than one monolithic table.  There is no guard flag:
 * the array is always encoded at rest and is only briefly exposed.
 */
void llvm::ArrayObfuscationPass::insertDecodeEncodeAroundUses(
    Module &M,
    ArrayRef<GlobalVariable *> Arrays,
    uint32_t Key
) {
    LLVMContext &Ctx = M.getContext();
    const DataLayout &DL = M.getDataLayout();

    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *I32Ty  = Type::getInt32Ty(Ctx);
    Type *I64Ty  = Type::getInt64Ty(Ctx);
    PointerType *PtrTy = PointerType::getUnqual(Ctx);

    /*
     * typedef struct ARR_DEC_INFO {
     *     void    *ptr;
     *     uint32_t bits;
     *     uint64_t mask;
     * } ARR_DEC_INFO;
     */
    StructType *DecodeInfoTy = StructType::get(
        Ctx, {PtrTy, I32Ty, I64Ty}, /*isPacked=*/false
    );

    /*
     * void __deobf_data(ARR_DEC_INFO *items, uint32_t count);
     */
    FunctionCallee DecodeFunc = M.getOrInsertFunction(
        "__deobf_data", VoidTy, PtrTy, I32Ty
    );

    Value *Zero = ConstantInt::get(I32Ty, 0);

    for (GlobalVariable *GV : Arrays) {
        if (!GV || !GV->hasInitializer())
            continue;

        /* ── 1. Build the decode-info records for this GV ── */

        SmallVector<Constant *, 8> RootIndices;
        RootIndices.push_back(ConstantInt::get(I32Ty, 0));

        uint64_t InitialPath = llvm::hash_value(GV->getName());

        std::vector<decodeInfo> DecodeData;
        emitDecodeForAggregateRecursive(
            GV, GV->getValueType(), RootIndices,
            Key, DL, InitialPath, DecodeData
        );

        if (DecodeData.empty())
            continue;

        std::vector<Constant *> Records;
        Records.reserve(DecodeData.size());

        for (const decodeInfo &Info : DecodeData) {
            if (!Info.GV || !Info.Ty || Info.Indices.empty())
                continue;

            Constant *PtrConst = ConstantExpr::getInBoundsGetElementPtr(
                Info.GV->getValueType(), Info.GV, Info.Indices
            );

            uint64_t Mask = buildPerByteMask(Info.Ty, Key, Info.PathHash);

            Records.push_back(ConstantStruct::get(DecodeInfoTy, {
                PtrConst,
                ConstantInt::get(I32Ty, Info.Bits),
                ConstantInt::get(I64Ty, Mask)
            }));
        }

        if (Records.empty())
            continue;

        if (Records.size() > UINT32_MAX) {
            errs() << "[arrenc] too many decoder entries for "
                   << GV->getName() << "; skipped\n";
            continue;
        }

        /* ── 2. Create a private per-GV descriptor table ── */

        /*
         * @__arr_obf_desc_<name> = private constant [N x { ptr, i32, i64 }] [...]
         */
        ArrayType *DescArrTy = ArrayType::get(DecodeInfoTy, Records.size());
        Constant  *DescInit  = ConstantArray::get(DescArrTy, Records);

        GlobalVariable *DescGV = new GlobalVariable(
            M,
            DescArrTy,
            /*isConstant=*/true,
            GlobalValue::PrivateLinkage,
            DescInit,
            Twine("__arr_obf_desc_") + GV->getName()
        );
        DescGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        DescGV->setAlignment(DL.getABITypeAlign(DescArrTy));

        uint32_t Count = static_cast<uint32_t>(Records.size());

        /* ── 3. Collect all instructions that use this GV ── */

        SmallVector<Instruction *, 16> UseInsts;
        collectUsingInstructions(GV, UseInsts);

        if (UseInsts.empty())
            continue;

        /* ── 4. Wrap each use site with decode-before / encode-after ── */

        for (Instruction *UseInst : UseInsts) {
            /*
             * Skip terminators: a load/store is never a terminator in normal
             * IR, but guard defensively — we cannot insert after a terminator.
             */
            if (UseInst->isTerminator())
                continue;

            Instruction *NextInst = UseInst->getNextNode();
            if (!NextInst)
                continue;

            /* Decode before the use. */
            {
                IRBuilder<NoFolder> B(UseInst);

                Value *DescPtr = B.CreateInBoundsGEP(
                    DescArrTy, DescGV, {Zero, Zero},
                    Twine("arr.dec.ptr.") + GV->getName()
                );

                B.CreateCall(DecodeFunc,
                    {DescPtr, ConstantInt::get(I32Ty, Count)});
            }

            /* Re-encode after the use (same XOR → same call). */
            {
                IRBuilder<NoFolder> B(NextInst);

                Value *DescPtr = B.CreateInBoundsGEP(
                    DescArrTy, DescGV, {Zero, Zero},
                    Twine("arr.enc.ptr.") + GV->getName()
                );

                B.CreateCall(DecodeFunc,
                    {DescPtr, ConstantInt::get(I32Ty, Count)});
            }
        }
    }
}

bool llvm::ArrayObfuscationPass::traverseGlobals(
    LLVMContext &Ctx,
    ArrayRef<GlobalVariable *> Arrays,
    std::vector<GlobalVariable *> &Out,
    llvm::DenseSet<GlobalVariable *> &Visited,
    const uint32_t Key
) {
    bool Changed = false;

    for (GlobalVariable *GV : Arrays) {
        if (!GV)
            continue;

        if (Visited.contains(GV))
            continue;

        Visited.insert(GV);

        if (!GV->hasInitializer())
            continue;
    
        Constant *Init = GV->getInitializer();
        


        SmallVector<GlobalVariable *, 32> TargetArrays;
        collectGlobalRefsFromConstant(Init, TargetArrays);

        if (!TargetArrays.empty()) {
            bool SubChanged = traverseGlobals(
                Ctx,
                TargetArrays,
                Out,
                Visited,
                Key
            );

            Changed |= SubChanged;
        }   


        if (isa<ConstantDataArray>(Init) ||
            isa<ConstantArray>(Init) ||
            isa<ConstantStruct>(Init) ||
            isa<ConstantVector>(Init)) {

            bool NestedChanged = false;


            uint64_t InitialPath = llvm::hash_value(GV->getName());

            Constant *NewInit = encodeConstantArrayRecursive(
                Ctx,
                Init,
                Key,
                NestedChanged,
                InitialPath
            );

            if (NestedChanged && NewInit != Init) {
                GV->setInitializer(NewInit);
                makeDecodedRuntimeGlobal(GV);
                Out.push_back(GV);
                Changed = true;
                continue;
            }
        }

        
        // 1. Directly encode integer arrays.
        /*
        if (auto *ArrTy = dyn_cast<ArrayType>(GV->getValueType())) {
            Type *ElemTy = ArrTy->getElementType();

            auto *CDA = dyn_cast<ConstantDataArray>(Init);
            auto *CA  = dyn_cast<ConstantArray>(Init);

            if (CDA || CA) {
                unsigned NumElements = CDA ? CDA->getNumElements() : CA->getNumOperands();

                auto getElementValue = [&](unsigned I) -> std::optional<uint64_t> {
                    if (CDA)
                        return CDA->getElementAsInteger(I);

                    if (!CA)
                        return std::nullopt;

                    auto *CI = dyn_cast<ConstantInt>(CA->getOperand(I));
                    if (!CI)
                        return std::nullopt;

                    return CI->getZExtValue();
                };

                
                if (ElemTy->isIntegerTy(8)) {
                    std::vector<uint8_t> Encoded;
                    Encoded.reserve(NumElements);

                    bool BadElement = false;

                    for (unsigned I = 0; I < NumElements; ++I) {
                        auto V = getElementValue(I);
                        if (!V) {
                            BadElement = true;
                            break;
                        }

                        uint64_t K = expandKeyForIntegerType(ElemTy, Key);
                        Encoded.push_back(static_cast<uint8_t>(*V ^ K));
                    }

                    if (!BadElement) {
                        GV->setInitializer(ConstantDataArray::get(
                            Ctx,
                            ArrayRef<uint8_t>(Encoded.data(), Encoded.size())
                        ));

                        makeDecodedRuntimeGlobal(GV);
                        Out.push_back(GV);
                        Changed = true;
                        continue;
                    }
                }

                if (ElemTy->isIntegerTy(16)) {
                    std::vector<uint16_t> Encoded;
                    Encoded.reserve(NumElements);

                    bool BadElement = false;

                    for (unsigned I = 0; I < NumElements; ++I) {
                        auto V = getElementValue(I);
                        if (!V) {
                            BadElement = true;
                            break;
                        }
                        uint64_t K = expandKeyForIntegerType(ElemTy, Key);
                        Encoded.push_back(static_cast<uint16_t>(*V ^ K));
                    }

                    if (!BadElement) {
                        GV->setInitializer(ConstantDataArray::get(
                            Ctx,
                            ArrayRef<uint16_t>(Encoded.data(), Encoded.size())
                        ));
                        makeDecodedRuntimeGlobal(GV);
                        Out.push_back(GV);
                        Changed = true;
                        continue;
                    }
                }

                if (ElemTy->isIntegerTy(32)) {
                    std::vector<uint32_t> Encoded;
                    Encoded.reserve(NumElements);

                    bool BadElement = false;

                    for (unsigned I = 0; I < NumElements; ++I) {
                        auto V = getElementValue(I);
                        if (!V) {
                            BadElement = true;
                            break;
                        }

                        uint64_t K = expandKeyForIntegerType(ElemTy, Key);
                        Encoded.push_back(static_cast<uint32_t>(*V ^ K));
                    }

                    if (!BadElement) {
                        GV->setInitializer(ConstantDataArray::get(
                            Ctx,
                            ArrayRef<uint32_t>(Encoded.data(), Encoded.size())
                        ));
                        makeDecodedRuntimeGlobal(GV);

                        Out.push_back(GV);
                        Changed = true;
                        continue;
                    }
                }

                if (ElemTy->isIntegerTy(64)) {
                    std::vector<uint64_t> Encoded;
                    Encoded.reserve(NumElements);

                    bool BadElement = false;

                    for (unsigned I = 0; I < NumElements; ++I) {
                        auto V = getElementValue(I);
                        if (!V) {
                            BadElement = true;
                            break;
                        }
                        uint64_t K = expandKeyForIntegerType(ElemTy, Key);
                        Encoded.push_back(static_cast<uint64_t>(*V ^ K));
                    }

                    if (!BadElement) {
                        GV->setInitializer(ConstantDataArray::get(
                            Ctx,
                            ArrayRef<uint64_t>(Encoded.data(), Encoded.size())
                        ));

                        makeDecodedRuntimeGlobal(GV);

                        Out.push_back(GV);
                        Changed = true;
                        continue;
                    }
                }
            }
        }
        */
    }

    return Changed;
}


void llvm::ArrayObfuscationPass::makeDecodedRuntimeGlobal(GlobalVariable *GV) {
    if (!GV)
        return;

    GV->setConstant(false);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::None);

    if (GV->hasComdat())
        GV->setComdat(nullptr);

    if (GV->hasLinkOnceODRLinkage() ||
        GV->hasWeakODRLinkage() ||
        GV->hasLinkOnceAnyLinkage() ||
        GV->hasWeakAnyLinkage()) {
        GV->setLinkage(GlobalValue::InternalLinkage);
    }
}

void llvm::ArrayObfuscationPass::collectGlobalRefsFromConstant(
    Constant *C,
    SmallVectorImpl<GlobalVariable *> &Refs
) {
    if (!C)
        return;

    C = C->stripPointerCasts();

    if (auto *GV = dyn_cast<GlobalVariable>(C)) {
        Refs.push_back(GV);
        return;
    }

    if (isa<ConstantDataSequential>(C))
        return;

    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
        for (Use &U : CE->operands()) {
            if (auto *OpC = dyn_cast<Constant>(U.get()))
                collectGlobalRefsFromConstant(OpC, Refs);
        }
        return;
    }

    if (auto *CA = dyn_cast<ConstantArray>(C)) {
        for (Use &U : CA->operands()) {
            if (auto *OpC = dyn_cast<Constant>(U.get()))
                collectGlobalRefsFromConstant(OpC, Refs);
        }
        return;
    }
    
    if (auto *CS = dyn_cast<ConstantStruct>(C)) {
        for (Use &U : CS->operands()) {
            if (auto *OpC = dyn_cast<Constant>(U.get()))
                collectGlobalRefsFromConstant(OpC, Refs);
        }
        return;
    }
    

    if (auto *CV = dyn_cast<ConstantVector>(C)) {
        for (Use &U : CV->operands()) {
            if (auto *OpC = dyn_cast<Constant>(U.get()))
                collectGlobalRefsFromConstant(OpC, Refs);
        }
        return;
    }
}


bool llvm::ArrayObfuscationPass::isSupportedIntegerType(Type *Ty) {
    return Ty->isIntegerTy(8)  ||
           Ty->isIntegerTy(16) ||
           Ty->isIntegerTy(32) ||
           Ty->isIntegerTy(64);
}

llvm::Constant *llvm::ArrayObfuscationPass::encodeConstantArrayRecursive(
    LLVMContext &Ctx,
    Constant *C,
    uint32_t Key,
    bool &Changed,
    uint64_t PathHash
) {
    if (!C)
        return C;

    if (auto *CI = dyn_cast<ConstantInt>(C)) {
        Type *Ty = CI->getType();

        if (!isSupportedIntegerType(Ty))
            return C;

        uint64_t V = CI->getZExtValue();

        uint64_t K = buildPerByteMask(Ty, Key, PathHash);
        uint64_t Enc = V ^ K;

        Changed = true;
        return ConstantInt::get(Ty, Enc);
    }

    if (auto *CDA = dyn_cast<ConstantDataArray>(C)) {
        Type *ElemTy = CDA->getElementType();

        if (!isSupportedIntegerType(ElemTy))
            return C;

        SmallVector<Constant *, 64> NewElems;
        NewElems.reserve(CDA->getNumElements());

        for (unsigned I = 0; I < CDA->getNumElements(); ++I) {
            uint64_t V = CDA->getElementAsInteger(I);

            uint64_t ChildPath =
                PathHash ^ (0x9E3779B97F4A7C15ULL + I * 0x100000001B3ULL);

            uint64_t K = buildPerByteMask(ElemTy, Key, ChildPath);
            uint64_t Enc = V ^ K;

            NewElems.push_back(ConstantInt::get(ElemTy, Enc));
        }

        Changed = true;
        return ConstantArray::get(cast<ArrayType>(CDA->getType()), NewElems);
    }

    if (auto *CA = dyn_cast<ConstantArray>(C)) {
        SmallVector<Constant *, 64> NewOps;
        NewOps.reserve(CA->getNumOperands());

        bool LocalChanged = false;


        unsigned I = 0;

        for (Use &U : CA->operands()) {
            auto *Child = dyn_cast<Constant>(U.get());
            if (!Child)
                return C;

            bool ChildChanged = false;

            uint64_t ChildPath =
                PathHash ^ (0x9E3779B97F4A7C15ULL + I * 0x100000001B3ULL);

            Constant *NewChild = encodeConstantArrayRecursive(
                Ctx,
                Child,
                Key,
                ChildChanged,
                ChildPath
            );

            LocalChanged |= ChildChanged;
            NewOps.push_back(NewChild);
            ++I;
        }

        if (!LocalChanged)
            return C;

        Changed = true;
        return ConstantArray::get(CA->getType(), NewOps);
    }
    if (auto *CS = dyn_cast<ConstantStruct>(C)) {
        SmallVector<Constant *, 32> NewOps;
        NewOps.reserve(CS->getNumOperands());

        bool LocalChanged = false;

        unsigned I = 0;

        for (Use &U : CS->operands()) {
            auto *Child = dyn_cast<Constant>(U.get());
            if (!Child)
                return C;

            bool ChildChanged = false;

            uint64_t ChildPath =
                PathHash ^ (0xD6E8FEB86659FD93ULL + I * 0x9E3779B97F4A7C15ULL);

            Constant *NewChild = encodeConstantArrayRecursive(
                Ctx,
                Child,
                Key,
                ChildChanged,
                ChildPath
            );

            LocalChanged |= ChildChanged;
            NewOps.push_back(NewChild);
            ++I;
        }

        if (!LocalChanged)
            return C;

        Changed = true;

        return ConstantStruct::get(cast<StructType>(CS->getType()), NewOps);
    }

    return C;
}

void llvm::ArrayObfuscationPass::emitDecodeForAggregateRecursive(
    GlobalVariable *GV,
    Type *CurrentTy,
    SmallVectorImpl<Constant *> &Indices,
    uint32_t Key,
    const DataLayout &DL,
    uint64_t PathHash,
    std::vector<decodeInfo> &Out
) {
    LLVMContext &Ctx = GV->getContext();
    Type *I32Ty = Type::getInt32Ty(Ctx);

    /*
     * Array case:
     *   [N x T]
     */
    if (auto *ArrTy = dyn_cast<ArrayType>(CurrentTy)) {
        Type *ElemTy = ArrTy->getElementType();

        for (uint64_t I = 0; I < ArrTy->getNumElements(); ++I) {
            Indices.push_back(ConstantInt::get(I32Ty, I));

            uint64_t ChildPath =
                PathHash ^ (0x9E3779B97F4A7C15ULL + I * 0x100000001B3ULL);

            emitDecodeForAggregateRecursive(
                GV,
                ElemTy,
                Indices,
                Key,
                DL,
                ChildPath,
                Out
            );

            Indices.pop_back();
        }

        return;
    }

    /*
     * Struct case:
     *   %struct.X = type { ... }
     */
    if (auto *STy = dyn_cast<StructType>(CurrentTy)) {
        if (STy->isOpaque())
            return;

        for (unsigned I = 0; I < STy->getNumElements(); ++I) {
            Type *FieldTy = STy->getElementType(I);

            Indices.push_back(ConstantInt::get(I32Ty, I));

            uint64_t ChildPath =
                PathHash ^ (0xD6E8FEB86659FD93ULL + I * 0x9E3779B97F4A7C15ULL);

            emitDecodeForAggregateRecursive(
                GV,
                FieldTy,
                Indices,
                Key,
                DL,
                ChildPath,
                Out
            );

            Indices.pop_back();
        }

        return;
    }

    /*
     * Integer leaf:
     *   i8 / i16 / i32 / i64
     */
    if (isSupportedIntegerType(CurrentTy)) {
        decodeInfo Info;

        Info.GV = GV;
        Info.Ty = CurrentTy;
        Info.Bits = CurrentTy->getIntegerBitWidth();
        Info.PathHash = PathHash;

        for (Constant *C : Indices)
            Info.Indices.push_back(C);

        Out.push_back(std::move(Info));
        return;
    }

    /*
     * Ignore ptr, float, double, function ptr, etc.
     */
    return;
}

uint64_t llvm::ArrayObfuscationPass::buildPerByteMask(Type *Ty, uint8_t Seed, uint64_t ElementIndex) {
    unsigned Bits = cast<IntegerType>(Ty)->getBitWidth();
    unsigned Bytes = Bits / 8;

    uint64_t Mask = 0;

    for (unsigned B = 0; B < Bytes; ++B) {
        uint8_t Kb = deriveKeyByte(Seed, ElementIndex, B);
        Mask |= static_cast<uint64_t>(Kb) << (B * 8);
    }

    return Mask;
}

uint8_t llvm::ArrayObfuscationPass::deriveKeyByte(
    uint8_t Seed,
    uint64_t PathHash,
    unsigned ByteIndex
) {
    uint64_t X = PathHash;

    X ^= static_cast<uint64_t>(Seed) * 0x9E3779B97F4A7C15ULL;
    X ^= static_cast<uint64_t>(ByteIndex + 1) * 0xBF58476D1CE4E5B9ULL;

    X ^= X >> 30;
    X *= 0xBF58476D1CE4E5B9ULL;
    X ^= X >> 27;
    X *= 0x94D049BB133111EBULL;
    X ^= X >> 31;

    uint8_t K = static_cast<uint8_t>(X & 0xFF);
    return K ? K : 0xA5;
}