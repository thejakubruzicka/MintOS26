#include <stddef.h>
#include <stdint.h>
#include "../include/elf_loader.h"
#include "../include/paging.h"

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_GNU_STACK 0x6474E551u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define PAGE_MASK (MINT_PAGING_PAGE_SIZE - 1u)

typedef struct __attribute__((packed)) mint_elf64_ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} MINT_ELF64_EHDR;

typedef struct __attribute__((packed)) mint_elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} MINT_ELF64_PHDR;

typedef struct mint_segment_plan {
    MINT_ELF64_PHDR ph;
    uint64_t map_base;
    uint64_t map_end;
    uint32_t page_flags;
    uint32_t region_kind;
} MINT_SEGMENT_PLAN;

static void bytes_zero(void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}

static void bytes_copy(void *out, const void *in, size_t n) {
    uint8_t *d = (uint8_t *)out;
    const uint8_t *s = (const uint8_t *)in;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}


static int align_up_page(uint64_t value, uint64_t *out) {
    if (!out || value > UINT64_MAX - PAGE_MASK) return 0;
    *out = (value + PAGE_MASK) & ~PAGE_MASK;
    return 1;
}

static int pow2_or_small(uint64_t x) {
    return x <= 1u || (x & (x - 1u)) == 0;
}

static int plan_overlaps(const MINT_SEGMENT_PLAN *plans, uint32_t n,
                         uint64_t base, uint64_t end) {
    for (uint32_t i = 0; i < n; ++i)
        if (base < plans[i].map_end && plans[i].map_base < end) return 1;
    return 0;
}

static int map_region_pages(MINT_ADDRESS_SPACE *space, uint64_t base,
                            uint64_t end, uint32_t flags) {
    for (uint64_t va = base; va < end; va += MINT_PAGING_PAGE_SIZE) {
        uint64_t pa = mint_address_space_alloc_frame(space);
        if (!pa) return 0;
        if (!mint_address_space_map(space, va, pa, flags)) {
            (void)mint_address_space_release_frame(space, pa);
            return 0;
        }
    }
    return 1;
}

static int fail_and_destroy(MINT_ADDRESS_SPACE *space) {
    if (space && space->initialized && !space->active)
        (void)mint_address_space_destroy(space);
    return 0;
}

int mint_elf64_load_user(MINT_ADDRESS_SPACE *space, const void *image,
                         size_t image_size, size_t stack_bytes,
                         MINT_USER_IMAGE *out) {
    if (out) bytes_zero(out, sizeof(*out));
    if (!space || !space->initialized || space->active || !image || !out ||
        image_size < sizeof(MINT_ELF64_EHDR) ||
        stack_bytes < 2u * MINT_PAGING_PAGE_SIZE ||
        stack_bytes > 64u * 1024u || (stack_bytes & PAGE_MASK) != 0)
        return fail_and_destroy(space);

    const uint8_t *blob = (const uint8_t *)image;
    MINT_ELF64_EHDR eh;
    bytes_copy(&eh, blob, sizeof(eh));
    if (eh.ident[0] != 0x7Fu || eh.ident[1] != 'E' || eh.ident[2] != 'L' ||
        eh.ident[3] != 'F' || eh.ident[4] != ELFCLASS64 || eh.ident[5] != ELFDATA2LSB ||
        eh.ident[6] != EV_CURRENT || eh.type != ET_EXEC || eh.machine != EM_X86_64 ||
        eh.version != EV_CURRENT || eh.ehsize != sizeof(MINT_ELF64_EHDR) ||
        eh.phentsize != sizeof(MINT_ELF64_PHDR) || eh.phnum == 0 ||
        eh.phnum > MINT_ELF64_MAX_SEGMENTS || eh.phoff > image_size ||
        (uint64_t)eh.phnum * eh.phentsize > image_size - eh.phoff)
        return fail_and_destroy(space);

    MINT_SEGMENT_PLAN plans[MINT_ELF64_MAX_SEGMENTS];
    bytes_zero(plans, sizeof(plans));
    uint32_t count = 0;
    uint64_t total_pages = 0;
    uint64_t image_base = UINT64_MAX, image_end = 0;
    int entry_is_executable = 0;

    for (uint32_t i = 0; i < eh.phnum; ++i) {
        MINT_ELF64_PHDR ph;
        const uint64_t off = eh.phoff + (uint64_t)i * eh.phentsize;
        bytes_copy(&ph, blob + (size_t)off, sizeof(ph));
        if (ph.type == PT_INTERP || ph.type == PT_DYNAMIC) return fail_and_destroy(space);
        if (ph.type == PT_GNU_STACK) {
            if (ph.flags & PF_X) return fail_and_destroy(space);
            continue;
        }
        if (ph.type != PT_LOAD) continue;
        if (count >= MINT_ELF64_MAX_SEGMENTS || ph.memsz == 0 || ph.filesz > ph.memsz ||
            ph.offset > image_size || ph.filesz > image_size - ph.offset ||
            ph.vaddr > UINT64_MAX - ph.memsz || !pow2_or_small(ph.align) ||
            ((ph.flags & PF_W) && (ph.flags & PF_X)) || !(ph.flags & PF_R) ||
            (ph.flags & ~(PF_R | PF_W | PF_X)) != 0)
            return fail_and_destroy(space);
        if (ph.align > 1u && ((ph.vaddr - ph.offset) & (ph.align - 1u)) != 0)
            return fail_and_destroy(space);
        uint64_t mem_end = ph.vaddr + ph.memsz;
        if (!mint_address_space_is_user_address(ph.vaddr) ||
            mem_end <= ph.vaddr || mem_end > MINT_ASPACE_USER_END)
            return fail_and_destroy(space);
        if ((ph.vaddr & PAGE_MASK) != (ph.offset & PAGE_MASK))
            return fail_and_destroy(space);
        uint64_t map_base = ph.vaddr & ~PAGE_MASK, map_end = 0;
        if (!align_up_page(mem_end, &map_end) || map_end > MINT_ASPACE_USER_END ||
            map_end <= map_base || plan_overlaps(plans, count, map_base, map_end))
            return fail_and_destroy(space);
        uint64_t pages = (map_end - map_base) / MINT_PAGING_PAGE_SIZE;
        if (pages > MINT_ASPACE_MAX_USER_FRAMES || total_pages > MINT_ASPACE_MAX_USER_FRAMES - pages)
            return fail_and_destroy(space);
        total_pages += pages;

        uint32_t page_flags = 0;
        if (ph.flags & PF_W) page_flags |= MINT_PAGE_MAP_WRITE;
        if (ph.flags & PF_X) page_flags |= MINT_PAGE_MAP_EXEC;
        plans[count].ph = ph;
        plans[count].map_base = map_base;
        plans[count].map_end = map_end;
        plans[count].page_flags = page_flags;
        plans[count].region_kind = (ph.flags & PF_X) ? MINT_ASPACE_REGION_IMAGE_TEXT :
                                                       MINT_ASPACE_REGION_IMAGE_DATA;
        if ((ph.flags & PF_X) && eh.entry >= ph.vaddr && eh.entry < mem_end)
            entry_is_executable = 1;
        if (map_base < image_base) image_base = map_base;
        if (map_end > image_end) image_end = map_end;
        ++count;
    }
    if (!count || !entry_is_executable) return fail_and_destroy(space);

    uint64_t stack_top = MINT_ASPACE_USER_END - MINT_PAGING_PAGE_SIZE;
    uint64_t stack_base = stack_top - stack_bytes;
    if (stack_base <= MINT_ASPACE_USER_BASE || stack_base - MINT_PAGING_PAGE_SIZE < MINT_ASPACE_USER_BASE ||
        plan_overlaps(plans, count, stack_base - MINT_PAGING_PAGE_SIZE, stack_top))
        return fail_and_destroy(space);
    total_pages += stack_bytes / MINT_PAGING_PAGE_SIZE;
    if (total_pages > MINT_ASPACE_MAX_USER_FRAMES) return fail_and_destroy(space);

    for (uint32_t i = 0; i < count; ++i) {
        if (!mint_address_space_region_reserve(space, plans[i].map_base,
                plans[i].map_end - plans[i].map_base, plans[i].page_flags,
                (MINT_ASPACE_REGION_KIND)plans[i].region_kind) ||
            !map_region_pages(space, plans[i].map_base, plans[i].map_end, plans[i].page_flags))
            return fail_and_destroy(space);
    }
    if (!mint_address_space_region_reserve(space, stack_base, stack_bytes,
            MINT_PAGE_MAP_WRITE, MINT_ASPACE_REGION_STACK) ||
        !map_region_pages(space, stack_base, stack_top, MINT_PAGE_MAP_WRITE))
        return fail_and_destroy(space);

    /* The trusted loader initializes mapped pages through the physical backing,
       so it can populate RX text without temporarily making it writable. */
    for (uint32_t i = 0; i < count; ++i) {
        const MINT_ELF64_PHDR *ph = &plans[i].ph;
        if (ph->filesz && !mint_address_space_initialize_mapped_bytes(space, ph->vaddr,
                blob + (size_t)ph->offset, (size_t)ph->filesz))
            return fail_and_destroy(space);
    }

    out->entry = eh.entry;
    out->image_base = image_base;
    out->image_end = image_end;
    out->stack_base = stack_base;
    out->stack_top = stack_top & ~15ULL;
    out->stack_size = stack_bytes;
    out->segment_count = count;
    out->loaded = 1u;
    return 1;
}
