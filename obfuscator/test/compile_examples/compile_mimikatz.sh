#!/bin/bash

set -e

MIMIKATZ_ROOT=/home/user/Documents/mimikatz/mimikatz

SRC_DIRS="$MIMIKATZ_ROOT $MIMIKATZ_ROOT/../modules /home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools/decoder.c /home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools/ProxyCallbacks.cpp /home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools/PatternSearch.cpp"

IR_DIR=$MIMIKATZ_ROOT/build/ir

LLVM_BIN=../../../llvm-project/build/bin

XWIN="/home/user/Downloads/xwin-0.9.0-x86_64-unknown-linux-musl/.xwin-cache/splat"

VCTOOLS="$XWIN/crt"
UCRT="$XWIN/sdk"
WINSDK="$XWIN/sdk"

mkdir -p "$IR_DIR"

CFLAGS="/nologo  /W1 /MT /Gy /Ob2 /Oy /GF /GS- /fp:fast /fp:except- /errorReport:none /clang:-Wno-everything /clang:-fcommon -Wno-nonportable-include-path -Wno-microsoft-anon-tag -Wno-pragma-pack -Wno-missing-prototype-for-cc -Wno-incompatible-pointer-types -Wno-deprecated-declarations -Wno-microsoft-enum-forward-reference -Wno-microsoft-goto"

DEFINES="/D WIN32 /D NDEBUG /D _CONSOLE /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS"

rm -f "$IR_DIR"/*.ll

find $SRC_DIRS -type f \( -name "*.c" -o -name "*.cpp" \) -exec sh -c '
  IR_DIR="$1"
  LLVM_BIN="$2"
  MIMIKATZ_ROOT="$3"
  XWIN="$4"
  WINSDK="$5"
  UCRT="$6"
  VCTOOLS="$7"
  CFLAGS="$8"
  DEFINES="$9"
  shift 9

  for src do
    base=$(basename "$src")
    out="$IR_DIR/${base%.*}.ll"
    
    "$LLVM_BIN/clang-cl" "$src" $CFLAGS $DEFINES \
      /clang:-S /clang:-emit-llvm \
      /clang:-O0 \
      /clang:-Xclang /clang:-disable-O0-optnone \
      /vctoolsdir $VCTOOLS \
      /winsdkdir $WINSDK \
      /I $MIMIKATZ_ROOT/../inc \
      /clang:-o /clang:"$out"
  done
' sh "$IR_DIR" "$LLVM_BIN" "$MIMIKATZ_ROOT" "$XWIN" "$WINSDK" "$UCRT" "$VCTOOLS" "$CFLAGS" "$DEFINES" {} +

$LLVM_BIN/llvm-link $IR_DIR/*.ll -S -o "$MIMIKATZ_ROOT/build/combined_mimikatz.ll"

$LLVM_BIN/opt -load-pass-plugin "../../build/ObfuscationPlugin.so" -passes="api_hashing,function(constobf),function(swapops),function(sub),function(splitbb),function(fla),verify" -os_version="windows" -api_type="threadpool" --api_entry="wmain" --excluded_funcs="" -swap_prob=66 -arrenc_seed=0xAABBCCDD -sub_loop=2 -split_num=2 -S "$MIMIKATZ_ROOT/build/combined_mimikatz.ll" -o "$MIMIKATZ_ROOT/build/obfuscated_mimikatz.ll"

#LIBS="advapi32.lib ntdll.lib bcrypt.lib cabinet.lib crypt32.lib cryptdll.lib delayimp.lib dnsapi.lib fltlib.lib mpr.lib msxml2.lib ncrypt.lib netapi32.lib ntdsapi.lib odbc32.lib ole32.lib oleaut32.lib rpcrt4.lib shlwapi.lib samlib.lib secur32.lib shell32.lib user32.lib userenv.lib version.lib hid.lib setupapi.lib winscard.lib winsta.lib wbemuuid.lib wldap32.lib ws2_32.lib wtsapi32.lib msasn1.min.lib"
LIBS="ole32.lib rpcrt4.lib wbemuuid.lib msxml2.lib"

#/DEBUG:NONE /OPT:REF /OPT:ICF 

"$LLVM_BIN/clang-cl"  -x ir "$MIMIKATZ_ROOT/build/obfuscated_mimikatz.ll"  $CFLAGS $DEFINES \
  /c /clang:-o /clang:$MIMIKATZ_ROOT/build/hola.obj

"$LLVM_BIN/clang-cl" /home/user/Documents/obfuscator-ng/obfuscator/test/WindowsTools/Callback.asm \
  /c /clang:-o /clang:$MIMIKATZ_ROOT/build/WorkCallback.obj

"$LLVM_BIN/clang-cl"  $MIMIKATZ_ROOT/build/hola.obj $MIMIKATZ_ROOT/build/WorkCallback.obj $CFLAGS $DEFINES \
  -fuse-ld=lld \
  /Fe:$MIMIKATZ_ROOT/build/hola.exe \
  /link /DEBUG:NONE /OPT:REF /OPT:ICF  \
  /SUBSYSTEM:CONSOLE \
  /MACHINE:X64 \
  /LIBPATH:"$XWIN/crt/lib/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/um/x86_64" \
  /LIBPATH:"$XWIN/sdk/lib/ucrt/x86_64" \
  /LIBPATH:"$MIMIKATZ_ROOT/../lib/x64" \
    advapi32.hash.lib \
    bcrypt.lib \
    cryptdll.lib \
    fltlib.lib \
    hid.lib \
    msasn1.min.lib \
    ncrypt.lib \
    netapi32.min.lib \
    ntdll.min.lib \
    samlib.lib \
    winsta.lib \
    $LIBS
