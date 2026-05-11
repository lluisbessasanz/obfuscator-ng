#include "PatternSearch.h"

PVOID g_SaveRaxInRbx = NULL;

void* find_pattern_masked(const void* haystack, size_t haystack_size,
    const unsigned char* pattern,
    const unsigned char* mask,
    size_t pattern_len) {
    const unsigned char* mem = (const unsigned char*)haystack;

    for (size_t i = 0; i <= haystack_size - pattern_len; i++) {
        size_t j = 0;
        for (; j < pattern_len; j++) {
            if (mask[j] == 0x00 && mem[i + j] != pattern[j])
                break;
        }
        if (j == pattern_len)
            return (void*)(mem + i);
    }

    return nullptr;
}


void* find_pattern_in_module(HMODULE hmod,
    const unsigned char* pattern,
    const unsigned char* mask,
    size_t pattern_len) {
    if (!hmod) return NULL;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hmod;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((BYTE*)hmod + dos->e_lfanew);

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            BYTE* start  = (BYTE*)hmod + sec->VirtualAddress;
            DWORD size   = sec->Misc.VirtualSize;

            for (DWORD offset = 0; offset <= size - pattern_len; offset++) {
                size_t j = 0;
                for (; j < pattern_len; j++) {
                    if (mask[j] == 0x00 && start[offset + j] != pattern[j])
                        break;
                }
                if (j == pattern_len)
                    return (void*)(start + offset);
            }
            break;
        }
    }
    return NULL;
}

void* find_address_to_push() {
    if (g_SaveRaxInRbx != 0)
        return g_SaveRaxInRbx;

    /*
     * We are looking for a wininet.dll function whose epilogue matches:
     *
     *   CALL  ???                       ; wildcard – some internal call
     *   MOV   qword ptr [RBX], RAX      ; 48 89 03  ← address we return (+5)
     *   MOV   EAX, 1                    ; B8 01 00 00 00
     *   JMP   +6                        ; EB 06
     *   MOV   qword ptr [RDX], RAX      ; 48 89 02   \  alternate path
     *   MOV   qword ptr [RBX], RAX      ; 48 89 03   /
     *   ADD   RSP, 0x80                 ; 48 81 C4 80 00 00 00  ← 7-byte imm32 form!
     *   POP   RBX                       ; 5B
     *   RET                             ; C3
     *
     * IMPORTANT – encoding of  ADD RSP, 0x80:
     *   WRONG:  48 83 C4 80   (imm8 sign-extended → ADD RSP, -128 = SUB RSP, 128)
     *   RIGHT:  48 81 C4 80 00 00 00   (imm32 → ADD RSP, +128)
     *
     * Why 0x80?  do_call builds:
     *   pushq %rbx          (−8)
     *   subq  $0x80, %rsp   (−128)   room for shadow(0x20)+11 stack args(0x58)+pad(0x08)
     *   pushq %r12          (−8)     fake retaddr  → total frame = 0x90 bytes
     *
     * After target function ret:
     *   ADD RSP, 0x80  →  RSP = RSP_entry − 8   (where saved rbx lives)
     *   POP RBX        →  restores rbx
     *   RET            →  LastCallback's caller return address
     */

    // Pattern bytes (30 bytes total)
    const unsigned char pattern[] = {
        // CALL <wildcard 4-byte offset>
        0xE8, 0x00, 0x00, 0x00, 0x00,

        // MOV [RBX], RAX   ← find_address_to_push returns (this address + 5)
        0x48, 0x89, 0x03,

        // MOV EAX, 1
        0xB8, 0x01, 0x00, 0x00, 0x00,

        // JMP +6
        0xEB, 0x06,

        // MOV [RDX], RAX
        0x48, 0x89, 0x02,

        // MOV [RBX], RAX
        0x48, 0x89, 0x03,

        // FIX: ADD RSP, 0x80 — 7-byte imm32 form (was 4-byte imm8: 48 83 C4 80 = -128)
        0x48, 0x83, 0xC4, 0x20,

        // POP RBX
        0x5B,

        // RET
        0xC3
    };

    // Mask: 0x00 = must match exactly, 0xFF = wildcard
    const unsigned char mask[] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF,   // CALL (offset wildcarded)
        0x00, 0x00, 0x00,               // MOV [RBX], RAX
        0x00, 0x00, 0x00, 0x00, 0x00,   // MOV EAX, 1
        0x00, 0x00,                     // JMP +6
        0x00, 0x00, 0x00,               // MOV [RDX], RAX
        0x00, 0x00, 0x00,               // MOV [RBX], RAX
        0x00, 0x00, 0x00, 0x00,         // ADD RSP, 0x80 (7 bytes)
        0x00,                           // POP RBX
        0x00                            // RET
    };

    static_assert(sizeof(pattern) == sizeof(mask),
        "pattern and mask must have the same length");

    HMODULE hMod = LoadLibraryA("wininet.dll");
    if (!hMod) {
        printf("Failed to load wininet.dll: %lu\n", GetLastError());
        return nullptr;
    }



    void* address = find_pattern_in_module(hMod, pattern, mask, sizeof(pattern));

    if (!address) {
        printf("Pattern not found in wininet.dll\n");
        return nullptr;
    }


    // Return the address of the first  MOV [RBX], RAX  instruction
    // (5 bytes after the start of the CALL instruction).
    // When the target function returns here, RAX = its return value;
    // this instruction writes it into workItems[0].retAddress via rbx.
    g_SaveRaxInRbx = (void*)((UINT64)address + 5);
    return g_SaveRaxInRbx;
}