#include "ApiHashing.h"

static llvm::cl::opt<std::string> os_version(
    "os_version",
    llvm::cl::desc("only supported -os windows"),
    llvm::cl::value_desc("-os_version"),
    llvm::cl::Required);

static llvm::cl::opt<std::string> api_type(
    "api_type",
    llvm::cl::desc("Supported types hashing,threadpool"),
    llvm::cl::value_desc("api_type"),
    llvm::cl::init("hashing"));

static llvm::cl::opt<std::string> api_entry(
    "api_entry",
    llvm::cl::desc("program entry"),
    llvm::cl::value_desc("-api_entry"),
    llvm::cl::Required);

static llvm::cl::opt<std::string> excluded_funcs(
    "excluded_funcs",
    llvm::cl::desc("excluded funcs from hashing"),
    llvm::cl::value_desc("-excluded_funcs=func1,func2"),
    llvm::cl::Optional);

llvm::StringSet<> llvm::ApiHashingPass::parseExcludedFuncs() {
    llvm::StringSet<> Excluded;

    if (excluded_funcs.empty())
        return Excluded;

    llvm::SmallVector<llvm::StringRef, 16> Items;
    llvm::StringRef(excluded_funcs).split(Items, ",");

    for (llvm::StringRef Item : Items) {
        Item = Item.trim();

        if (!Item.empty())
            Excluded.insert(Item);
    }

    return Excluded;
}
    
llvm::PreservedAnalyses llvm::ApiHashingPass::run(
    Module &M,
    ModuleAnalysisManager &AM
) {
    if (os_version.getValue() != "windows") {
        errs() << "Only supporting windows as os_version";
        return PreservedAnalyses::all();
    }

    if (api_type.getValue() != "hashing" && api_type.getValue() != "threadpool") {
        errs() << "Only supporting hashing and threadpool as api hashing";
        return PreservedAnalyses::all();
    }

    Function *Main = nullptr;
    
    Main = M.getFunction(api_entry.getValue());
    if (!Main || Main->isDeclaration()){
        errs() << "Entry for the program not found";
        return PreservedAnalyses::all();
    }

    llvm::StringSet<> Excluded = parseExcludedFuncs();

    LLVMContext &Ctx = M.getContext();

    Type *VoidTy  = Type::getVoidTy(Ctx);
    Type *Int32Ty = Type::getInt32Ty(Ctx);
    auto *PtrTy   = PointerType::getUnqual(Ctx);

    FunctionCallee ResolverFunc = M.getOrInsertFunction(
        "getFunctionAddressByHash",
        PtrTy,    // return ptr
        PtrTy,    // const char *
        Int32Ty   // uint32_t
    );

    struct ApiRecord {
        std::string Name;
        std::string Library;
        uint32_t Hash = 0;
        GlobalVariable *Slot = nullptr;
    };

    struct ApiUse {
        CallBase *CB = nullptr;
        std::string Name;
    };

    auto sanitizeName = [](StringRef S) -> std::string {
        std::string Out;
        Out.reserve(S.size());

        for (char C : S) {
            if ((C >= 'a' && C <= 'z') ||
                (C >= 'A' && C <= 'Z') ||
                (C >= '0' && C <= '9') ||
                C == '_') {
                Out.push_back(C);
            } else {
                Out.push_back('_');
            }
        }

        return Out;
    };

    std::map<std::string, ApiRecord> Records;
    std::vector<ApiUse> Uses;

    /*
     * 1. Scan the whole module.
     */
    for (Function &F : M) {
        /*
         * Do not rewrite the resolver itself.
         * If you hash LoadLibrary/GetProcAddress inside the resolver,
         * resolution can become recursive and break.
         */
        if (Excluded.contains(F.getName()))
            continue;

        if (F.isDeclaration())
            continue;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                auto *CB = dyn_cast<CallBase>(&I);
                if (!CB)
                    continue;

                Function *Called = CB->getCalledFunction();
                if (!Called)
                    continue;

                if (!Called->hasName())
                    continue;

                if (!Called->isDeclaration())
                    continue;

                StringRef Name = Called->getName();

                std::string Library = getLibraryForFunction(Name).str();

                if (Library.empty())
                    continue;

                std::string ApiName = Name.str();

                auto It = Records.find(ApiName);

                if (It == Records.end()) {
                    ApiRecord Rec;
                    Rec.Name = ApiName;
                    Rec.Library = Library;
                    Rec.Hash = crc32(Name.data(), Name.size());

                    Records.emplace(ApiName, std::move(Rec));
                }

                Uses.push_back({CB, ApiName});
            }
        }
    }

    if (Records.empty() || Uses.empty()) {
        return PreservedAnalyses::all();
    }

    if (!Main) {
        errs() << "[api_hashing] no main/entry function found; "
               << "not replacing API calls\n";
        return PreservedAnalyses::all();
    }

    /*
     * 3. Create one global ptr slot per API.
     *
     * Example:
     *   @__apihash_ptr_CreateFileW = private global ptr null
     */
    for (auto &KV : Records) {
        ApiRecord &Rec = KV.second;

        std::string SlotName =
            "__apihash_ptr_" + sanitizeName(Rec.Name);

        GlobalVariable *Slot = M.getGlobalVariable(SlotName);

        if (!Slot) {
            Slot = new GlobalVariable(
                M,
                PtrTy,
                false,
                GlobalValue::PrivateLinkage,
                ConstantPointerNull::get(PtrTy),
                SlotName
            );

            Slot->setAlignment(MaybeAlign(M.getDataLayout().getPointerSize()));
        }

        Rec.Slot = Slot;
    }

    /*
     * 4. Insert all resolver calls at the beginning of main.
     */
    BasicBlock &Entry = Main->getEntryBlock();
    Instruction *InsertPt = Entry.getFirstNonPHI();
    while (isa<AllocaInst>(InsertPt) && !InsertPt->isTerminator())
        InsertPt = InsertPt->getNextNode();

    IRBuilder<NoFolder> MainBuilder(InsertPt);

    for (auto &KV : Records) {
        ApiRecord &Rec = KV.second;

        if (!Rec.Slot)
            continue;

        std::string SafeName = sanitizeName(Rec.Name);

        Value *LibraryName = MainBuilder.CreateGlobalStringPtr(
            Rec.Library,
            "api.lib." + SafeName
        );

        Value *HashArg = ConstantInt::get(Int32Ty, Rec.Hash);

        Value *ResolvedPtr = MainBuilder.CreateCall(
            ResolverFunc,
            {
                LibraryName,
                HashArg
            },
            "resolved." + SafeName
        );

        MainBuilder.CreateStore(
            ResolvedPtr,
            Rec.Slot
        );
    }

    ApiRecord TpAllocWork;
    TpAllocWork.Name = "TpAllocWork";
    TpAllocWork.Library = "ntdll.dll";
    TpAllocWork.Hash = crc32(TpAllocWork.Name.c_str(), TpAllocWork.Name.length());
    TpAllocWork.Slot = new GlobalVariable(M,PtrTy,false,GlobalValue::PrivateLinkage,ConstantPointerNull::get(PtrTy),"__apihash_ptr_" + sanitizeName(TpAllocWork.Name));
    TpAllocWork.Slot->setAlignment(MaybeAlign(M.getDataLayout().getPointerSize()));

    Value *TpAllocWorkLN = MainBuilder.CreateGlobalStringPtr(
            TpAllocWork.Library,
            "api.lib." + sanitizeName(TpAllocWork.Name)
    );
    Value *HashArgAllocWork = ConstantInt::get(Int32Ty, TpAllocWork.Hash);


    Value *TpAllocWorkPtr = MainBuilder.CreateCall(
        ResolverFunc,
        {
            TpAllocWorkLN,
            HashArgAllocWork
        },
        "resolved." + sanitizeName(TpAllocWork.Name)
    );

    MainBuilder.CreateStore(
        TpAllocWorkPtr,
        TpAllocWork.Slot
    );

    ApiRecord TpPostWork;
    TpPostWork.Name = "TpPostWork";
    TpPostWork.Library = "ntdll.dll";
    TpPostWork.Hash = crc32(TpPostWork.Name.c_str(), TpPostWork.Name.length());
    TpPostWork.Slot = new GlobalVariable(M,PtrTy,false,GlobalValue::PrivateLinkage,ConstantPointerNull::get(PtrTy),"__apihash_ptr_" + sanitizeName(TpPostWork.Name));
    TpPostWork.Slot->setAlignment(MaybeAlign(M.getDataLayout().getPointerSize()));


    Value *TpPostWorkLN = MainBuilder.CreateGlobalStringPtr(
            TpPostWork.Library,
            "api.lib." + sanitizeName(TpPostWork.Name)
    );

    Value *HashArgPostWork = ConstantInt::get(Int32Ty, TpPostWork.Hash);

    Value *TpPostWorkPtr = MainBuilder.CreateCall(
        ResolverFunc,
        {
            TpPostWorkLN,
            HashArgPostWork
        },
        "resolved." + sanitizeName(TpPostWork.Name)
    );
    
    MainBuilder.CreateStore(
        TpPostWorkPtr,
        TpPostWork.Slot
    );



    ApiRecord TpReleaseWork;
    TpReleaseWork.Name = "TpReleaseWork";
    TpReleaseWork.Library = "ntdll.dll";
    TpReleaseWork.Hash = crc32(TpReleaseWork.Name.c_str(), TpReleaseWork.Name.length());
    TpReleaseWork.Slot = new GlobalVariable(M,PtrTy,false,GlobalValue::PrivateLinkage,ConstantPointerNull::get(PtrTy),"__apihash_ptr_" + sanitizeName(TpReleaseWork.Name));
    TpReleaseWork.Slot->setAlignment(MaybeAlign(M.getDataLayout().getPointerSize()));

    Value *TpReleaseWorkLN = MainBuilder.CreateGlobalStringPtr(
            TpReleaseWork.Library,
            "api.lib." + sanitizeName(TpReleaseWork.Name)
    );
    Value *HashArgReleaseWork = ConstantInt::get(Int32Ty, TpReleaseWork.Hash);


    Value *TpReleaseWorkPtr = MainBuilder.CreateCall(
        ResolverFunc,
        {
            TpReleaseWorkLN,
            HashArgReleaseWork
        },
        "resolved." + sanitizeName(TpReleaseWork.Name)
    );


    MainBuilder.CreateStore(
        TpReleaseWorkPtr,
        TpReleaseWork.Slot
    );

    ApiRecord TpWaitForWork;
    TpWaitForWork.Name = "TpWaitForWork";
    TpWaitForWork.Library = "ntdll.dll";
    TpWaitForWork.Hash = crc32(TpWaitForWork.Name.c_str(), TpWaitForWork.Name.length());
    TpWaitForWork.Slot = new GlobalVariable(M,PtrTy,false,GlobalValue::PrivateLinkage,ConstantPointerNull::get(PtrTy),"__apihash_ptr_" + sanitizeName(TpWaitForWork.Name));
    TpWaitForWork.Slot->setAlignment(MaybeAlign(M.getDataLayout().getPointerSize()));

    Value *TpWaitForWorkLN = MainBuilder.CreateGlobalStringPtr(
            TpWaitForWork.Library,
            "api.lib." + sanitizeName(TpWaitForWork.Name)
    );
    Value *HashArgWaitForWork = ConstantInt::get(Int32Ty, TpWaitForWork.Hash);


    Value *TpWaitForWorkPtr = MainBuilder.CreateCall(
        ResolverFunc,
        {
            TpWaitForWorkLN,
            HashArgWaitForWork
        },
        "resolved." + sanitizeName(TpWaitForWork.Name)
    );


    MainBuilder.CreateStore(
        TpWaitForWorkPtr,
        TpWaitForWork.Slot
    );

    for (ApiUse &Use : Uses) {
        if (!Use.CB)
            continue;

        auto It = Records.find(Use.Name);

        if (It == Records.end())
            continue;

        ApiRecord &Rec = It->second;

        if (!Rec.Slot)
            continue;

        IRBuilder<NoFolder> Builder(Use.CB);

        if (api_type.getValue() == "hashing" || (!Use.CB->use_empty() && Use.CB->arg_size() > 4) || Use.CB->arg_size() > 8) {
            Value *LoadedPtr = Builder.CreateLoad(
                PtrTy,
                Rec.Slot,
                "api.ptr." + sanitizeName(Rec.Name)
            );

            Use.CB->setCalledOperand(LoadedPtr);
        } else if (api_type.getValue() == "threadpool" && Use.CB->arg_size() > 4) {
            errs() << "Api " << Use.Name << "\n";
            Type *VoidTy  = Type::getVoidTy(Ctx);
            Type *Int32Ty = Type::getInt32Ty(Ctx);
            Type *Int8Ty  = Type::getInt8Ty(Ctx);
            Type *Int64Ty  = Type::getInt64Ty(Ctx);
            auto *PtrTy   = PointerType::getUnqual(Ctx);

            const DataLayout &DL = M.getDataLayout();
            Type *SizeTTy = DL.getIntPtrType(Ctx);

            Value *NullPtr = ConstantPointerNull::get(PtrTy);

            Function *EnclosingFn = Use.CB->getFunction();
            BasicBlock &FnEntry = EnclosingFn->getEntryBlock();
            IRBuilder<NoFolder> AllocaBuilder(&*FnEntry.getFirstNonPHI());
            Value *WorkSlot = AllocaBuilder.CreateAlloca(PtrTy, nullptr, "work.return.slot");

            // Then store null via the existing Builder (at the call site), not the alloca builder:
            Builder.CreateStore(NullPtr, WorkSlot);

            /*
            * ------------------------------------------------------------
            * 2.2 Declarations
            * ------------------------------------------------------------
            */
            /*
            FunctionCallee CallbackFn = M.getOrInsertFunction(
                "WorkCallbackSpoof",
                VoidTy,
                PtrTy,
                PtrTy,
                PtrTy
            );
            */
            FunctionCallee MallocFn = M.getOrInsertFunction(
                "malloc",
                PtrTy,
                SizeTTy
            );

            unsigned NumArgs = Use.CB->arg_size();

            std::vector<Type *> Fields;

            ArrayType *ArgsArrayTy = ArrayType::get(PtrTy, 8);

            Fields.push_back(PtrTy);                            // Function pointer
            Fields.push_back(PtrTy);                           // Return address to simulate stack return
            Fields.push_back(Int64Ty);                          // Argument count
            Fields.push_back(ArgsArrayTy);                           // Arguments (up to 15)


            StructType *ContextStructTy =
                StructType::get(Ctx, Fields, true);

            uint64_t StructSize = DL.getTypeAllocSize(ContextStructTy);

            CallInst *MallocCall = Builder.CreateCall(
                MallocFn,
                { ConstantInt::get(SizeTTy, StructSize) },
                "ctx.malloc"
            );

            Value *StructPtr = MallocCall;

            Value *TypedStructPtr = Builder.CreateBitCast(
                StructPtr,
                PointerType::getUnqual(ContextStructTy),
                "ctx.typed"
            );

            Value *TargetFuncPtr= Builder.CreateLoad(
                PtrTy,
                Rec.Slot,
                "api.fptr." + sanitizeName(Rec.Name)
            );

            Value *FuncPtrField =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 0);

            Builder.CreateStore(TargetFuncPtr, FuncPtrField);

            // field 2: Return address to simulate stack return
            Value *RetAddrPtr =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 1, "ctx.result.ptr");
            Builder.CreateStore(NullPtr, RetAddrPtr);

            // field 3: Argument count
            Value *NumArgsField =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 2, "ctx.event.ptr");
            Builder.CreateStore(Builder.getInt64(NumArgs), NumArgsField);

            /*
            * Fields 4+: Arguments
            */
            Value *ArgsField = Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 3);
            
            for (unsigned ArgNo = 0; ArgNo < NumArgs; ++ArgNo) {
                Value *ArgPtr = Builder.CreateGEP(
                    ArgsArrayTy,
                    ArgsField,
                    {
                        Builder.getInt32(0),
                        Builder.getInt32(ArgNo)
                    }
                );

                Value *Arg = Use.CB->getArgOperand(ArgNo);

                Type *ArgTy = Arg->getType();

                if (ArgTy->isPointerTy()) {
                    // ok
                }
                else if (ArgTy->isIntegerTy()) {

                    unsigned Bits = cast<IntegerType>(ArgTy)->getBitWidth();

                    if (Bits < 64)
                        Arg = Builder.CreateZExt(Arg, Int64Ty);

                    Arg = Builder.CreateIntToPtr(Arg, PtrTy);
                }
                else {
                    errs() << "Unsupported arg type\n";
                }

                Builder.CreateStore(
                    Arg,
                    ArgPtr
                );
            }

            FunctionCallee CallbackFn = M.getOrInsertFunction(
                "StackSmashingCallback",
                VoidTy,
                PtrTy,
                PtrTy,
                PtrTy
            );
            
            FunctionType *AllocFTy = FunctionType::get(
                Int32Ty,
                { PtrTy, PtrTy, PtrTy, PtrTy },
                false
            );

            Value *TpAllocWorkLoad = Builder.CreateLoad(
                PtrTy,
                TpAllocWork.Slot,
                "load.TpAllocWork"
            );

            CallInst *TpAllocCall = Builder.CreateCall(
                AllocFTy,
                TpAllocWorkLoad,
                {
                    WorkSlot,
                    CallbackFn.getCallee(),
                    StructPtr,
                    NullPtr
                },
                "tpallocwork.status"
            );


            Value *WorkHandle = Builder.CreateLoad(
                PtrTy,
                WorkSlot,
                "work.handle.val"
            );


            FunctionType *PostFTy = FunctionType::get(
                VoidTy,
                { PtrTy },
                false
            );

            Value *TpPostWorkLoad = Builder.CreateLoad(
                PtrTy,
                TpPostWork.Slot,
                "load.TpPostWork"
            );

            CallInst *TpPostCall = Builder.CreateCall(
                PostFTy,
                TpPostWorkLoad,
                { WorkHandle }
            );

            
            FunctionType *WaitFTy = FunctionType::get(
                VoidTy,
                { PtrTy, Int32Ty },  // LOGICAL = ULONG = i32
                false
            );

            Value *TpWaitForWorkLoad = Builder.CreateLoad(
                PtrTy,
                TpWaitForWork.Slot,
                "load.TpWaitForWork"
            );

                        
            CallInst *TpWaitCall = Builder.CreateCall(
                WaitFTy,
                TpWaitForWorkLoad,
                {
                    WorkHandle,
                    ConstantInt::get(Int32Ty, 0)   // was Int8Ty
                }
            );

            
            FunctionType *ReleaseFTy = FunctionType::get(
                VoidTy,
                { PtrTy },
                false
            );

            Value *TpReleaseWorkLoad = Builder.CreateLoad(
                PtrTy,
                TpReleaseWork.Slot,
                "load.TpReleaseWork"
            );

            CallInst *TpReleaseCall = Builder.CreateCall(
                ReleaseFTy,
                TpReleaseWorkLoad,
                { WorkHandle }
            );

            
            FunctionCallee FreeFn = M.getOrInsertFunction(
                "free",
                VoidTy,
                PtrTy
            );

            CallInst *FreeCall = Builder.CreateCall(
                FreeFn,
                { StructPtr }
            );
            

            // Propagate the recovered return value to every use of the
            // original call *before* finishReplaCBngCallBase erases it.
            // For void calls ResultReplacement is null — nothing to do.
            // finishReplaCBngCallBase's own replaceAllUsesWith(Undef) becomes
            // a no-op because the use-list is already empty at that point.
            // ── REMOVE these two lines ────────────────────────────────────────────────


            if (!finishReplaCBngCallBase(Use.CB)) {
                errs() << "[api-async] failed to replace callbase\n";
                continue;
            }

        } else if (api_type.getValue() == "threadpool") {
            Type *VoidTy  = Type::getVoidTy(Ctx);
            Type *Int32Ty = Type::getInt32Ty(Ctx);
            Type *Int8Ty  = Type::getInt8Ty(Ctx);
            Type *Int64Ty  = Type::getInt64Ty(Ctx);
            auto *PtrTy   = PointerType::getUnqual(Ctx);

            const DataLayout &DL = M.getDataLayout();
            Type *SizeTTy = DL.getIntPtrType(Ctx);

            Value *NullPtr = ConstantPointerNull::get(PtrTy);

            Function *EnclosingFn = Use.CB->getFunction();
            BasicBlock &FnEntry = EnclosingFn->getEntryBlock();
            IRBuilder<NoFolder> AllocaBuilder(&*FnEntry.getFirstNonPHI());
            Value *WorkSlot = AllocaBuilder.CreateAlloca(PtrTy, nullptr, "work.return.slot");

            // Then store null via the existing Builder (at the call site), not the alloca builder:
            Builder.CreateStore(NullPtr, WorkSlot);

            /*
            * ------------------------------------------------------------
            * 2.2 Declarations
            * ------------------------------------------------------------
            */

            FunctionCallee MallocFn = M.getOrInsertFunction(
                "malloc",
                PtrTy,
                SizeTTy
            );

            unsigned NumArgs = Use.CB->arg_size();

            std::vector<Type *> Fields;

            ArrayType *ArgsArrayTy = ArrayType::get(PtrTy, 4);

            Fields.push_back(PtrTy);                           // Function pointer
            Fields.push_back(PtrTy);                           // Return address to simulate stack return
            Fields.push_back(Int64Ty);                         // Argument count
            Fields.push_back(ArgsArrayTy);                     // Arguments (up to 15)


            StructType *ContextStructTy =
                StructType::get(Ctx, Fields, true);

            uint64_t StructSize = DL.getTypeAllocSize(ContextStructTy);

            CallInst *MallocCall = Builder.CreateCall(
                MallocFn,
                { ConstantInt::get(SizeTTy, StructSize) },
                "ctx.malloc"
            );

            Value *StructPtr = MallocCall;

            Value *TypedStructPtr = Builder.CreateBitCast(
                StructPtr,
                PointerType::getUnqual(ContextStructTy),
                "ctx.typed"
            );

            Value *TargetFuncPtr= Builder.CreateLoad(
                PtrTy,
                Rec.Slot,
                "api.fptr." + sanitizeName(Rec.Name)
            );

            Value *FuncPtrField =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 0);

            Builder.CreateStore(TargetFuncPtr, FuncPtrField);

            // field 2: Return address to simulate stack return
            Value *RetAddrPtr =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 1, "ctx.result.ptr");
            Builder.CreateStore(NullPtr, RetAddrPtr);

            // field 3: Argument count
            Value *NumArgsField =
                Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 2, "ctx.event.ptr");
            Builder.CreateStore(Builder.getInt64(NumArgs), NumArgsField);

            /*
            * Fields 4+: Arguments
            */
            Value *ArgsField = Builder.CreateStructGEP(ContextStructTy, TypedStructPtr, 3);
            
            for (unsigned ArgNo = 0; ArgNo < NumArgs; ++ArgNo) {
                Value *ArgPtr = Builder.CreateGEP(
                    ArgsArrayTy,
                    ArgsField,
                    {
                        Builder.getInt32(0),
                        Builder.getInt32(ArgNo)
                    }
                );

                Value *Arg = Use.CB->getArgOperand(ArgNo);

                Type *ArgTy = Arg->getType();

                if (ArgTy->isPointerTy()) {
                    // ok
                }
                else if (ArgTy->isIntegerTy()) {

                    unsigned Bits = cast<IntegerType>(ArgTy)->getBitWidth();

                    if (Bits < 64)
                        Arg = Builder.CreateZExt(Arg, Int64Ty);

                    Arg = Builder.CreateIntToPtr(Arg, PtrTy);
                }
                else {
                    errs() << "Unsupported arg type\n";
                }

                Builder.CreateStore(
                    Arg,
                    ArgPtr
                );
            }

            FunctionCallee ProxyPoolingFn = M.getOrInsertFunction(
                "ProxyPooling",
                Int64Ty,
                PtrTy,
                Int32Ty
            );

            CallInst *ProxyPollingCall = Builder.CreateCall(
                ProxyPoolingFn,
                { StructPtr, Builder.getInt32(0)},
                "ctx.proxypooling"
            );

            // Propagate the recovered return value to every use of the
            // original call *before* finishReplaCBngCallBase erases it.
            // For void calls ResultReplacement is null — nothing to do.
            // finishReplaCBngCallBase's own replaceAllUsesWith(Undef) becomes
            // a no-op because the use-list is already empty at that point.
            // ── REMOVE these two lines ────────────────────────────────────────────────

            
            if (ProxyPollingCall) {
                Type *OrigRetTy = Use.CB->getType();

                if (!OrigRetTy->isVoidTy()) {
                    Value *ResultReplacement = nullptr;

                    if (OrigRetTy->isPointerTy()) {
                        ResultReplacement = Builder.CreateIntToPtr(
                            ProxyPollingCall, OrigRetTy, "result.as.ptr");

                    } else if (OrigRetTy->isIntegerTy()) {
                        unsigned W = cast<IntegerType>(OrigRetTy)->getBitWidth();
                        if (W < 64)
                            ResultReplacement = Builder.CreateTrunc(
                                ProxyPollingCall, OrigRetTy, "result.trunc");
                        else
                            ResultReplacement = ProxyPollingCall;

                    } else {
                        ResultReplacement = UndefValue::get(OrigRetTy);
                    }

                    Use.CB->replaceAllUsesWith(ResultReplacement);  // ✅ now fires with correct type
                }
                // void return → no uses to replace
            }

            if (!finishReplaCBngCallBase(Use.CB)) {
                errs() << "[api-async] failed to replace callbase\n";
                continue;
            }
        }
    }

    errs() << "[api_hashing] resolved "
           << Records.size()
           << " API pointers at main, replaced "
           << Uses.size()
           << " call sites\n";

    return PreservedAnalyses::none();
}


llvm::StringRef llvm::ApiHashingPass::getLibraryForFunction(StringRef Name) {
    static const llvm::StringMap<StringRef> ApiToDll = {
        {"WSAStartup", "ws2_32.dll"},
        {"WSACleanup", "ws2_32.dll"},
        {"socket", "ws2_32.dll"},
        {"connect", "ws2_32.dll"},
        {"send", "ws2_32.dll"},
        {"recv", "ws2_32.dll"},
        {"closesocket", "ws2_32.dll"},
        {"WSASocketA", "ws2_32.dll"},
        {"htons", "ws2_32.dll"},
        {"inet_addr", "ws2_32.dll"},


        {"MessageBoxA", "user32.dll"},
        {"MessageBoxW", "user32.dll"},

        {"RegOpenKeyExA", "advapi32.dll"},
        {"RegOpenKeyExW", "advapi32.dll"},
        {"RegSetValueExA", "advapi32.dll"},
        {"RegSetValueExW", "advapi32.dll"},
        {"RegCloseKey", "advapi32.dll"},

        {"CreateFile", "kernel32.dll"},
        {"CreateFileA", "kernel32.dll"},
        {"CreateFileW", "kernel32.dll"},
        {"ReadFile", "kernel32.dll"},
        {"WriteFile", "kernel32.dll"},
        {"CloseHandle", "kernel32.dll"},
        {"VirtualAlloc", "kernel32.dll"},
        {"VirtualFree", "kernel32.dll"},
        {"LoadLibraryW", "kernel32.dll"},
        {"GetProcAddress", "kernel32.dll"},
        {"CreateProcessA", "kernel32.dll"},
        {"ExitProcess", "kernel32.dll"},
        {"CreatePipe", "kernel32.dll"},
        {"SetHandleInformation", "kernel32.dll"},
        {"PeekNamedPipe", "kernel32.dll"},
        {"Process32First", "kernel32.dll"},
        {"Process32Next", "kernel32.dll"},
        {"CreateToolhelp32Snapshot", "kernel32.dll"},

        {"CryptBinaryToStringA", "crypt32.dll"},

        {"WinHttpOpen", "winhttp.dll"},
        {"WinHttpConnect", "winhttp.dll"},
        {"WinHttpCloseHandle", "winhttp.dll"},
        {"WinHttpOpenRequest", "winhttp.dll"},
        {"WinHttpSetOption", "winhttp.dll"},
        {"WinHttpSendRequest", "winhttp.dll"},
        {"WinHttpReceiveResponse", "winhttp.dll"},
        {"WinHttpQueryDataAvailable", "winhttp.dll"},
        {"WinHttpReadData", "winhttp.dll"},

        {"LsaClose", "advapi32.dll"},
        {"LsaEnumerateTrustedDomainsEx", "advapi32.dll"},
        {"LsaFreeMemory", "advapi32.dll"},
        {"LsaOpenPolicy", "advapi32.dll"},
        {"LsaOpenSecret", "advapi32.dll"},
        {"LsaQueryInformationPolicy", "advapi32.dll"},
        {"LsaQuerySecret", "advapi32.dll"},
        {"LsaQueryTrustedDomainInfoByName", "advapi32.dll"},
        {"LsaRetrievePrivateData", "advapi32.dll"},
        {"LsaSetSecret", "advapi32.dll"},

        {"LsaCallAuthenticationPackage", "secur32.dll"},
        {"LsaConnectUntrusted", "secur32.dll"},
        {"LsaDeregisterLogonProcess", "secur32.dll"},
        {"LsaFreeReturnBuffer", "secur32.dll"},
        {"LsaLookupAuthenticationPackage", "secur32.dll"},

        {"SCardEstablishContext", "winscard.dll"},
        {"SCardReleaseContext", "winscard.dll"},
        {"LocalFree", "kernel32.dll"},
        {"CreateThread", "kernel32.dll"},
        {"GetKeyboardLayout", "user32.dll"},
        {"LocalAlloc", "kernel32.dll"},
        {"OpenProcess", "kernel32.dll"},
        {"GetCurrentProcessId", "kernel32.dll"},
        {"CryptEnumProvidersW", "advapi32.dll"},
        {"CryptAcquireContextW", "advapi32.dll"},
        {"CryptReleaseContext", "advapi32.dll"},
        {"GetLastError", "kernel32.dll"},
        {"CryptEnumProviderTypesW", "advapi32.dll"},
        {"BCryptEnumRegisteredProviders", "bcrypt.dll"},
        {"BCryptFreeBuffer", "bcrypt.dll"},
        {"CertEnumSystemStore", "crypt32.dll"},
        {"CertOpenStore", "crypt32.dll"},
        {"CertEnumCertificatesInStore", "crypt32.dll"},
        {"CertGetNameStringW", "crypt32.dll"},
        {"CertGetCertificateContextProperty", "crypt32.dll"},
        {"CryptAcquireCertificatePrivateKey", "crypt32.dll"},
        {"CryptGetUserKey", "advapi32.dll"},
        {"CryptDestroyKey", "advapi32.dll"},
        {"NCryptFreeObject", "ncrypt.dll"},
        {"CertCloseStore", "crypt32.dll"},
        {"RtlInitUnicodeString", "ntdll.dll"},
        {"SystemFunction007", "advapi32.dll"},
        {"RtlUpcaseUnicodeStringToOemString", "ntdll.dll"},
        {"SystemFunction006", "advapi32.dll"},
        {"RtlFreeOemString", "ntdll.dll"},
        {"PathIsDirectoryW", "shlwapi.dll"},
        {"PathFindFileNameW", "shlwapi.dll"},
        {"CertFindCertificateInStore", "crypt32.dll"},
        {"CryptExportKey", "advapi32.dll"},
        {"NCryptOpenStorageProvider", "ncrypt.dll"},
        {"NCryptOpenKey", "ncrypt.dll"},
        {"NCryptExportKey", "ncrypt.dll"},
        {"CryptSetProvParam", "advapi32.dll"},
        {"CryptImportKey", "advapi32.dll"},
        {"CryptSetKeyParam", "advapi32.dll"},
        {"CertFreeCertificateContext", "crypt32.dll"},
        {"CertSetCertificateContextProperty", "crypt32.dll"},
        {"NCryptGetProperty", "ncrypt.dll"},
        {"CryptStringToBinaryA", "crypt32.dll"},
        {"CryptDecodeObjectEx", "crypt32.dll"},
        {"CryptGetKeyParam", "advapi32.dll"},
        {"lstrlenA", "kernel32.dll"},
        {"CryptQueryObject", "crypt32.dll"},
        {"CryptFindOIDInfo", "crypt32.dll"},
        {"CryptGetProvParam", "advapi32.dll"},
        {"NCryptEnumKeys", "ncrypt.dll"},
        {"NCryptFreeBuffer", "ncrypt.dll"},
        {"CertAddCertificateContextToStore", "crypt32.dll"},
        {"FreeLibrary", "kernel32.dll"},
        {"CryptEncodeObject", "crypt32.dll"},
        {"A_SHAInit", "advapi32.dll"},
        {"A_SHAUpdate", "advapi32.dll"},
        {"A_SHAFinal", "advapi32.dll"},
        {"lstrlenW", "kernel32.dll"},
        {"CryptExportPublicKeyInfo", "crypt32.dll"},
        {"CDGenerateRandomBits", "cryptdll.dll"},
        {"CryptSignAndEncodeCertificate", "crypt32.dll"},
        {"CertNameToStrW", "crypt32.dll"},
        {"GetSystemTimeAsFileTime", "kernel32.dll"},
        {"CryptGenKey", "advapi32.dll"},
        {"SCardControl", "winscard.dll"},
        {"SCardConnectW", "winscard.dll"},
        {"SCardGetAttrib", "winscard.dll"},
        {"RtlEqualString", "ntdll.dll"},
        {"SCardFreeMemory", "winscard.dll"},
        {"SCardDisconnect", "winscard.dll"},
        {"SCardListReadersW", "winscard.dll"},
        {"SCardListCardsW", "winscard.dll"},
        {"SCardGetCardTypeProviderNameW", "winscard.dll"},
        {"BCryptOpenAlgorithmProvider", "bcrypt.dll"},
        {"BCryptSetProperty", "bcrypt.dll"},
        {"BCryptGenerateSymmetricKey", "bcrypt.dll"},
        {"BCryptDecrypt", "bcrypt.dll"},
        {"BCryptCloseAlgorithmProvider", "bcrypt.dll"},
        {"BCryptDestroyKey", "bcrypt.dll"},
        {"UrlUnescapeW", "shlwapi.dll"},
        {"SysFreeString", "oleaut32.dll"},
        {"CryptDecrypt", "advapi32.dll"},
        {"CryptProtectData", "crypt32.dll"},
        {"RtlGUIDFromString", "ntdll.dll"},
        {"ConvertStringSidToSidW", "advapi32.dll"},
        {"ConvertSidToStringSidW", "advapi32.dll"},
        {"RtlStringFromGUID", "ntdll.dll"},
        {"SetFileAttributesW", "kernel32.dll"},
        {"MD5Init", "advapi32.dll"},
        {"MD5Update", "advapi32.dll"},
        {"MD5Final", "advapi32.dll"},
        {"IsValidSid", "advapi32.dll"},
        {"GetSidSubAuthorityCount", "advapi32.dll"},
        {"GetSidSubAuthority", "advapi32.dll"},
        {"CoCreateInstance", "ole32.dll"},
        {"CoSetProxyBlanket", "ole32.dll"},
        {"VariantClear", "oleaut32.dll"},
        {"GetTokenInformation", "advapi32.dll"},
        {"DuplicateTokenEx", "advapi32.dll"},
        {"SetThreadToken", "advapi32.dll"},
        {"BCryptImportKeyPair", "bcrypt.dll"},
        {"BCryptExportKey", "bcrypt.dll"},
        {"OpenEventLogW", "advapi32.dll"},
        {"GetNumberOfEventLogRecords", "advapi32.dll"},
        {"ClearEventLogW", "advapi32.dll"},
        {"RtlEqualUnicodeString", "ntdll.dll"},
        {"ber_bvfree", "wldap32.dll"},
        {"RtlAnsiStringToUnicodeString", "ntdll.dll"},
        {"CDLocateCSystem", "cryptdll.dll"},
        {"RtlUpcaseUnicodeString", "ntdll.dll"},
        {"RtlAppendUnicodeStringToString", "ntdll.dll"},
        {"CDLocateCheckSum", "cryptdll.dll"},
        {"ber_alloc_t", "wldap32.dll"},
        {"ber_printf", "wldap32.dll"},
        {"ber_flatten", "wldap32.dll"},
        {"ber_free", "wldap32.dll"},
        {"CommandLineToArgvW", "shell32.dll"},
        {"ldap_unbind", "wldap32.dll"},
        {"ldap_search_sW", "wldap32.dll"},
        {"ldap_count_entries", "wldap32.dll"},
        {"ldap_first_entry", "wldap32.dll"},
        {"ldap_get_dnW", "wldap32.dll"},
        {"ldap_get_values_lenW", "wldap32.dll"},
        {"ldap_msgfree", "wldap32.dll"},
        {"SystemFunction027", "advapi32.dll"},
        {"UuidCreate", "rpcrt4.dll"},
        {"RtlFreeUnicodeString", "ntdll.dll"},
        {"ldap_count_values_len", "wldap32.dll"},
        {"ldap_value_free_len", "wldap32.dll"},
        {"ldap_get_valuesW", "wldap32.dll"},
        {"ldap_count_valuesW", "wldap32.dll"},
        {"ldap_value_freeW", "wldap32.dll"},
        {"ldap_get_valuesA", "wldap32.dll"},
        {"ldap_count_valuesA", "wldap32.dll"},
        {"ldap_value_freeA", "wldap32.dll"},
        {"ldap_memfreeW", "wldap32.dll"},
        {"ldap_explode_dnW", "wldap32.dll"},
        {"ldap_search_ext_sW", "wldap32.dll"},
        {"ldap_get_optionW", "wldap32.dll"},
        {"ldap_initW", "wldap32.dll"},
        {"ldap_set_optionW", "wldap32.dll"},
        {"ldap_connect", "wldap32.dll"},
        {"ldap_bind_sW", "wldap32.dll"},
        {"ldap_modify_sW", "wldap32.dll"},
        {"ldap_next_entry", "wldap32.dll"},
        {"GetLengthSid", "advapi32.dll"},
        {"ConvertStringSecurityDescriptorToSecurityDescriptorW", "advapi32.dll"},
        {"SystemFunction026", "advapi32.dll"},
        {"ldap_add_sW", "wldap32.dll"},
        {"ldap_delete_sW", "wldap32.dll"},
        {"SetEvent", "kernel32.dll"},
        {"GetComputerNameW", "kernel32.dll"},
        {"CreateEventW", "kernel32.dll"},
        {"SetConsoleCtrlHandler", "kernel32.dll"},
        {"WaitForSingleObject", "kernel32.dll"},
        {"I_RpcGetCurrentCallHandle", "rpcrt4.dll"},
        {"I_RpcBindingInqSecurityContext", "rpcrt4.dll"},
        {"QueryContextAttributesW", "secur32.dll"},
        {"FreeContextBuffer", "secur32.dll"},
        {"SamConnect", "samlib.dll"},
        {"SamOpenDomain", "samlib.dll"},
        {"SamLookupIdsInDomain", "samlib.dll"},
        {"SamFreeMemory", "samlib.dll"},
        {"SamLookupNamesInDomain", "samlib.dll"},
        {"SamEnumerateUsersInDomain", "samlib.dll"},
        {"SamCloseHandle", "samlib.dll"},
        {"I_NetServerReqChallenge", "netapi32.dll"},
        {"I_NetServerAuthenticate2", "netapi32.dll"},
        {"I_NetServerTrustPasswordsGet", "netapi32.dll"},
        {"SystemFunction023", "advapi32.dll"},
        {"EnumerateSecurityPackagesW", "secur32.dll"},
        {"AcquireCredentialsHandleW", "secur32.dll"},
        {"InitializeSecurityContextW", "secur32.dll"},
        {"DeleteSecurityContext", "secur32.dll"},
        {"FreeCredentialsHandle", "secur32.dll"},
        {"RpcEpResolveBinding", "rpcrt4.dll"},
        {"SamSetInformationUser", "samlib.dll"},
        {"SamEnumerateDomainsInSamServer", "samlib.dll"},
        {"SamLookupDomainInSamServer", "samlib.dll"},
        {"SamOpenUser", "samlib.dll"},
        {"SystemFunction001", "advapi32.dll"},
        {"SamiChangePasswordUser", "samlib.dll"},
        {"SamQueryInformationUser", "samlib.dll"},
        {"SystemFunction033", "advapi32.dll"},
        {"CryptCreateHash", "advapi32.dll"},
        {"CryptHashData", "advapi32.dll"},
        {"CryptGetHashParam", "advapi32.dll"},
        {"CryptDestroyHash", "advapi32.dll"},
        {"SystemFunction032", "advapi32.dll"},
        {"SystemFunction005", "advapi32.dll"},
        {"CryptSignHashW", "advapi32.dll"},
        {"CryptDeriveKey", "advapi32.dll"},
        {"GetCurrentProcess", "kernel32.dll"},
        {"DuplicateHandle", "kernel32.dll"},
        {"MapViewOfFile", "kernel32.dll"},
        {"SystemFunction041", "advapi32.dll"},
        {"UnmapViewOfFile", "kernel32.dll"},
        {"GetModuleHandleW", "kernel32.dll"},
        {"FilterFindFirst", "fltlib.dll"},
        {"FilterFindNext", "fltlib.dll"},
        {"RegisterClassExW", "user32.dll"},
        {"CreateWindowExW", "user32.dll"},
        {"SetClipboardViewer", "user32.dll"},
        {"GetMessageW", "user32.dll"},
        {"TranslateMessage", "user32.dll"},
        {"DispatchMessageW", "user32.dll"},
        {"ChangeClipboardChain", "user32.dll"},
        {"DestroyWindow", "user32.dll"},
        {"UnregisterClassW", "user32.dll"},
        {"CoTaskMemFree", "ole32.dll"},
        {"WNetCancelConnection2W", "mpr.dll"},
        {"WNetAddConnection2W", "mpr.dll"},
        {"RpcBindingSetObject", "rpcrt4.dll"},
        {"SQLAllocHandle", "odbc32.dll"},
        {"SQLSetEnvAttr", "odbc32.dll"},
        {"SQLDriverConnectW", "odbc32.dll"},
        {"SQLExecDirectW", "odbc32.dll"},
        {"SQLFetch", "odbc32.dll"},
        {"SQLGetData", "odbc32.dll"},
        {"SQLFreeHandle", "odbc32.dll"},
        {"SQLDisconnect", "odbc32.dll"},
        {"NtOpenDirectoryObject", "ntdll.dll"},
        {"NtQueryDirectoryObject", "ntdll.dll"},
        {"GetFileAttributesExW", "kernel32.dll"},
        {"SendMessageW", "user32.dll"},
        {"GetClipboardSequenceNumber", "user32.dll"},
        {"OpenClipboard", "user32.dll"},
        {"EnumClipboardFormats", "user32.dll"},
        {"GetClipboardData", "user32.dll"},
        {"GlobalSize", "kernel32.dll"},
        {"CloseClipboard", "user32.dll"},
        {"DefWindowProcW", "user32.dll"},
        {"PostMessageW", "user32.dll"},
        {"NtResumeProcess", "ntdll.dll"},
        {"SamGetGroupsForUser", "samlib.dll"},
        {"SamRidToSid", "samlib.dll"},
        {"SamGetAliasMembership", "samlib.dll"},
        {"SamEnumerateGroupsInDomain", "samlib.dll"},
        {"SamOpenGroup", "samlib.dll"},
        {"SamGetMembersInGroup", "samlib.dll"},
        {"SamEnumerateAliasesInDomain", "samlib.dll"},
        {"SamOpenAlias", "samlib.dll"},
        {"SamGetMembersInAlias", "samlib.dll"},
        {"NetSessionEnum", "netapi32.dll"},
        {"NetApiBufferFree", "netapi32.dll"},
        {"NetWkstaUserEnum", "netapi32.dll"},
        {"NetRemoteTOD", "netapi32.dll"},
        {"SystemTimeToFileTime", "kernel32.dll"},
        {"NetStatisticsGet", "netapi32.dll"},
        {"NetShareEnum", "netapi32.dll"},
        {"NetServerGetInfo", "netapi32.dll"},
        {"DsEnumerateDomainTrustsW", "netapi32.dll"},
        {"ldap_first_attributeW", "wldap32.dll"},
        {"ldap_next_attributeW", "wldap32.dll"},
        {"DnsQuery_A", "dnsapi.dll"},
        {"DnsFree", "dnsapi.dll"},
        {"RpcBindingSetAuthInfoW", "rpcrt4.dll"},
        {"NCryptSetProperty", "ncrypt.dll"},
        {"NCryptSignHash", "ncrypt.dll"},
        {"GetFileAttributesW", "kernel32.dll"},
        {"LookupPrivilegeValueW", "advapi32.dll"},
        {"RtlAdjustPrivilege", "ntdll.dll"},
        {"CreateProcessW", "kernel32.dll"},
        {"OpenProcessToken", "advapi32.dll"},
        {"CreateEnvironmentBlock", "userenv.dll"},
        {"CreateProcessAsUserW", "advapi32.dll"},
        {"DestroyEnvironmentBlock", "userenv.dll"},
        {"NtTerminateProcess", "ntdll.dll"},
        {"NtSuspendProcess", "ntdll.dll"},
        {"RpcMgmtStopServerListening", "rpcrt4.dll"},
        {"RpcMgmtEpEltInqBegin", "rpcrt4.dll"},
        {"RpcMgmtEpEltInqNextW", "rpcrt4.dll"},
        {"RpcStringFreeW", "rpcrt4.dll"},
        {"RpcBindingToStringBindingW", "rpcrt4.dll"},
        {"RpcBindingFree", "rpcrt4.dll"},
        {"RpcMgmtEpEltInqDone", "rpcrt4.dll"},
        {"RpcServerUseProtseqEpW", "rpcrt4.dll"},
        {"RpcServerRegisterAuthInfoW", "rpcrt4.dll"},
        {"RpcServerRegisterIf2", "rpcrt4.dll"},
        {"RpcServerInqBindings", "rpcrt4.dll"},
        {"RpcEpRegisterW", "rpcrt4.dll"},
        {"RpcServerListen", "rpcrt4.dll"},
        {"RpcEpUnregister", "rpcrt4.dll"},
        {"RpcBindingVectorFree", "rpcrt4.dll"},
        {"RpcServerUnregisterIfEx", "rpcrt4.dll"},
        {"InitializeCriticalSection", "kernel32.dll"},
        {"DeleteCriticalSection", "kernel32.dll"},
        {"EnterCriticalSection", "kernel32.dll"},
        {"LeaveCriticalSection", "kernel32.dll"},
        {"NtQueryObject", "ntdll.dll"},
        {"GetProcessId", "kernel32.dll"},
        {"CredIsMarshaledCredentialW", "advapi32.dll"},
        {"CredUnmarshalCredentialW", "advapi32.dll"},
        {"CredFree", "advapi32.dll"},
        {"BCryptGetProperty", "bcrypt.dll"},
        {"GetFileSizeEx", "kernel32.dll"},
        {"SetFilePointerEx", "kernel32.dll"},
        {"StartServiceCtrlDispatcherW", "advapi32.dll"},
        {"RegisterServiceCtrlHandlerW", "advapi32.dll"},
        {"SetServiceStatus", "advapi32.dll"},
        {"GetStdHandle", "kernel32.dll"},
        {"GetConsoleScreenBufferInfo", "kernel32.dll"},
        {"FillConsoleOutputCharacterW", "kernel32.dll"},
        {"SetConsoleCursorPosition", "kernel32.dll"},
        {"GetFileVersionInfoSizeW", "version.dll"},
        {"GetFileVersionInfoW", "version.dll"},
        {"VerQueryValueW", "version.dll"},
        {"GetSystemDirectoryW", "kernel32.dll"},
        {"PathCombineW", "shlwapi.dll"},
        {"SetCurrentDirectoryW", "kernel32.dll"},
        {"GetTimeZoneInformation", "kernel32.dll"},
        {"NtEnumerateSystemEnvironmentValuesEx", "ntdll.dll"},
        {"NtQuerySystemEnvironmentValueEx", "ntdll.dll"},
        {"NtSetSystemEnvironmentValueEx", "ntdll.dll"},
        {"GetCurrentThread", "kernel32.dll"},
        {"OpenThreadToken", "advapi32.dll"},
        {"EqualSid", "advapi32.dll"},
        {"LookupPrivilegeNameW", "advapi32.dll"},
        {"ProcessIdToSessionId", "kernel32.dll"},
        {"WinStationOpenServerW", "winsta.dll"},
        {"WinStationEnumerateW", "winsta.dll"},
        {"WinStationQueryInformationW", "winsta.dll"},
        {"RtlIpv4AddressToStringW", "ntdll.dll"},
        {"RtlIpv6AddressToStringW", "ntdll.dll"},
        {"WinStationFreeMemory", "winsta.dll"},
        {"WinStationCloseServer", "winsta.dll"},
        {"WinStationConnectW", "winsta.dll"},
        {"IsWow64Process", "kernel32.dll"},
        {"CredEnumerateW", "advapi32.dll"},
        {"CryptUnprotectData", "crypt32.dll"},
        {"RegQueryValueExW", "advapi32.dll"},
        {"SCardTransmit", "winscard.dll"},
        {"FileTimeToSystemTime", "kernel32.dll"},
        {"RtlUnicodeStringToAnsiString", "ntdll.dll"},
        {"RtlFreeAnsiString", "ntdll.dll"},
        {"ASN1_CreateModule", "msasn1.dll"},
        {"ASN1_CreateEncoder", "msasn1.dll"},
        {"ASN1_CreateDecoder", "msasn1.dll"},
        {"ASN1_CloseDecoder", "msasn1.dll"},
        {"ASN1_CloseEncoder", "msasn1.dll"},
        {"ASN1_CloseModule", "msasn1.dll"},
        {"ASN1BERDotVal2Eoid", "msasn1.dll"},
        {"ASN1_FreeEncoded", "msasn1.dll"},
        {"ASN1BEREoid2DotVal", "msasn1.dll"},
        {"ASN1Free", "msasn1.dll"},
        {"HidD_GetHidGuid", "hid.dll"},
        {"SetupDiGetClassDevsW", "setupapi.dll"},
        {"SetupDiEnumDeviceInterfaces", "setupapi.dll"},
        {"SetupDiGetDeviceInterfaceDetailW", "setupapi.dll"},
        {"HidD_GetAttributes", "hid.dll"},
        {"HidD_GetPreparsedData", "hid.dll"},
        {"HidP_GetCaps", "hid.dll"},
        {"HidD_FreePreparsedData", "hid.dll"},
        {"SetupDiDestroyDeviceInfoList", "setupapi.dll"},
        {"TerminateThread", "kernel32.dll"},
        {"SetFilePointer", "kernel32.dll"},
        {"DeleteFileA", "kernel32.dll"},
        {"GetTempPathA", "kernel32.dll"},
        {"GetTempFileNameA", "kernel32.dll"},
        {"GetFileInformationByHandle", "kernel32.dll"},
        {"FileTimeToLocalFileTime", "kernel32.dll"},
        {"FileTimeToDosDateTime", "kernel32.dll"},
        {"GetCurrentDirectoryA", "kernel32.dll"},
        {"FCICreate", "cabinet.dll"},
        {"FCIAddFile", "cabinet.dll"},
        {"FCIFlushCabinet", "cabinet.dll"},
        {"FCIDestroy", "cabinet.dll"},
        {"CryptAcquireContextA", "advapi32.dll"},
        {"CryptSetHashParam", "advapi32.dll"},
        {"CryptEncrypt", "advapi32.dll"},
        {"CryptDuplicateKey", "advapi32.dll"},
        {"RtlDowncaseUnicodeString", "ntdll.dll"},
        {"PFXExportCertStoreEx", "crypt32.dll"},
        {"CertAddEncodedCertificateToStore", "crypt32.dll"},
        {"NCryptImportKey", "ncrypt.dll"},
        {"NCryptFinalizeKey", "ncrypt.dll"},
        {"CryptStringToBinaryW", "crypt32.dll"},
        {"BCryptKeyDerivation", "bcrypt.dll"},
        {"BCryptCreateHash", "bcrypt.dll"},
        {"BCryptHashData", "bcrypt.dll"},
        {"BCryptFinishHash", "bcrypt.dll"},
        {"BCryptDestroyHash", "bcrypt.dll"},
        {"NCryptDecrypt", "ncrypt.dll"},
        {"BCryptDeriveKeyPBKDF2", "bcrypt.dll"},
        {"CopySid", "advapi32.dll"},
        {"GetCurrentDirectoryW", "kernel32.dll"},
        {"PathIsRelativeW", "shlwapi.dll"},
        {"PathCanonicalizeW", "shlwapi.dll"},
        {"CryptBinaryToStringW", "crypt32.dll"},
        {"FlushFileBuffers", "kernel32.dll"},
        {"ExpandEnvironmentStringsW", "kernel32.dll"},
        {"FindFirstFileW", "kernel32.dll"},
        {"FindNextFileW", "kernel32.dll"},
        {"FindClose", "kernel32.dll"},
        {"GetUserObjectInformationW", "user32.dll"},
        {"DeviceIoControl", "kernel32.dll"},
        {"ReadProcessMemory", "kernel32.dll"},
        {"WriteProcessMemory", "kernel32.dll"},
        {"VirtualAllocEx", "kernel32.dll"},
        {"SetLastError", "kernel32.dll"},
        {"VirtualFreeEx", "kernel32.dll"},
        {"VirtualQuery", "kernel32.dll"},
        {"VirtualQueryEx", "kernel32.dll"},
        {"VirtualProtect", "kernel32.dll"},
        {"VirtualProtectEx", "kernel32.dll"},
        {"RtlGetCompressionWorkSpaceSize", "ntdll.dll"},
        {"RtlCompressBuffer", "ntdll.dll"},
        {"RtlDecompressBuffer", "ntdll.dll"},
        {"CreateFileMappingW", "kernel32.dll"},
        {"CreateWellKnownSid", "advapi32.dll"},
        {"DsGetDcNameW", "netapi32.dll"},
        {"GetComputerNameExW", "kernel32.dll"},
        {"GetConsoleOutputCP", "kernel32.dll"},
        {"SetConsoleOutputCP", "kernel32.dll"},
        {"CreateNamedPipeW", "kernel32.dll"},
        {"ConnectNamedPipe", "kernel32.dll"},
        {"WaitNamedPipeW", "kernel32.dll"},
        {"SetNamedPipeHandleState", "kernel32.dll"},
        {"GetNamedPipeInfo", "kernel32.dll"},
        {"DisconnectNamedPipe", "kernel32.dll"},
        {"NtQuerySystemInformation", "ntdll.dll"},
        {"NtQueryInformationProcess", "ntdll.dll"},
        {"CreateProcessWithLogonW", "advapi32.dll"},
        {"HidD_SetFeature", "hid.dll"},
        {"HidD_GetFeature", "hid.dll"},
        {"RegQueryInfoKeyW", "advapi32.dll"},
        {"RegEnumKeyExW", "advapi32.dll"},
        {"RegEnumValueW", "advapi32.dll"},
        {"RtlCreateUserThread", "ntdll.dll"},
        {"CreateRemoteThread", "kernel32.dll"},
        {"NdrMesTypeAlignSize2", "rpcrt4.dll"},
        {"NdrMesTypeEncode2", "rpcrt4.dll"},
        {"NdrMesTypeDecode2", "rpcrt4.dll"},
        {"NdrMesTypeFree2", "rpcrt4.dll"},
        {"UuidToStringW", "rpcrt4.dll"},
        {"RpcMgmtWaitServerListen", "rpcrt4.dll"},
        {"RpcStringBindingComposeW", "rpcrt4.dll"},
        {"RpcBindingFromStringBindingW", "rpcrt4.dll"},
        {"RpcBindingSetAuthInfoExW", "rpcrt4.dll"},
        {"RpcBindingSetOption", "rpcrt4.dll"},
        {"RpcBindingInqAuthClientW", "rpcrt4.dll"},
        {"RpcImpersonateClient", "rpcrt4.dll"},
        {"RpcRevertToSelf", "rpcrt4.dll"},
        {"MesDecodeIncrementalHandleCreate", "rpcrt4.dll"},
        {"MesIncrementalHandleReset", "rpcrt4.dll"},
        {"MesHandleFree", "rpcrt4.dll"},
        {"MesEncodeIncrementalHandleCreate", "rpcrt4.dll"},
        {"NdrClientCall2", "rpcrt4.dll"},
        {"OpenSCManagerW", "advapi32.dll"},
        {"OpenServiceW", "advapi32.dll"},
        {"QueryServiceStatusEx", "advapi32.dll"},
        {"CloseServiceHandle", "advapi32.dll"},
        {"StartServiceW", "advapi32.dll"},
        {"DeleteService", "advapi32.dll"},
        {"ControlService", "advapi32.dll"},
        {"QueryServiceObjectSecurity", "advapi32.dll"},
        {"AllocateAndInitializeSid", "advapi32.dll"},
        {"BuildSecurityDescriptorW", "advapi32.dll"},
        {"SetServiceObjectSecurity", "advapi32.dll"},
        {"FreeSid", "advapi32.dll"},
        {"CreateServiceW", "advapi32.dll"},
        {"PurgeComm", "kernel32.dll"},
        {"ClearCommError", "kernel32.dll"},
        {"IsCharAlphaNumericW", "user32.dll"},
        {"IsTextUnicode", "advapi32.dll"},
        {"WideCharToMultiByte", "kernel32.dll"},
        {"GetDateFormatW", "kernel32.dll"},
        {"GetTimeFormatW", "kernel32.dll"},
        {"LookupAccountSidW", "advapi32.dll"},
        {"CheckTokenMembership", "advapi32.dll"},
        {"LookupAccountNameW", "advapi32.dll"},
        {"NtCompareTokens", "ntdll.dll"},
        {"SysAllocString", "oleaut32.dll"},
        {"VariantInit", "oleaut32.dll"},
        {"RaiseException", "kernel32.dll"},
        {"SetConsoleTitleW", "kernel32.dll"},
        {"ExitThread", "kernel32.dll"},
        {"RtlGetNtVersionNumbers", "ntdll.dll"},
        {"CoInitializeEx", "ole32.dll"},
        {"CoUninitialize", "ole32.dll"},
        {"GetCurrentThreadId", "kernel32.dll"},
        {"TryEnterCriticalSection", "kernel32.dll"},
        {"MiniDumpWriteDump", "dbghelp.dll"},
        {"AdjustTokenPrivileges", "advapi32.dll"},
        {"ImpersonateLoggedOnUser", "advapi32.dll"},
        {"CreateProcessWithTokenW",    "advapi32.dll"}

    };

    auto It = ApiToDll.find(Name);
    if (It != ApiToDll.end())
        return It->second;

    return "";
}

bool llvm::ApiHashingPass::finishReplaCBngCallBase(CallBase *CB) {
    if (!CB)
        return false;

    // CRITICAL: replace all uses of the return value BEFORE erasing.
    // Any instruction using the call result (e.g. `icmp ne i32 %ret, 0`)
    // would hold a dangling reference after erase, causing verifyFunction
    // to crash with a null getFunction() (segfault at Instruction::getFunction+4).
    // The async dispatch makes the original return value meaningless anyway.
    if (!CB->getType()->isVoidTy())
        CB->replaceAllUsesWith(UndefValue::get(CB->getType()));

    if (auto *Call = dyn_cast<CallInst>(CB)) {
        Call->eraseFromParent();
        return true;
    }

    if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
        BasicBlock *NormalDest = Invoke->getNormalDest();
        BasicBlock *UnwindDest = Invoke->getUnwindDest();

        UnwindDest->removePredecessor(Invoke->getParent());
        BranchInst::Create(NormalDest, Invoke);
        Invoke->eraseFromParent();

        // If the unwind block has no remaining invoke predecessors,
        // its landingpad instruction is now illegal. Neutralise it.
        bool HasInvokePred = llvm::any_of(
            llvm::predecessors(UnwindDest),
            [](BasicBlock *P) { return isa<InvokeInst>(P->getTerminator()); }
        );
        if (!HasInvokePred) {
            for (Instruction &I : llvm::make_early_inc_range(*UnwindDest)) {
                if (I.isTerminator()) break;
                if (!I.getType()->isVoidTy())
                    I.replaceAllUsesWith(UndefValue::get(I.getType()));
                I.eraseFromParent();
            }
            UnwindDest->getTerminator()->eraseFromParent();
            new UnreachableInst(UnwindDest->getContext(), UnwindDest);
        }
        return true;
    }

    errs() << "[api-async] unsupported CallBase kind\n";
    return false;
}

bool llvm::ApiHashingPass::shouldSkipDllForThreadPool(std::string ApiName) {
    std::vector<std::string> skipList = {"WinHttpOpen", 
                                        "WinHttpConnect", 
                                        "WinHttpOpenRequest"};
    
    // Method 1: Using std::find
    if (std::find(skipList.begin(), skipList.end(), ApiName) != skipList.end()) {
        return true;
    }
    
    // Method 2: Using C++20 contains (if you have it)
    // if (std::ranges::find(skipList, Dll) != skipList.end()) {
    //     return true;
    // }
    
    return false;
}