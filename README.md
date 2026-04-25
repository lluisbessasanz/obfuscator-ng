# obfuscator-ng

## Testing the project

This project has been built and tested in Debian Trixie (13).

```bash
sudo apt install cmake ninja-build mingw-w64

git submodule update --init --recursive

cd llvm-project

cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86"

ninja -C build opt clang llvm-config llvm-link 

cd ../obfuscator

cmake -S . -B build -G Ninja \
  -DLLVM_DIR=../llvm-project/build/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release

ninja -C build

# Testing
cd test/build-test
cmake ..
cmake --build .
cmake --build . --target run_test
cmake --build . --target diff_ir
```

Now you have a working obfuscated test.c in build-test named obfuscated-test.out


