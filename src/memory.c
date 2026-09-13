/*==============================================================================
 * rootkit/src/memory.c
 *------------------------------------------------------------------------------
 * Pool allocation wrappers with tag tracking and secure wipe.
 * All allocations use NonPagedPoolNx with the 'Kdmp' tag.
 *============================================================================*/
#include "utils.h"

/*------------------------------------------------------------------------------
 * rk_alloc - Allocate non-paged pool (Nx = no-execute)
 *----------------------------------------------------------------------------*/
PVOID rk_alloc(SIZE_T size, ULONG tag) {
    return ExAllocatePoolWithTag(NonPagedPoolNx, size, tag);
}

/*------------------------------------------------------------------------------
 * rk_alloc_zero - Allocate and zero-initialize
 *----------------------------------------------------------------------------*/
PVOID rk_alloc_zero(SIZE_T size, ULONG tag) {
    PVOID p = rk_alloc(size, tag);
    if (p) RtlZeroMemory(p, size);
    return p;
}

/*------------------------------------------------------------------------------
 * rk_free - Free pool with tag verification
 *----------------------------------------------------------------------------*/
VOID rk_free(PVOID p, ULONG tag) {
    if (p) ExFreePoolWithTag(p, tag);
}

/*------------------------------------------------------------------------------
 * rk_secure_free - Secure wipe + free
 *   Overwrites memory with zeros before returning to pool to prevent
 *   sensitive data leakage (keys, tokens, etc.).
 *----------------------------------------------------------------------------*/
VOID rk_secure_free(PVOID p, SIZE_T size, ULONG tag) {
    if (!p) return;
    RtlSecureZeroMemory(p, size);
    ExFreePoolWithTag(p, tag);
}

/*==============================================================================
 * UNICODE STRING HELPERS
 *----------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
 * rk_unicode_from_bytes - Convert raw UTF-16LE bytes to UNICODE_STRING
 *   Allocates buffer from non-paged pool. Caller must call rk_unicode_free().
 *----------------------------------------------------------------------------*/
NTSTATUS rk_unicode_from_bytes(PUNICODE_STRING out, const UCHAR* bytes, USHORT byte_len) {
    if (!out || !bytes || byte_len == 0) return STATUS_INVALID_PARAMETER;

    out->Length        = byte_len;
    out->MaximumLength = byte_len;
    out->Buffer        = (PWCH)rk_alloc(byte_len, POOL_TAG);
    if (!out->Buffer) return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(out->Buffer, bytes, byte_len);
    return STATUS_SUCCESS;
}

/*------------------------------------------------------------------------------
 * rk_unicode_free - Secure free a UNICODE_STRING allocated by above
 *----------------------------------------------------------------------------*/
VOID rk_unicode_free(PUNICODE_STRING s) {
    if (s && s->Buffer) {
        RtlSecureZeroMemory(s->Buffer, s->MaximumLength);
        ExFreePoolWithTag(s->Buffer, POOL_TAG);
        s->Buffer = NULL;
        s->Length = s->MaximumLength = 0;
    }
}