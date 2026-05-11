#!/bin/bash
set -e

SRC_DIR="/home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools"

SRC_FILES="$SRC_DIR/APIResolver.c $SRC_DIR/decoder.c $SRC_DIR/PatternSearch.cpp $SRC_DIR/ProxyCallbacks.cpp $SRC_DIR/httpsRS.c"

IR_DIR=./build/ir

LLVM_BIN=../../llvm-project/build/bin

XWIN="/home/user/Downloads/xwin-0.9.0-x86_64-unknown-linux-musl/.xwin-cache/splat"

VCTOOLS="$XWIN/crt"
UCRT="$XWIN/sdk"
WINSDK="$XWIN/sdk"

mkdir -p "$IR_DIR"

rm -f "$IR_DIR"/*.ll


for src in $SRC_FILES; do
  base=$(basename "$src")
  out="$IR_DIR/${base%.*}.ll"
  
  "$LLVM_BIN/clang-cl" "$src" \
    /clang:-S /clang:-emit-llvm \
    /DUNICODE /D_UNICODE \
    /clang:-O0 \
    /clang:-Xclang /clang:-disable-O0-optnone \
    /vctoolsdir $VCTOOLS \
    /winsdkdir $WINSDK \
    /clang:-o /clang:"$out"
done

$LLVM_BIN/llvm-link $IR_DIR/*.ll -S -o "./build/combined.ll"

#$LLVM_BIN/opt -load-pass-plugin "../build/ObfuscationPlugin.so" -passes="api_hashing,function(constobf),function(swapops),function(sub),function(splitbb),function(fla),arrenc,verify" -os_version="windows" -api_type="threadpool" --api_entry="main" --excluded_funcs="" -swap_prob=66 -arrenc_seed=0xAABBCCDD -sub_loop=2 -split_num=2 -arrenc_entry=main -S "./build/combined.ll" -o "./build/obfuscated.ll"
$LLVM_BIN/opt -load-pass-plugin "../build/ObfuscationPlugin.so" -passes="api_hashing,function(constobf),function(swapops),function(sub),function(splitbb),function(fla),verify" -os_version="windows" -api_type="threadpool" --api_entry="main" --excluded_funcs="" -swap_prob=66 -arrenc_seed=0xAABBCCDD -sub_loop=2 -split_num=2 -S "./build/combined.ll" -o "./build/obfuscated.ll"

LIBS="user32.lib gdi32.lib" 

"$LLVM_BIN/clang-cl"  -x ir "./build/obfuscated.ll" \
  /c /clang:-o /clang:./build/hola.obj

"$LLVM_BIN/clang-cl" /home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools/Callback.asm \
  /c /clang:-o /clang:./build/WorkCallback.obj

"$LLVM_BIN/clang-cl"  ./build/hola.obj ./build/WorkCallback.obj  \
  -fuse-ld=lld \
  /Fe:./build/hola.exe \
  /link /DEBUG:NONE /OPT:REF /OPT:ICF  \
  /SUBSYSTEM:CONSOLE \
  /MACHINE:X64 \
  /LIBPATH:"$XWIN/crt/lib/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/um/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/ucrt/x86_64" \
  $LIBS
