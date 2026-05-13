#include "TokenRegistry.h"

/* -------------------------------------------------------------------------
 * Module-private state
 * ---------------------------------------------------------------------- */
static HANDLE           g_hToken      = NULL;
static CRITICAL_SECTION g_lock;
static BOOL             g_initialized = FALSE;

/* -------------------------------------------------------------------------
 * TokenRegistry_Init
 *
 * Called exactly once at program entry (injected by the LLVM pass into the
 * beginning of the api_entry function).  Idempotent — safe if called more
 * than once, but the LLVM pass guarantees a single call.
 * ---------------------------------------------------------------------- */
void TokenRegistry_Init(void)
{
    if (g_initialized)
        return;

    InitializeCriticalSection(&g_lock);
    g_hToken      = NULL;
    g_initialized = TRUE;
}

/* -------------------------------------------------------------------------
 * TokenRegistry_Store
 *
 * Duplicates hToken into the global slot with SecurityImpersonation level.
 * If hToken is NULL the slot is cleared (equivalent to TokenRegistry_Clear).
 * The old handle, if any, is closed under the lock.
 *
 * Failure policy: if DuplicateTokenEx fails, the function returns without
 * touching the existing slot so the previous identity stays in force.
 * ---------------------------------------------------------------------- */
void TokenRegistry_Store(HANDLE hToken)
{
    if (!g_initialized)
        TokenRegistry_Init();

    HANDLE hDup = NULL;

    if (hToken != NULL) {
        BOOL ok = DuplicateTokenEx(
            hToken,
            TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_DUPLICATE,
            NULL,
            SecurityImpersonation,
            TokenImpersonation,
            &hDup
        );
        if (!ok)
            return;   /* leave existing token in place on failure */
    }

    EnterCriticalSection(&g_lock);
    HANDLE hOld = g_hToken;
    g_hToken     = hDup;    /* hDup is NULL when hToken was NULL → clear */
    LeaveCriticalSection(&g_lock);

    if (hOld)
        CloseHandle(hOld);
}

/* -------------------------------------------------------------------------
 * TokenRegistry_Clear
 *
 * Equivalent to Store(NULL).  Called automatically by the LLVM pass after
 * every intercepted RevertToSelf() call.
 * ---------------------------------------------------------------------- */
void TokenRegistry_Clear(void)
{
    TokenRegistry_Store(NULL);
}

/* -------------------------------------------------------------------------
 * TokenRegistry_Snapshot
 *
 * Returns a fresh duplicate of the currently stored token, or NULL if none
 * is registered.  The CALLER is responsible for CloseHandle on the returned
 * handle.  The duplicate has the same SecurityImpersonation level and is
 * independent of the registry copy, so the registry may be updated or
 * cleared while the snapshot is still in use.
 * ---------------------------------------------------------------------- */
HANDLE TokenRegistry_Snapshot(void)
{
    if (!g_initialized)
        return NULL;

    EnterCriticalSection(&g_lock);
    HANDLE hSrc = g_hToken;
    HANDLE hDup = NULL;
    if (hSrc != NULL) {
        DuplicateTokenEx(
            hSrc,
            TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_DUPLICATE,
            NULL,
            SecurityImpersonation,
            TokenImpersonation,
            &hDup
        );
        /* If duplication fails hDup stays NULL — caller gets NULL,
           no impersonation applied.  Preferable to crashing.      */
    }
    LeaveCriticalSection(&g_lock);

    return hDup;
}