/* 26.5.014.0: process memory services (see proc_memory.h). */
#include <stdint.h>
#include "../include/proc_memory.h"
#include "../include/address_space.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/uapi/mintos/process.h"

#define PAGE 4096ULL

static uint64_t mmap_high(void) {
    /* Leave the top 16 GiB for the stack, its guard and future use. */
    return MINT_ASPACE_USER_END - 16ULL * 1024ULL * 1024ULL * 1024ULL;
}

static int prot_flags(uint32_t prot, uint32_t *flags) {
    if (prot & ~(MINT_MEM_READ | MINT_MEM_WRITE | MINT_MEM_EXEC)) return -MINT_EINVAL;
    if ((prot & MINT_MEM_WRITE) && (prot & MINT_MEM_EXEC)) return -MINT_EACCES;   /* W^X */
    uint32_t f = 0;
    if (!prot) f = MINT_ASPACE_REGION_NOACCESS;
    if (prot & MINT_MEM_WRITE) f |= MINT_PAGE_MAP_WRITE;
    if (prot & MINT_MEM_EXEC) f |= MINT_PAGE_MAP_EXEC;
    *flags = f;
    return 0;
}

int64_t mint_proc_mmap(MINT_PROCESS *p, uint64_t hint, uint64_t length, uint32_t prot, int fixed) {
    if (!p || !p->address_space.initialized) return -MINT_EINVAL;
    if (!length || length > MINT_MMAP_MAX_LENGTH) return -MINT_EINVAL;
    uint64_t size = (length + PAGE - 1u) & ~(PAGE - 1u);
    uint32_t flags = 0;
    int rc = prot_flags(prot, &flags);
    if (rc) return rc;
    MINT_ADDRESS_SPACE *space = &p->address_space;
    uint64_t address = 0;
    if (fixed) {
        if ((hint & (PAGE - 1u)) || !mint_address_space_is_user_address(hint) ||
            hint > MINT_ASPACE_USER_END - size) return -MINT_EINVAL;
        /* MAP_FIXED replaces older anonymous mappings; image/stack/shared
         * memory is never replaced. */
        if (!mint_address_space_region_carve(space, hint, size)) return -MINT_ENOMEM;
        address = hint;
    } else {
        uint64_t low = MINT_MMAP_LOW;
        if (hint && !(hint & (PAGE - 1u)) && hint >= MINT_MMAP_LOW && hint < mmap_high()) low = hint;
        if (!mint_address_space_find_gap(space, low, mmap_high(), size, &address) &&
            !mint_address_space_find_gap(space, MINT_MMAP_LOW, mmap_high(), size, &address))
            return -MINT_ENOMEM;
    }
    if (!mint_address_space_region_add(space, address, size, flags, MINT_ASPACE_REGION_ANONYMOUS))
        return -MINT_ENOMEM;
    p->mmap_bytes += size;
    return (int64_t)address;
}

int64_t mint_proc_munmap(MINT_PROCESS *p, uint64_t address, uint64_t length) {
    if (!p || !length || (address & (PAGE - 1u)) || !mint_address_space_is_user_address(address))
        return -MINT_EINVAL;
    uint64_t size = (length + PAGE - 1u) & ~(PAGE - 1u);
    if (size > MINT_ASPACE_USER_END - address) return -MINT_EINVAL;
    /* The brk heap and the stack are not unmappable through munmap. */
    if (p->brk_current > p->brk_base && address < p->brk_base + MINT_BRK_MAX &&
        p->brk_base < address + size) return -MINT_EINVAL;
    if (!mint_address_space_region_carve(&p->address_space, address, size)) return -MINT_EINVAL;
    p->mmap_bytes = p->mmap_bytes > size ? p->mmap_bytes - size : 0;
    return 0;
}

int64_t mint_proc_mprotect(MINT_PROCESS *p, uint64_t address, uint64_t length, uint32_t prot) {
    if (!p || (address & (PAGE - 1u))) return -MINT_EINVAL;
    if (!length) return 0;
    uint64_t size = (length + PAGE - 1u) & ~(PAGE - 1u);
    uint32_t flags = 0;
    int rc = prot_flags(prot, &flags);
    if (rc) return rc;
    /* Image segments keep their fixed permissions; asking for what they
     * already have succeeds so loaders like musl's do not fail. */
    MINT_ADDRESS_REGION region;
    if (mint_address_space_region_query(&p->address_space, address, &region) &&
        (region.kind == MINT_ASPACE_REGION_IMAGE_TEXT || region.kind == MINT_ASPACE_REGION_IMAGE_DATA ||
         region.kind == MINT_ASPACE_REGION_STACK)) {
        uint32_t have = region.flags & (MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC);
        return have == (flags & (MINT_PAGE_MAP_WRITE | MINT_PAGE_MAP_EXEC)) ? 0 : -MINT_EACCES;
    }
    return mint_address_space_region_protect(&p->address_space, address, size, flags) ? 0 : -MINT_ENOMEM;
}

uint64_t mint_proc_brk(MINT_PROCESS *p, uint64_t new_end) {
    if (!p || !p->brk_base) return 0;
    if (!new_end || new_end < p->brk_base || new_end > p->brk_base + MINT_BRK_MAX) return p->brk_current;
    uint64_t old_size = (p->brk_current - p->brk_base + PAGE - 1u) & ~(PAGE - 1u);
    uint64_t new_size = (new_end - p->brk_base + PAGE - 1u) & ~(PAGE - 1u);
    MINT_ADDRESS_SPACE *space = &p->address_space;
    if (new_size != old_size) {
        int ok;
        if (!old_size) ok = mint_address_space_region_add(space, p->brk_base, new_size, MINT_PAGE_MAP_WRITE,
                                                          MINT_ASPACE_REGION_ANONYMOUS);
        else if (!new_size) ok = mint_address_space_region_carve(space, p->brk_base, old_size);
        else ok = mint_address_space_region_resize(space, p->brk_base, new_size);
        if (!ok) return p->brk_current;
    }
    p->brk_current = new_end;
    return new_end;
}
