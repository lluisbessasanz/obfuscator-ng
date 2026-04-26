# obfuscator-ng

## Description

This project is a port from LLVM 4.0 to LLVM 19 of the original obfuscator-llvm project: https://lnkd.in/eeBkzn_U

So far, I have already ported several features, including:
- Operation substitution
- Basic block splitting
- Control-flow flattening

I have also built a compile-time API hashing pass. This means you can write code using the libraries you normally need, and if a function is referenced — or explicitly added in obfuscator/lib/ApiHashing.cpp — the pass will replace it with a CRC32-obfuscated resolution mechanism at compile time.

For those less familiar with LLVM: LLVM is a compiler infrastructure widely used to build compilers, language toolchains, static analyzers, and optimization frameworks. One of its most powerful concepts is the LLVM pass: a modular transformation or analysis step that operates on the intermediate representation, or IR, of a program.

Passes can be used for many purposes, including optimization, instrumentation, static analysis, and — in this case — code obfuscation. By working at the IR level, LLVM passes can transform code before the final binary is generated, making them a very flexible place to implement security research techniques.

## Building the project

This project has been built and tested in Debian Trixie (13).

```bash
# Depends
sudo apt install cmake ninja-build mingw-w64


# Compile needed parts of LLVM compiler toolchain.

git submodule update --init --recursive

cd llvm-project

cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86"

ninja -C build opt clang llvm-config llvm-link 


# Build the Obfuscation Plugin

cd ../obfuscator

cmake -S . -B build -G Ninja \
  -DLLVM_DIR=../llvm-project/build/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release

ninja -C build
```

## Passes

### Subtitution Pass

The Substitution Pass replaces simple arithmetic, logical, and binary operations with equivalent but more complex instruction sequences.

**Options**:
* -passes="sub"
* -sub_loop=<number of substitutions on same expression>

### Flattening Pass

The Flattening Pass transforms the control flow of a function into a dispatcher-based structure.

Normally, a function has a clear control-flow graph made of basic blocks connected through direct branches. Flattening rewrites this structure so that execution is routed through a central dispatcher, usually based on a state variable and a switch statement.

**Options**:
* -passes="fla"

### Split Basic Block Pass

The Split Basic Block Pass breaks large basic blocks into smaller ones.

A basic block is a sequence of LLVM instructions with a single entry point and a single exit point. By splitting these blocks, the pass increases the number of nodes in the control-flow graph.

For example, one basic block containing many instructions can be divided into several smaller blocks connected by unconditional branches.

**Options**:
* -passes="splitbb"

### API Hashing (only working in Windows executables at this time)

The API Hashing Pass replaces direct references to selected API functions with compile-time generated hashes.
The hash is calculated at compile time, and the runtime resolver is responsible for finding the real function address.

For running api_hashing you need to link your executable with API Resolver.

```bash
/path/to/llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S WindowsTools/APIResolver.cpp -o WindowsTools/APIResolver.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu

# If you have various IR files you need to link them.
/path/to/llvm-project/build/bin/llvm-link WindowsTools/APIResolver.ll your-file.ll -S -o hack.ll
```

**Options**:
* -passes="api_hashing"
* -os windows

## Testing the project

This project has been built and tested in Debian Trixie (13).

In this example we compile an obfuscated reverse shell, when compiling for Linux just omit the flags `-I/usr/x86_64-w64-mingw32/include` and `--target=x86_64-w64-windows-gnu`.

```bash
/path/to/llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -Wimplicit-function-declaration -emit-llvm -S httpsRS.c -o httpsRS.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu
/path/to/llvm-project/build/bin/clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S WindowsTools/APIResolver.cpp -o WindowsTools/APIResolver.ll -I/usr/x86_64-w64-mingw32/include --target=x86_64-w64-windows-gnu

# If you have various IR files you need to link them.
/path/to/llvm-project/build/bin/llvm-link WindowsTools/APIResolver.ll httpsRS.ll -S -o hack.ll

# Run the passes on the ouput .ll
/path/to/llvm-project/build/bin/opt -load-pass-plugin "../build/ObfuscationPlugin.so" -passes="api_hashing,sub,splitbb,fla" -os windows -sub_loop=2 -split_num=2 -S "hack.ll" -o "out.ll"

# Compile
/path/to/llvm-project/build/bin/clang out.ll --target=x86_64-w64-windows-gnu 
```

## Extra

There is an example program and compilation steps in `obfuscator/test` directory that has bypassed the following security solutions.

* Microsoft Defender
* Avast
* Malwarebytes
* Bitdefender Free, if using httpsRS.c (a little bit buggy the RS interface)