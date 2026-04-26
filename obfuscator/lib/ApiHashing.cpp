#include "ApiHashing.h"

static llvm::cl::opt<std::string> os_version(
    "os",
    llvm::cl::desc("only supported -os windows"),
    llvm::cl::value_desc("os version"),
    llvm::cl::Optional);

llvm::PreservedAnalyses llvm::ApiHashingPass::run(Function &F, FunctionAnalysisManager &AM) {

    if (os_version!="windows") {
    errs()<<"os version can only be -os windows";
    return PreservedAnalyses::all();
    }

    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();

    bool Changed = false;

    Type *Int32Ty = Type::getInt32Ty(Ctx);
Type *PtrTy = PointerType::getUnqual(Ctx);

FunctionType *ResolverType = FunctionType::get(PtrTy, {PtrTy, Int32Ty}, false);

FunctionCallee ResolverFunc = M->getOrInsertFunction("getFunctionAddressByHash", ResolverType);

    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            auto *CB = dyn_cast<CallInst>(&I);
            if (!CB)
                continue;

            Function *Called = CB->getCalledFunction();
            if (!Called || !Called->hasName())
                continue;
            
            if (!Called->isDeclaration())
                continue;

            StringRef Name = Called->getName();
/*
            if (Name == "getFunctionAddressByHash" ||
                Name == "crc32" ||
                Name == "printf" ||
                Name == "strlen") {
                continue;
            }
*/
            uint32_t HashVal = llvm::crc32(Name.str().data(), Name.size());

            IRBuilder<> Builder(CB);
            StringRef library = getLibraryForFunction(Name);
            if (library == "")
                continue;

            outs() << Name;
            Value *LibraryName = Builder.CreateGlobalStringPtr(library, "libname");
            Value *HashArg = ConstantInt::get(Int32Ty, HashVal);
            Value *ResolvedPtr = Builder.CreateCall(ResolverFunc, {LibraryName,HashArg}, "resolved_api"); 

            CB->setCalledOperand(ResolvedPtr);

            Changed = true;
        }
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
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
        {"CloseHandle", "kernel32.dll"},
        {"PeekNamedPipe", "kernel32.dll"},
        {"ReadFile", "kernel32.dll"},
        {"WriteFile", "kernel32.dll"},

        {"CryptBinaryToStringA", "crypt32.dll"},

        {"WinHttpOpen", "winhttp.dll"},
        {"WinHttpConnect", "winhttp.dll"},
        {"WinHttpCloseHandle", "winhttp.dll"},
        {"WinHttpOpenRequest", "winhttp.dll"},
        {"WinHttpSetOption", "winhttp.dll"},
        {"WinHttpSendRequest", "winhttp.dll"},
        {"WinHttpSendRequest", "winhttp.dll"},
        {"WinHttpReceiveResponse", "winhttp.dll"},
        {"WinHttpQueryDataAvailable", "winhttp.dll"},
        {"WinHttpReadData", "winhttp.dll"}
    };

    auto It = ApiToDll.find(Name);
    if (It != ApiToDll.end())
        return It->second;

    return "";
}