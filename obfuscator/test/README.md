
# Automated build for a test case
```bash
cd build_test
cmake ..
cmake --build .
cmake --build . --target run_test
```

# Build reverse shell for Windows
```bash
../../llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -Wimplicit-function-declaration -emit-llvm -S windows.c -o windows.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu
../../llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S WindowsTools/APIResolver.cpp -o WindowsTools/APIResolver.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu
../../llvm-project/build/bin/llvm-link WindowsTools/APIResolver.ll windows.ll -S -o hack.ll
../../llvm-project/build/bin/opt -load-pass-plugin "../build/ObfuscationPlugin.so" -passes="api_hashing,sub,splitbb,fla" -os windows -sub_loop=2 -split_num=2 -S "hack.ll" -o "out.ll"
../../llvm-project/build/bin/clang out.ll --target=x86_64-w64-windows-gnu 
```