#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "address_space.h"
#include "elf_loader.h"

#define MOCK_PAGE_COUNT 1024u
#define MOCK_PHYS_BASE 0x00100000ULL

typedef struct mock_page {
    uint64_t physical;
    int active;
    uint8_t bytes[4096];
} MOCK_PAGE;
static MOCK_PAGE pages[MOCK_PAGE_COUNT];
static int fail_after = -1;
static uint64_t current_root;
static uint64_t invalidations;
static uint64_t shared_low[MINT_ASPACE_SHARED_PDPT_ENTRIES];

typedef struct __attribute__((packed)) test_elf64_ehdr {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} TEST_ELF64_EHDR;
typedef struct __attribute__((packed)) test_elf64_phdr {
    uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align;
} TEST_ELF64_PHDR;

static uint64_t mock_claim(void *ctx) {
    (void)ctx;
    if (fail_after == 0) { fail_after = -1; return 0; }
    if (fail_after > 0) --fail_after;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i) {
        if (!pages[i].active) {
            pages[i].active = 1;
            pages[i].physical = MOCK_PHYS_BASE + (uint64_t)i * 4096ULL;
            memset(pages[i].bytes, 0, sizeof(pages[i].bytes));
            return pages[i].physical;
        }
    }
    return 0;
}
static int mock_release(void *ctx, uint64_t pa) {
    (void)ctx;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i) {
        if (pages[i].active && pages[i].physical == pa) {
            pages[i].active = 0;
            memset(pages[i].bytes, 0xDD, sizeof(pages[i].bytes));
            return 1;
        }
    }
    return 0;
}
static void *mock_ptr(void *ctx, uint64_t pa) {
    (void)ctx;
    if (pa & 4095u) return 0;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i)
        if (pages[i].active && pages[i].physical == pa) return pages[i].bytes;
    return 0;
}
static int mock_claimed(void *ctx, uint64_t pa) {
    (void)ctx;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i)
        if (pages[i].active && pages[i].physical == pa) return 1;
    return 0;
}
static void mock_load_root(void *ctx, uint64_t pa) { (void)ctx; current_root = pa; }
static void mock_invlpg(void *ctx, uint64_t va) { (void)ctx; (void)va; ++invalidations; }
static unsigned active_pages(void) {
    unsigned n=0; for (unsigned i=0;i<MOCK_PAGE_COUNT;i++) n += pages[i].active != 0; return n;
}
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n", label); return 1; } else puts("PASS: " label); } while (0)

int main(void) {
    MINT_ADDRESS_SPACE_BACKEND backend = {0, mock_claim, mock_release, mock_ptr,
                                           mock_claimed, mock_load_root, mock_invlpg};
    for (unsigned i=0; i<MINT_ASPACE_SHARED_PDPT_ENTRIES; ++i)
        shared_low[i] = (0x40000000ULL + (uint64_t)i * 4096ULL) | 0x3ULL;
    CHECK(mint_address_space_system_init(&backend, 0x00300000ULL, shared_low,
                                         0x200003ULL, 1), "manager init with supervisor template");

    MINT_ADDRESS_SPACE a = {0}, b = {0}, failed = {0};
    CHECK(mint_address_space_create(&a) && mint_address_space_create(&b), "create two independent roots");
    CHECK(mint_address_space_root(&a) != mint_address_space_root(&b), "root pages are independent");
    uint64_t *ra = (uint64_t *)mock_ptr(0, mint_address_space_root(&a));
    uint64_t *rb = (uint64_t *)mock_ptr(0, mint_address_space_root(&b));
    uint64_t *da = (uint64_t *)mock_ptr(0, a.pdpt_physical);
    uint64_t *db = (uint64_t *)mock_ptr(0, b.pdpt_physical);
    CHECK(ra && rb && ra[256] == 0x200003ULL && rb[256] == 0x200003ULL,
          "share only supervisor direct-map PML4 entry");
    CHECK((ra[0] & 7ULL) == 7ULL && (rb[0] & 7ULL) == 7ULL,
          "private lower PML4 allows user subtree while retaining supervisor leaf restrictions");
    CHECK(da && db && memcmp(da, shared_low, sizeof(shared_low)) == 0 &&
          memcmp(db, shared_low, sizeof(shared_low)) == 0,
          "copy shared low kernel mapping template");
    CHECK((da[64] & 1ULL) == 0 && (db[64] & 1ULL) == 0,
          "user region starts unmapped and is private");

    uint64_t fa = mint_address_space_alloc_frame(&a);
    uint64_t fb = mint_address_space_alloc_frame(&b);
    CHECK(fa && fb && fa != fb, "allocate address-space-owned user frames");
    const uint64_t user_va = MINT_ASPACE_USER_BASE + 0x00200000ULL;
    CHECK(!mint_address_space_map(&a, user_va, fa, MINT_PAGE_MAP_WRITE),
          "reject mapping without a reserved virtual region");
    CHECK(mint_address_space_region_reserve(&a, user_va, 4096u, MINT_PAGE_MAP_WRITE,
                                             MINT_ASPACE_REGION_ANONYMOUS) &&
          mint_address_space_region_reserve(&b, user_va, 4096u, MINT_PAGE_MAP_WRITE,
                                             MINT_ASPACE_REGION_ANONYMOUS) &&
          mint_address_space_region_reserve(&b, user_va + 4096u, 4096u,
                                             MINT_PAGE_MAP_WRITE, MINT_ASPACE_REGION_ANONYMOUS),
          "reserve same VA independently plus a cross-owner probe region");
    CHECK(!mint_address_space_region_reserve(&a, user_va, 8192u, MINT_PAGE_MAP_WRITE,
                                              MINT_ASPACE_REGION_ANONYMOUS),
          "reject overlapping virtual-region reservations");
    CHECK(!mint_address_space_region_reserve(&a, user_va + 0x10000u, 4096u,
                                              MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC,
                                              MINT_ASPACE_REGION_ANONYMOUS),
          "reject writable executable regions");
    MINT_ADDRESS_REGION region = {0};
    CHECK(mint_address_space_region_query(&a, user_va + 12u, &region) &&
          region.base == user_va && region.size == 4096u &&
          (region.flags & MINT_PAGE_MAP_WRITE) && region.kind == MINT_ASPACE_REGION_ANONYMOUS,
          "query region metadata by contained virtual address");
    CHECK(mint_address_space_map(&a, user_va, fa, MINT_PAGE_MAP_WRITE) &&
          mint_address_space_map(&b, user_va, fb, MINT_PAGE_MAP_WRITE),
          "map the same virtual address in separately reserved roots");
    uint64_t ppa=0, ppb=0; uint32_t fflags=0;
    CHECK(mint_address_space_lookup(&a, user_va+17, &ppa, &fflags) && ppa == fa+17 &&
          (fflags & MINT_PAGE_MAP_WRITE) && !(fflags & MINT_PAGE_MAP_EXEC),
          "lookup physical offset and enforce writable non-executable leaf");
    CHECK(mint_address_space_lookup(&b, user_va, &ppb, 0) && ppb == fb && ppb != fa,
          "the same VA resolves to a different frame in the other root");
    CHECK(!mint_address_space_map(&b, user_va+4096, fa, MINT_PAGE_MAP_WRITE),
          "reject another address-space's frame inside an otherwise valid region");
    CHECK(!mint_address_space_map(&a, user_va, fa, MINT_PAGE_MAP_WRITE),
          "reject duplicate leaf mapping");
    CHECK(!mint_address_space_map(&a, MINT_ASPACE_USER_BASE-4096, fa, MINT_PAGE_MAP_WRITE) &&
          !mint_address_space_map(&a, MINT_ASPACE_USER_END, fa, MINT_PAGE_MAP_WRITE),
          "reject addresses outside private user window");

    CHECK(mint_address_space_activate(&a) && current_root == a.root_physical && a.active,
          "activate address space A");
    CHECK(!mint_address_space_destroy(&a), "refuse to destroy the active address space");
    CHECK(mint_address_space_activate(&b) && current_root == b.root_physical && !a.active && b.active,
          "switch CR3 to address space B and update active state");
    CHECK(mint_address_space_activate_kernel() && current_root == 0x00300000ULL && !b.active,
          "restore kernel root");
    CHECK(invalidations >= 2, "mapping changes request TLB invalidation");

    uint64_t fail_frame = mint_address_space_alloc_frame(&a);
    CHECK(fail_frame != 0, "allocate frame for mapping rollback test");
    const uint64_t rollback_va = MINT_ASPACE_USER_BASE + (1ULL << 30);
    CHECK(mint_address_space_region_reserve(&a, rollback_va, 4096u, MINT_PAGE_MAP_WRITE,
                                             MINT_ASPACE_REGION_ANONYMOUS),
          "reserve second region for mapping rollback test");
    fail_after = 1; /* allow PD allocation at private PDPT[65], fail PT allocation */
    uint64_t before_fail = active_pages();
    CHECK(!mint_address_space_map(&a, rollback_va, fail_frame, MINT_PAGE_MAP_WRITE),
          "simulate partial page-table allocation failure");
    uint64_t *da_after = (uint64_t *)mock_ptr(0, a.pdpt_physical);
    CHECK(active_pages() == before_fail && da_after && !(da_after[65] & 1ULL),
          "mapping failure rolls back newly-created table pages");
    CHECK(mint_address_space_release_frame(&a, fail_frame), "release frame after rolled-back mapping");
    CHECK(mint_address_space_region_release(&a, rollback_va), "release unmapped rollback region");

    uint64_t got=0; uint32_t gotflags=0;
    CHECK(mint_address_space_unmap(&a, user_va, &got, &gotflags) && got == fa &&
          (gotflags & MINT_PAGE_MAP_WRITE), "unmap returns correct frame and permissions");
    CHECK(!mint_address_space_unmap(&a, user_va, 0, 0), "reject repeated unmap");
    CHECK(mint_address_space_unmap(&b, user_va, &got, &gotflags) && got == fb,
          "unmap the peer address-space mapping");
    CHECK(mint_address_space_release_frame(&a, fa), "release now-unmapped user frame");
    CHECK(mint_address_space_region_release(&a, user_va) &&
          mint_address_space_region_release(&b, user_va) &&
          mint_address_space_region_release(&b, user_va + 4096u),
          "release regions after their mappings are removed");
    CHECK(mint_address_space_destroy(&a), "destroy address space A and reclaim its tables");
    CHECK(mint_address_space_destroy(&b), "destroy address space B and reclaim mapped frames");
    CHECK(active_pages() == 0, "no physical page leaks after address-space teardown");

    /* A small, synthetic but structurally valid static ELF64 image: RX text,
       RW data+BSS, and a private NX stack loaded by the production loader. */
    MINT_ADDRESS_SPACE elf_space = {0};
    uint8_t synthetic_elf[0x2008];
    memset(synthetic_elf, 0, sizeof(synthetic_elf));
    TEST_ELF64_EHDR *eh = (TEST_ELF64_EHDR *)synthetic_elf;
    eh->ident[0]=0x7F; eh->ident[1]='E'; eh->ident[2]='L'; eh->ident[3]='F';
    eh->ident[4]=2; eh->ident[5]=1; eh->ident[6]=1;
    eh->type=2; eh->machine=62; eh->version=1;
    const uint64_t elf_code_va = MINT_ASPACE_USER_BASE + 0x00200000ULL;
    eh->entry=elf_code_va; eh->phoff=sizeof(*eh); eh->ehsize=sizeof(*eh);
    eh->phentsize=sizeof(TEST_ELF64_PHDR); eh->phnum=2;
    TEST_ELF64_PHDR *ph=(TEST_ELF64_PHDR *)(synthetic_elf+eh->phoff);
    ph[0]=(TEST_ELF64_PHDR){1u,5u,0x1000u,elf_code_va,0u,4u,4096u,4096u};
    ph[1]=(TEST_ELF64_PHDR){1u,6u,0x2000u,elf_code_va+4096u,0u,8u,8192u,4096u};
    memcpy(synthetic_elf+0x1000u, "CODE", 4u);
    const uint64_t data_tag=0x4D494E54454C4636ULL;
    memcpy(synthetic_elf+0x2000u, &data_tag, sizeof(data_tag));
    CHECK(mint_address_space_create(&elf_space), "create empty address space for ELF loader");
    MINT_USER_IMAGE loaded_image={0};
    CHECK(mint_elf64_load_user(&elf_space, synthetic_elf, sizeof(synthetic_elf),
                                4u*4096u, &loaded_image),
          "load static ELF64 PT_LOAD segments and allocate private stack");
    uint64_t elf_pa=0; uint32_t elf_flags=0;
    CHECK(loaded_image.loaded && loaded_image.entry==elf_code_va && loaded_image.segment_count==2u &&
          loaded_image.stack_top>loaded_image.stack_base &&
          mint_address_space_lookup(&elf_space,elf_code_va,&elf_pa,&elf_flags) &&
          (elf_flags&MINT_PAGE_MAP_EXEC) && !(elf_flags&MINT_PAGE_MAP_WRITE),
          "ELF loader preserves RX text and records entry/stack layout");
    CHECK(mint_address_space_lookup(&elf_space,elf_code_va+4096u,&elf_pa,&elf_flags) &&
          (elf_flags&MINT_PAGE_MAP_WRITE) && !(elf_flags&MINT_PAGE_MAP_EXEC) &&
          mint_address_space_lookup(&elf_space,loaded_image.stack_base,&elf_pa,&elf_flags) &&
          (elf_flags&MINT_PAGE_MAP_WRITE) && !(elf_flags&MINT_PAGE_MAP_EXEC),
          "ELF loader creates RW+NX data and stack mappings");
    CHECK(mint_address_space_activate(&elf_space), "activate loaded ELF address space for verification");
    char loaded_code[4]={0}; uint64_t loaded_tag=0;
    CHECK(mint_address_space_copy_from_user(&elf_space,loaded_code,elf_code_va,sizeof(loaded_code)) &&
          memcmp(loaded_code,"CODE",4u)==0 &&
          mint_address_space_copy_from_user(&elf_space,&loaded_tag,elf_code_va+4096u,sizeof(loaded_tag)) &&
          loaded_tag==data_tag,
          "ELF loader copies segment bytes and leaves BSS/data mapping valid");
    CHECK(mint_address_space_activate_kernel() && mint_address_space_destroy(&elf_space),
          "teardown loaded ELF address space cleanly");
    CHECK(active_pages()==0, "ELF load/teardown returns all mock pages");

    fail_after = 1; /* root page succeeds, private PDPT page allocation fails */
    CHECK(!mint_address_space_create(&failed), "simulate private-root creation failure");
    CHECK(active_pages() == 0, "root creation failure releases partial allocations");
    puts("PASS: address-space isolation, mapping permissions, rollback, switching and teardown tests complete");
    return 0;
}
