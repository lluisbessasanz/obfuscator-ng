#pragma once
#include <windows.h>

/*
 * TokenRegistry — thread-impersonation propagation registry.
 *
 * Lifecycle:
 *   TokenRegistry_Init()     — call once at program entry (injected by LLVM pass).
 *   TokenRegistry_Store(h)   — called automatically after every SetThreadToken /
 *                              ImpersonateLoggedOnUser interception.  Duplicates h
 *                              into the global slot.  NULL clears the slot.
 *   TokenRegistry_Clear()    — equivalent to Store(NULL); called after RevertToSelf.
 *   TokenRegistry_Snapshot() — called by GuardedFirstCallback /
 *                              GuardedStackSmashingCallback before dispatching work.
 *                              Returns a fresh duplicate the caller must CloseHandle.
 *                              Returns NULL when no token is registered.
 *
 * Thread safety: all operations are protected by a CRITICAL_SECTION.
 */

#ifdef __cplusplus
extern "C" {
#endif

void   TokenRegistry_Init(void);
void   TokenRegistry_Store(HANDLE hToken);   /* NULL = clear the slot    */
void   TokenRegistry_Clear(void);            /* sugar for Store(NULL)    */
HANDLE TokenRegistry_Snapshot(void);         /* caller must CloseHandle  */

#ifdef __cplusplus
}
#endif