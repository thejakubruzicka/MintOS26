#include <stdint.h>
#include "include/boot_abi.h"
#include "include/arch.h"
#include "include/physmem.h"
#include "include/serial.h"
#include "include/task.h"
#include "include/syscall.h"
#include "include/e1000.h"
#include "include/uapi/mintos/syscall.h"
#include "include/paging.h"
#include "include/heap.h"
#include "include/vmalloc.h"
#include "include/address_space.h"
#include "include/elf_loader.h"
#include "include/process.h"
#include "include/desktop_abi.h"
#include "include/input.h"
#include "include/pci.h"
#include "include/storage_diag.h"
#include "include/log.h"
#include "include/power.h"
#include "include/rtc.h"
#include "include/usb.h"
#include "include/net.h"
#include "include/desktop_services.h"
#include "include/ramfs.h"
#include "include/mintos_fs.h"

/* Early native kernel core. This code runs only after ExitBootServices and
 * establishes its own 4-level identity page map before allocating the heap. */
#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

static const uint8_t font5x7[40][7] = {
 {0,0,0,0,0,0,0}, /* space */
 {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
 {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
 {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
 {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
 {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
 {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
 {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
 {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
 {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
 {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
 {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
 {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
 {4,4,4,4,4,0,4}, {0,0,0,31,0,0,0},
 {0,0,0,0,0,4,8}
};

static int glyph_index(char c) {
    if (c==' ') return 0;
    if (c>='A' && c<='Z') return 1+(c-'A');
    if (c>='0' && c<='9') return 27+(c-'0');
    if (c==':') return 37;
    if (c=='-') return 38;
    if (c=='.') return 39;
    return 0;
}

static MINT_DISPLAY_SURFACE kernel_surface;

static void rect(volatile uint32_t *fb,const MINT_FRAMEBUFFER_INFO *f,
                 int x,int y,int w,int h,uint32_t color) {
    (void)fb; (void)f;
    mint_display_fill_rect(&kernel_surface, x, y, w, h, color);
}
static void text(volatile uint32_t *fb,const MINT_FRAMEBUFFER_INFO *f,
                 int x,int y,const char *s,int scale,uint32_t color) {
    for (;*s;s++,x+=6*scale) {
        int gi=glyph_index(*s);
        for(int gy=0;gy<7;gy++) for(int gx=0;gx<5;gx++) {
            if(font5x7[gi][gy]&(1u<<(4-gx)))
                rect(fb,f,x+gx*scale,y+gy*scale,scale,scale,color);
        }
    }
}
static void centered(volatile uint32_t *fb,const MINT_FRAMEBUFFER_INFO *f,
                     int y,const char *s,int scale,uint32_t color) {
    int len=0;for(const char*p=s;*p;p++)len++;
    int x=((int)f->width-len*6*scale+scale)/2;
    text(fb,f,x,y,s,scale,color);
}


static const MINT_FRAMEBUFFER_INFO *active_framebuffer;
static volatile uint32_t *active_pixels;
static int active_panel_x, active_panel_y, active_panel_w;

static void decimal(char *out, uint64_t v) {
    char tmp[21]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < (int)sizeof(tmp));
    int i = 0; while (n) out[i++] = tmp[--n]; out[i] = 0;
}

static MINT_ADDRESS_SPACE address_space_test_a;
static MINT_ADDRESS_SPACE address_space_test_b;
static volatile uint32_t native_desktop_active;
static volatile uint32_t ring3_test_state; /* 0=pending, 1=running, 2=all passed, 3=any failed */
static volatile uint32_t ring3_tests_completed;
static volatile uint32_t ring3_tests_failed;
#define MINT_RING3_TEST_WORKERS 2u
extern const uint8_t mint_user_elf_start[];
extern const uint8_t mint_user_elf_end[];
extern const uint8_t mint_desktop_elf_start[];
extern const uint8_t mint_desktop_elf_end[];

static uint64_t address_space_claim_page(void *context) {
    (void)context;
    return mint_physmem_claim_page();
}
static int address_space_release_page(void *context, uint64_t pa) {
    (void)context;
    return mint_physmem_release_contiguous(pa, 1);
}
static void *address_space_physical_to_pointer(void *context, uint64_t pa) {
    (void)context;
    if (pa >= MINT_IDENTITY_MAP_LIMIT || (pa & (MINT_PAGING_PAGE_SIZE - 1u))) return 0;
    return (void *)(uintptr_t)pa;
}
static int address_space_page_is_claimed(void *context, uint64_t pa) {
    (void)context;
    return mint_physmem_is_claimed(pa, 1);
}
static void address_space_load_root(void *context, uint64_t root) {
    (void)context;
    mint_arch_load_cr3(root);
}
static void address_space_invalidate(void *context, uint64_t va) {
    (void)context;
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)va) : "memory");
}

/* A boot-time two-root test: map the same user VA to different owned frames,
   switch roots while continuing to execute supervisor code at low identity
   addresses, and verify each root sees only its own contents. */
static int address_space_isolation_selftest(void) {
    uint64_t shared_low[MINT_ASPACE_SHARED_PDPT_ENTRIES];
    uint64_t shared_high = 0, kernel_root = 0;
    MINT_ADDRESS_SPACE_BACKEND backend = {
        .context = 0,
        .claim_page = address_space_claim_page,
        .release_page = address_space_release_page,
        .physical_to_pointer = address_space_physical_to_pointer,
        .is_claimed_single_page = address_space_page_is_claimed,
        .load_root = address_space_load_root,
        .invalidate_page = address_space_invalidate
    };
    for (size_t i = 0; i < sizeof(address_space_test_a); ++i)
        ((uint8_t *)&address_space_test_a)[i] = 0;
    for (size_t i = 0; i < sizeof(address_space_test_b); ++i)
        ((uint8_t *)&address_space_test_b)[i] = 0;
    if (!mint_paging_export_address_space_template(shared_low, &shared_high, &kernel_root) ||
        !mint_address_space_system_init(&backend, kernel_root, shared_low, shared_high,
                                        mint_paging_nx_is_active())) {
        mint_serial_write("[aspace] unable to initialize address-space manager\n");
        return 0;
    }

    uint64_t free_before = mint_physmem_free_pages();
    int made_a = 0, made_b = 0, pass = 1;
    int region_a_reserved = 0, region_b_reserved = 0, region_b_probe_reserved = 0;
    int mapped_a = 0, mapped_b = 0;
    const char *failure = "unknown failure";
#define AS_CHECK(expr, reason) do { \
        if (pass && !(expr)) { pass = 0; failure = reason; } \
    } while (0)
    AS_CHECK((made_a = mint_address_space_create(&address_space_test_a)), "create root A");
    AS_CHECK((made_b = mint_address_space_create(&address_space_test_b)), "create root B");

    uint64_t frame_a = 0, frame_b = 0;
    const uint64_t user_va = MINT_ASPACE_USER_BASE + 0x00200000ULL;
    /* A second, separately reserved B page makes the cross-owner rejection
       test meaningful: region policy itself must not be the reason it fails. */
    const uint64_t user_probe_va = user_va + MINT_PAGING_PAGE_SIZE;
    if (pass) {
        region_a_reserved = mint_address_space_region_reserve(&address_space_test_a, user_va,
            MINT_PAGING_PAGE_SIZE, MINT_PAGE_MAP_WRITE, MINT_ASPACE_REGION_ANONYMOUS);
        AS_CHECK(region_a_reserved, "reserve root A test region");
    }
    if (pass) {
        region_b_reserved = mint_address_space_region_reserve(&address_space_test_b, user_va,
            MINT_PAGING_PAGE_SIZE, MINT_PAGE_MAP_WRITE, MINT_ASPACE_REGION_ANONYMOUS);
        AS_CHECK(region_b_reserved, "reserve root B test region");
    }
    if (pass) {
        region_b_probe_reserved = mint_address_space_region_reserve(&address_space_test_b, user_probe_va,
            MINT_PAGING_PAGE_SIZE, MINT_PAGE_MAP_WRITE, MINT_ASPACE_REGION_ANONYMOUS);
        AS_CHECK(region_b_probe_reserved, "reserve cross-owner probe region");
    }

    const uint64_t value_a = 0x4D494E5441535041ULL; /* MINTASPA */
    const uint64_t value_b = 0x4D494E5441535042ULL; /* MINTASPB */
    const uint64_t changed_a = 0x4144445253504143ULL;
    const uint64_t changed_b = 0x49534F4C41544544ULL;
    if (pass) {
        frame_a = mint_address_space_alloc_frame(&address_space_test_a);
        frame_b = mint_address_space_alloc_frame(&address_space_test_b);
        AS_CHECK(frame_a && frame_b && frame_a != frame_b, "allocate distinct owned frames");
    }
    if (pass) {
        *(volatile uint64_t *)(uintptr_t)frame_a = value_a;
        *(volatile uint64_t *)(uintptr_t)frame_b = value_b;
        mapped_a = mint_address_space_map(&address_space_test_a, user_va, frame_a, MINT_PAGE_MAP_WRITE);
        AS_CHECK(mapped_a, "map root A's frame");
    }
    if (pass) {
        mapped_b = mint_address_space_map(&address_space_test_b, user_va, frame_b, MINT_PAGE_MAP_WRITE);
        AS_CHECK(mapped_b, "map root B's frame");
    }

    uint64_t pa_a = 0, pa_b = 0;
    uint32_t flags_a = 0, flags_b = 0;
    if (pass) {
        AS_CHECK(mint_address_space_lookup(&address_space_test_a, user_va, &pa_a, &flags_a) &&
                 mint_address_space_lookup(&address_space_test_b, user_va, &pa_b, &flags_b) &&
                 pa_a == frame_a && pa_b == frame_b && pa_a != pa_b &&
                 (flags_a & MINT_PAGE_MAP_WRITE) && (flags_b & MINT_PAGE_MAP_WRITE),
                 "page-table lookup and permissions");
    }
    if (pass) {
        AS_CHECK(!mint_address_space_map(&address_space_test_b, user_probe_va, frame_a,
                                         MINT_PAGE_MAP_WRITE), "reject cross-address-space frame mapping");
    }

    uint64_t *root_a = (uint64_t *)(uintptr_t)mint_address_space_root(&address_space_test_a);
    uint64_t *root_b = (uint64_t *)(uintptr_t)mint_address_space_root(&address_space_test_b);
    if (pass) {
        AS_CHECK(root_a && root_b && root_a != root_b &&
                 root_a[256] == shared_high && root_b[256] == shared_high &&
                 (root_a[0] & 7ULL) == 7ULL && (root_b[0] & 7ULL) == 7ULL &&
                 !(root_a[256] & (1ULL << 2)) && !(root_b[256] & (1ULL << 2)),
                 "independent roots and supervisor-only shared direct map");
    }
    if (pass) AS_CHECK(mint_address_space_activate(&address_space_test_a), "activate root A");
    if (pass) AS_CHECK(*(volatile uint64_t *)(uintptr_t)user_va == value_a, "root A sees its own data");
    if (pass) *(volatile uint64_t *)(uintptr_t)user_va = changed_a;
    if (pass) AS_CHECK(mint_address_space_activate(&address_space_test_b), "activate root B");
    if (pass) AS_CHECK(*(volatile uint64_t *)(uintptr_t)user_va == value_b, "root B sees isolated data");
    if (pass) *(volatile uint64_t *)(uintptr_t)user_va = changed_b;

    /* Always return to the kernel root before cleanup or inspecting frames. */
    if (!mint_address_space_activate_kernel()) {
        if (pass) failure = "restore kernel CR3";
        pass = 0;
    }
    if (pass) {
        AS_CHECK(*(volatile uint64_t *)(uintptr_t)frame_a == changed_a &&
                 *(volatile uint64_t *)(uintptr_t)frame_b == changed_b,
                 "isolated writes persisted in the correct physical frames");
    }

    /* Cleanup is best-effort but the original failing stage is preserved. */
    uint64_t unmapped = 0;
    uint32_t old_flags = 0;
    if (address_space_test_a.initialized && address_space_test_a.mappings_live) {
        if (!mint_address_space_unmap(&address_space_test_a, user_va, &unmapped, &old_flags) ||
            (frame_a && unmapped != frame_a)) {
            if (pass) failure = "unmap root A during cleanup";
            pass = 0;
        }
    }
    if (address_space_test_b.initialized && address_space_test_b.mappings_live) {
        if (!mint_address_space_unmap(&address_space_test_b, user_va, &unmapped, &old_flags) ||
            (frame_b && unmapped != frame_b)) {
            if (pass) failure = "unmap root B during cleanup";
            pass = 0;
        }
    }
    if (region_b_probe_reserved &&
        !mint_address_space_region_release(&address_space_test_b, user_probe_va)) {
        if (pass) failure = "release cross-owner probe region";
        pass = 0;
    }
    if (region_a_reserved && !mint_address_space_region_release(&address_space_test_a, user_va)) {
        if (pass) failure = "release root A region";
        pass = 0;
    }
    if (region_b_reserved && !mint_address_space_region_release(&address_space_test_b, user_va)) {
        if (pass) failure = "release root B region";
        pass = 0;
    }
    if (made_b && !mint_address_space_destroy(&address_space_test_b)) {
        if (pass) failure = "destroy root B";
        pass = 0;
    }
    if (made_a && !mint_address_space_destroy(&address_space_test_a)) {
        if (pass) failure = "destroy root A";
        pass = 0;
    }
    if (mint_physmem_free_pages() != free_before) {
        if (pass) failure = "physical-page leak after cleanup";
        pass = 0;
    }
    if (pass) {
        mint_serial_write("[aspace] independent roots, isolated user mappings, CR3 switch and cleanup passed\n");
    } else {
        mint_serial_write("[aspace] FAILED stage: ");
        mint_serial_write(failure);
        mint_serial_write("\n");
    }
#undef AS_CHECK
    return pass;
}

static void draw_early_kernel(const MINT_BOOT_INFO *boot, int memory_ok, uint64_t reserved_page,
                              int paging_ok, int nx_ok, int heap_ok, int maptest_ok,
                              int vmalloc_ok, int vmalloc_test_ok, int aspace_test_ok,
                              int desktop_core_ok) {
    const MINT_FRAMEBUFFER_INFO *f = &boot->framebuffer;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)f->base;
    rect(fb, f, 0, 0, (int)f->width, (int)f->height, RGB(12,24,40));
    rect(fb, f, 0, 0, (int)f->width, 10, RGB(25,203,183));
    int panel_w = (int)f->width > 1300 ? 1050 : (int)f->width - 60;
    int panel_h = (int)f->height > 700 ? 700 : (int)f->height - 40;
    if (panel_w < 300) panel_w = (int)f->width;
    if (panel_h < 300) panel_h = (int)f->height;
    int px = ((int)f->width - panel_w) / 2, py = ((int)f->height - panel_h) / 2;
    active_panel_x = px; active_panel_y = py; active_panel_w = panel_w;
    rect(fb, f, px, py, panel_w, panel_h, RGB(25,47,68));
    rect(fb, f, px, py, panel_w, 5, RGB(25,203,183));
    int scale = f->width >= 1200 ? 3 : 2;
    centered(fb, f, py + 42, "MINTOS26 KERNEL 26.5.004.0", scale, RGB(244,250,255));
    centered(fb, f, py + 100, "KERNEL CORE ONLINE", 2, RGB(103,232,196));
    centered(fb, f, py + 142, "OWN GDT IDT INSTALLED", 2, RGB(223,237,245));
    centered(fb, f, py + 180, "CPU EXCEPTION HANDLERS ARMED", 2, RGB(223,237,245));
    centered(fb, f, py + 218, "UEFI BOOT SERVICES EXITED", 2, RGB(223,237,245));

    char n[24];
    decimal(n, boot->memory_map_size / boot->memory_descriptor_size);
    char line[80] = "MEMORY MAP DESCRIPTORS: ";
    int k = 24; for (int i = 0; n[i] && k < (int)sizeof(line)-1; ++i) line[k++] = n[i]; line[k] = 0;
    centered(fb, f, py + 270, line, 1, memory_ok ? RGB(103,232,196) : RGB(255,143,143));
    decimal(n, mint_physmem_free_pages());
    char line2[80] = "FREE PHYSICAL PAGES: ";
    k = 21; for (int i = 0; n[i] && k < (int)sizeof(line2)-1; ++i) line2[k++] = n[i]; line2[k] = 0;
    centered(fb, f, py + 300, line2, 1, memory_ok ? RGB(223,237,245) : RGB(255,143,143));
    centered(fb, f, py + 330, reserved_page ? "PHYSICAL PAGE CLAIMED" : "PAGE CLAIM FAILED", 1,
             reserved_page ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 346, paging_ok ? "64 GIB IDENTITY + HIGH DIRECT MAP ACTIVE" : "FIRMWARE PAGE MAP RETAINED", 1,
             paging_ok ? RGB(103,232,196) : RGB(255,190,130));
    centered(fb, f, py + 362, heap_ok ? (nx_ok ? "NX PROTECTION AND KERNEL HEAP READY" : "HEAP READY - NX NOT SUPPORTED")
                                        : "KERNEL HEAP UNAVAILABLE", 1,
             heap_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 378, "TSS/TR INSTALLED; DF/NMI/MC IST ARMED", 1, RGB(103,232,196));
    centered(fb, f, py + 394, "8 STACK GUARD PAGES ACTIVE", 1, RGB(103,232,196));
    centered(fb, f, py + 410, maptest_ok ? "PAGE MAP/UNMAP + HIGH ALIAS TEST PASSED" : "PAGE MAP/UNMAP OR ALIAS TEST FAILED", 1, maptest_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 426, vmalloc_ok ? "16 MiB VIRTUAL ARENA READY" : "VIRTUAL ARENA UNAVAILABLE", 1, vmalloc_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 442, vmalloc_test_ok ? "VMALLOC ALLOC/MAP/FREE TEST PASSED" : "VMALLOC SELF-TEST NOT PASSED", 1, vmalloc_test_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 458, aspace_test_ok ? "INDEPENDENT ADDRESS-SPACE TEST PASSED" : "ADDRESS-SPACE TEST NOT PASSED", 1, aspace_test_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 474, "RING-3 INT-0x80 USER TEST: PENDING", 1, RGB(181,201,215));
    rect(fb, f, px + 40, py + 490, panel_w - 80, 2, RGB(62,91,111));
    centered(fb, f, py + 494, "SERIAL COM1 LOGGING INITIALIZED", 2, RGB(103,232,196));
    centered(fb, f, py + 514, "MASKABLE INTERRUPTS PREPARING", 1, RGB(181,201,215));
    centered(fb, f, py + 530, "PIT IRQ0 TIMER NOT STARTED", 1, RGB(181,201,215));
    centered(fb, f, py + 546, "TIMER TICKS: WAITING", 1, RGB(181,201,215));
    centered(fb, f, py + 632, desktop_core_ok ? "KERNEL + DESKTOP CORE FOUNDATION" : "DESKTOP CORE INIT FAILED", 2, desktop_core_ok ? RGB(103,232,196) : RGB(255,143,143));
    centered(fb, f, py + 662, "RESET VM TO RETURN TO DESKTOP", 1, RGB(181,201,215));
}


static void draw_runtime_status(uint64_t ticks, int received, uint64_t active_tasks,
                                uint64_t total_created, uint64_t total_exited) {
    if (!active_framebuffer || !active_pixels) return;
    const MINT_FRAMEBUFFER_INFO *f = active_framebuffer;
    int x = active_panel_x + 28;
    int y = active_panel_y + 496;
    int w = active_panel_w - 56;
    rect(active_pixels, f, x, y, w, 118, RGB(25,47,68));
    centered(active_pixels, f, y + 3, "MASKABLE INTERRUPTS ENABLED", 1, RGB(181,201,215));
    centered(active_pixels, f, y + 19, received ? "PIT IRQ0 TIMER RUNNING" : "PIT IRQ0 ARMED WAITING FOR FIRST TICK", 1,
             received ? RGB(103,232,196) : RGB(181,201,215));
    char n[24]; decimal(n, ticks);
    char line[48] = "TIMER TICKS: ";
    int k = 13;
    for (int i = 0; n[i] && k < (int)sizeof(line)-1; ++i) line[k++] = n[i];
    line[k] = 0;
    centered(active_pixels, f, y + 35, line, 1, received ? RGB(103,232,196) : RGB(181,201,215));
    centered(active_pixels, f, y + 51, "COOPERATIVE SCHEDULER ACTIVE", 1, RGB(103,232,196));
    char task_line[48] = "LIVE TASKS: ";
    decimal(n, active_tasks); k = 12;
    for (int i = 0; n[i] && k < (int)sizeof(task_line)-1; ++i) task_line[k++] = n[i];
    task_line[k] = 0;
    centered(active_pixels, f, y + 67, task_line, 1, RGB(181,201,215));
    char lifecycle[64] = "CREATED: ";
    decimal(n, total_created); k = 9;
    for (int i = 0; n[i] && k < (int)sizeof(lifecycle)-1; ++i) lifecycle[k++] = n[i];
    const char *mid = "  EXITED: ";
    for (int i = 0; mid[i] && k < (int)sizeof(lifecycle)-1; ++i) lifecycle[k++] = mid[i];
    decimal(n, total_exited);
    for (int i = 0; n[i] && k < (int)sizeof(lifecycle)-1; ++i) lifecycle[k++] = n[i];
    lifecycle[k] = 0;
    centered(active_pixels, f, y + 83, lifecycle, 1, RGB(181,201,215));
    const char *ring3 = ring3_test_state == 1 ? "RING-3 INT-0x80: RUNNING" :
                        ring3_test_state == 2 ? "RING-3 INT-0x80: PASSED" :
                        ring3_test_state == 3 ? "RING-3 INT-0x80: FAILED" :
                                                "RING-3 INT-0x80: PENDING";
    centered(active_pixels, f, y + 99, ring3, 1,
             ring3_test_state == 2 ? RGB(103,232,196) :
             ring3_test_state == 3 ? RGB(255,143,143) : RGB(181,201,215));
}

static void status_task(void *argument) {
    (void)argument;
    for (;;) {
        mint_task_sleep_ticks(100);
        /* The native ring-3 shell owns the display after it starts. Keep the
           diagnostic panel only during boot tests or if shell startup fails. */
        if (native_desktop_active) continue;
        uint64_t now = mint_timer_ticks();
        draw_runtime_status(now, now != 0, mint_task_live_count(),
                            mint_task_total_created(), mint_task_total_exited());
    }
}

static int seed_desktop_runtime_events(void) {
    MINT_DESKTOP_EVENT event = {0};
    event.type = MINT_DESKTOP_EVENT_APP_REQUEST;
    event.timestamp_ms = 260053u;
    event.data.app.app_id = 0x4D494E54u; /* MINT */
    event.data.app.argument = 0x00260053u; /* release 26.3 */
    if (!mint_desktop_event_post(&event) || !mint_desktop_event_post(&event)) return 0;
    mint_serial_write("[desktop-core] seeded two bounded userspace event-service probes\n");
    return 1;
}

static void heartbeat_task(void *argument) {
    (void)argument;
    for (;;) {
        mint_task_sleep_ticks(500);
        mint_serial_write("[task] heartbeat pid=");
        mint_serial_dec64(mint_task_current_pid());
        mint_serial_write(" ticks=");
        mint_serial_dec64(mint_timer_ticks());
        mint_serial_write("\n");
    }
}

static void syscall_abi_smoke_task(void *argument) {
    (void)argument;
    static const char message[] = "[abi] Linux-inspired write syscall contract online\n";
    (void)mint_syscall_dispatch(MINT_SYS_WRITE, 1u, (uint64_t)(uintptr_t)message,
                                sizeof(message)-1u, 0, 0, 0);
    mint_serial_write("[abi] getpid returned ");
    mint_serial_dec64((uint64_t)mint_syscall_dispatch(MINT_SYS_GETPID,0,0,0,0,0,0));
    mint_serial_write("\n");
    mint_timespec64 ts = {0,0};
    int64_t rc = mint_syscall_dispatch(MINT_SYS_CLOCK_GETTIME, MINT_CLOCK_MONOTONIC,
                                       (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
    mint_serial_write("[abi] clock_gettime status=");
    mint_serial_dec64((uint64_t)rc);
    mint_serial_write(" uptime=");
    mint_serial_dec64((uint64_t)ts.tv_sec);
    mint_serial_write("s\n");
    /* A returning task is reaped by the scheduler. */
}


static void ring3_user_task(void *argument) {
    uint64_t worker = (uint64_t)(uintptr_t)argument;
    if (ring3_tests_completed == 0 && ring3_tests_failed == 0) ring3_test_state = 1;
    mint_serial_write("[ring3] worker ");
    mint_serial_dec64(worker);
    mint_serial_write(" creating an independent ELF64 process\n");
    const size_t elf_size = (size_t)(mint_user_elf_end - mint_user_elf_start);
    MINT_PROCESS *process = mint_process_create_current_from_elf(mint_user_elf_start, elf_size);
    int ok = process != 0;
    if (ok) ok = mint_process_run_current(process);
    if (ok) {
        ok = mint_process_state(process) == MINT_PROCESS_EXITED &&
             mint_process_exit_code(process) == 0;
    }
    if (ok) {
        mint_serial_write("[ring3] worker "); mint_serial_dec64(worker);
        mint_serial_write(" ELF64 lifecycle and resource reaping passed\n");
        ++ring3_tests_completed;
    } else {
        mint_serial_write("[ring3] worker "); mint_serial_dec64(worker);
        mint_serial_write(" ELF64 user-process lifecycle FAILED\n");
        ++ring3_tests_failed;
    }
    if (ring3_tests_failed) ring3_test_state = 3u;
    else if (ring3_tests_completed >= MINT_RING3_TEST_WORKERS) ring3_test_state = 2u;
    mint_serial_write(ok ? "[ring3] ELF64 USER PROCESS TEST PASSED\n"
                         : "[ring3] ELF64 USER PROCESS TEST FAILED\n");
    /* Two tasks run the same user ELF concurrently and yield between syscall
     * phases; global memory snapshots are intentionally avoided because each
     * process owns live pages while the other task is suspended. */
}

static void native_desktop_task(void *argument) {
    (void)argument;
    while (ring3_tests_completed < MINT_RING3_TEST_WORKERS && ring3_tests_failed == 0)
        mint_task_sleep_ticks(10);
    if (ring3_tests_failed || ring3_tests_completed < MINT_RING3_TEST_WORKERS) {
        mint_serial_write("[desktop-ui] native shell launch withheld because a ring-3 smoke test failed\n");
        return;
    }

    mint_serial_write("[desktop-ui] launching native shell as a persistent ring-3 process\n");
    const size_t elf_size = (size_t)(mint_desktop_elf_end - mint_desktop_elf_start);
    MINT_PROCESS *process = mint_process_create_current_from_elf(mint_desktop_elf_start, elf_size);
    if (!process) {
        mint_serial_write("[desktop-ui] user ELF load failed; keeping kernel status display\n");
        return;
    }
    native_desktop_active = 1u;
    if (!mint_process_run_current(process)) {
        native_desktop_active = 0u;
        mint_serial_write("[desktop-ui] native shell exited unexpectedly; diagnostic display resumed\n");
    }
}

/* Size the retained frame and glass backdrop from the real GOP mode instead of
 * a fixed 1920x1080 array, so padded strides and modes up to 4K boot. Buffers
 * come from the top of RAM to keep low memory free for 32-bit DMA devices. */
static int attach_display_backbuffers(const MINT_FRAMEBUFFER_INFO *f) {
    uint64_t pixels = mint_display_backbuffer_pixels_required();
    mint_serial_write("[display] GOP mode ");
    mint_serial_dec64(f->width); mint_serial_write("x"); mint_serial_dec64(f->height);
    mint_serial_write(" stride="); mint_serial_dec64(f->pixels_per_scanline);
    mint_serial_write(" format="); mint_serial_dec64(f->pixel_format);
    mint_serial_write(" base="); mint_serial_hex64(f->base);
    mint_serial_write("\n");
    if (!pixels) return 0;
    uint64_t pages = (pixels * sizeof(uint32_t) + MINT_PAGE_SIZE - 1u) / MINT_PAGE_SIZE;
    uint64_t back = mint_physmem_claim_contiguous_high(pages);
    uint64_t backdrop = back ? mint_physmem_claim_contiguous_high(pages) : 0;
    if (!back || !backdrop ||
        !mint_display_attach_backbuffers((uint32_t *)(uintptr_t)back,
                                         (uint32_t *)(uintptr_t)backdrop,
                                         pages * MINT_PAGE_SIZE / sizeof(uint32_t))) {
        if (backdrop) (void)mint_physmem_release_contiguous(backdrop, pages);
        if (back) (void)mint_physmem_release_contiguous(back, pages);
        mint_serial_write("[display] could not reserve backbuffers; drawing directly to scanout\n");
        return 0;
    }
    /* Third same-size buffer: MintDE's compositor scratch (blur, offscreen). */
    uint64_t scratch = mint_physmem_claim_contiguous_high(pages);
    if (!scratch || !mint_display_attach_scratch((uint32_t *)(uintptr_t)scratch,
                                                pages * MINT_PAGE_SIZE / sizeof(uint32_t))) {
        if (scratch) (void)mint_physmem_release_contiguous(scratch, pages);
        mint_serial_write("[display] compositor scratch buffer unavailable; MintDE canvas disabled\n");
    }
    mint_serial_write("[display] retained backbuffer + backdrop attached: 2 x ");
    mint_serial_dec64(pages * MINT_PAGE_SIZE / 1024u);
    mint_serial_write(" KiB at "); mint_serial_hex64(back);
    mint_serial_write(" / "); mint_serial_hex64(backdrop); mint_serial_write("\n");
    return 1;
}

void mint_exception_display(uint64_t vector, uint64_t rip, uint64_t error) {
    if (!active_framebuffer || !active_pixels) return;
    const MINT_FRAMEBUFFER_INFO *f = active_framebuffer;
    rect(active_pixels, f, 0, 0, (int)f->width, (int)f->height, RGB(31,10,22));
    rect(active_pixels, f, 0, 0, (int)f->width, 10, RGB(240,80,105));
    centered(active_pixels, f, (int)f->height/2 - 100, "MINTOS26 KERNEL PANIC", 3, RGB(255,240,244));
    centered(active_pixels, f, (int)f->height/2 - 30, "CPU EXCEPTION", 2, RGB(255,143,160));
    char n[24]; decimal(n, vector); char line[64] = "VECTOR: "; int j=8;
    for(int i=0;n[i]&&j<(int)sizeof(line)-1;i++)line[j++]=n[i];line[j]=0;
    centered(active_pixels, f, (int)f->height/2 + 10, line, 2, RGB(255,240,244));
    (void)rip; (void)error;
    centered(active_pixels, f, (int)f->height/2 + 60, "DETAILS WRITTEN TO COM1", 1, RGB(255,190,200));
    centered(active_pixels, f, (int)f->height/2 + 95, "RESET VM TO REBOOT", 1, RGB(255,190,200));
}

__attribute__((section(".text.entry"),used,noreturn,ms_abi))
void kernel_entry(const MINT_BOOT_INFO *boot) {
    __asm__ volatile("cli; cld" ::: "memory");
    mint_serial_init();
    mint_log_init();
    mint_serial_write("\nMintOS26 26.5.013.0 native kernel entry\n");
    if (!boot || boot->magic != MINT_BOOT_INFO_MAGIC ||
        boot->abi_version != MINT_BOOT_INFO_ABI ||
        boot->struct_size < sizeof(MINT_BOOT_INFO)) {
        mint_serial_write("[boot] invalid handoff ABI; halted\n");
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    }
    const MINT_FRAMEBUFFER_INFO *f = &boot->framebuffer;
    if (!mint_display_surface_init(&kernel_surface, f)) {
        mint_serial_write("[boot] invalid/unsupported framebuffer surface; halted\n");
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    }
    active_framebuffer = f;
    active_pixels = (volatile uint32_t *)(uintptr_t)f->base;
    mint_arch_init();
    (void)mint_pci_enumerate();
    int memory_ok = mint_physmem_init(boot);
    int paging_ok = mint_paging_init(boot);
    if (paging_ok) mint_storage_diag_init();
    else mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AHCI,
                       "storage probe skipped because kernel paging initialization failed");
    int arena_reserved = paging_ok && mint_physmem_reserve_range(MINT_VMALLOC_BASE, MINT_VMALLOC_PAGE_COUNT);
    if (paging_ok && !arena_reserved) mint_serial_write("[vmalloc] could not reserve arena physical overlap\n");
    uint64_t reserved_page = memory_ok && arena_reserved ? mint_physmem_claim_page() : 0;
    if (reserved_page) {
        mint_serial_write("[mm] claimed test physical frame at ");
        mint_serial_hex64(reserved_page);
        mint_serial_write("\n");
    }
    int maptest_ok = paging_ok && reserved_page && mint_paging_selftest_page(reserved_page);
    mint_serial_write(maptest_ok ? "[paging] dynamic map/unmap and higher-half alias self-test passed\n"
                                  : "[paging] dynamic map/unmap or higher-half alias self-test did not pass\n");
    uint64_t heap_base = 0;
    int heap_ok = 0;
    if (paging_ok) {
        const uint64_t heap_pages = 64u; /* 256 KiB initial kernel heap */
        heap_base = mint_physmem_claim_contiguous(heap_pages);
        if (heap_base && mint_heap_init((void *)(uintptr_t)heap_base,
                                        heap_pages * MINT_PAGE_SIZE)) {
            heap_ok = 1;
            mint_serial_write("[heap] 256 KiB contiguous identity-mapped kernel heap online at ");
            mint_serial_hex64(heap_base);
            mint_serial_write("\n");
        } else {
            mint_serial_write("[heap] unable to reserve 256 KiB contiguous heap\n");
        }
    } else {
        mint_serial_write("[paging] retaining firmware tables; page-backed heap disabled\n");
    }
    if (paging_ok) {
        /* Real GPUs (e.g. a GOP framebuffer on a discrete PCIe card) are many
           times faster to present to when write-combined than uncached. */
        if (mint_paging_set_framebuffer_write_combining(f->base, f->size))
            mint_serial_write("[display] framebuffer remapped write-combining via PAT\n");
        else
            mint_serial_write("[display] PAT write-combining unavailable; framebuffer stays uncached\n");
        (void)attach_display_backbuffers(f);
    }
    /* ACPI tables are only read; nothing is written until a confirmed
       shutdown/restart request. Works without paging via firmware tables. */
    (void)mint_power_init(boot->acpi_rsdp_physical);
    int vmalloc_ok = arena_reserved && mint_vmalloc_init();
    int vmalloc_test_ok = vmalloc_ok && mint_vmalloc_selftest();
    mint_serial_write(vmalloc_ok ? "[vmalloc] transactional physical release path ready\n"
                                 : "[vmalloc] transactional release unavailable\n");
    mint_serial_write("[paging] dynamic PT active/peak/reclaimed=");
    mint_serial_dec64(mint_paging_dynamic_tables_used()); mint_serial_write("/");
    mint_serial_dec64(mint_paging_dynamic_tables_peak()); mint_serial_write("/");
    mint_serial_dec64(mint_paging_dynamic_tables_reclaimed()); mint_serial_write("\n");
    mint_serial_write("[vmalloc] used/peak pages=");
    mint_serial_dec64(mint_vmalloc_used_pages()); mint_serial_write("/");
    mint_serial_dec64(mint_vmalloc_peak_pages()); mint_serial_write(" allocations/frees=");
    mint_serial_dec64(mint_vmalloc_total_allocations()); mint_serial_write("/");
    mint_serial_dec64(mint_vmalloc_total_frees()); mint_serial_write(" failed/rollback/free-errors=");
    mint_serial_dec64(mint_vmalloc_failed_allocations()); mint_serial_write("/");
    mint_serial_dec64(mint_vmalloc_rollback_count()); mint_serial_write("/");
    mint_serial_dec64(mint_vmalloc_failed_frees()); mint_serial_write("\n");
    int aspace_test_ok = vmalloc_ok && vmalloc_test_ok && address_space_isolation_selftest();
    int desktop_core_ok = mint_desktop_core_init();
    mint_desktop_settings_init();
    mint_ramfs_init();
    mint_serial_write("[settings] versioned theme/accent/scale/wallpaper/reduced-motion API ready (session memory)\n");
    mint_serial_write("[vfs] session RAM filesystem online files=3 max-files=8 bytes-per-file=4096 (volatile across reboot)\n");
    int desktop_probes_ok = desktop_core_ok && seed_desktop_runtime_events();
    mint_serial_write(desktop_core_ok ? "[desktop-core] settings service + session RAM filesystem + bounded display/event services ready\n"
                                      : "[desktop-core] event/display initialization failed\n");
    if (!desktop_probes_ok) mint_serial_write("[desktop-core] userspace event-service probe seeding failed\n");
    /* Boot performs only read-only PCI match discovery. Bus mastering and DMA
       remain disabled until the user explicitly runs Terminal: net start. */
    (void)mint_e1000_probe();
    draw_early_kernel(boot, memory_ok, reserved_page, paging_ok,
                      mint_paging_nx_is_active(), heap_ok, maptest_ok,
                      vmalloc_ok, vmalloc_test_ok, aspace_test_ok, desktop_core_ok);
    (void)mint_rtc_init();
    mint_timer_init();
    (void)mint_input_init();
    mint_serial_write("[kernel] enabling maskable interrupts after IDT/PIC/PIT/input setup\n");
    /* Scheduling remains cooperative. PS/2 input IRQs now feed the same bounded
     * event ABI consumed by the ring-3 native desktop. */
    mint_process_system_init();
    mint_task_init();
    int64_t status_pid = mint_task_create("desktop-status", status_task, 0);
    int64_t heartbeat_pid = mint_task_create("heartbeat", heartbeat_task, 0);
    int64_t abi_pid = mint_task_create("abi-smoke", syscall_abi_smoke_task, 0);
    int64_t ring3_pid = mint_task_create("ring3-smoke-a", ring3_user_task, (void *)(uintptr_t)1u);
    int64_t ring3_pid_b = mint_task_create("ring3-smoke-b", ring3_user_task, (void *)(uintptr_t)2u);
    int64_t desktop_pid = mint_task_create("native-desktop", native_desktop_task, 0);
    int64_t usb_pid = mint_task_create("usb", mint_usb_task, 0);
    if (usb_pid < 0) mint_serial_write("[usb] could not create the USB polling task; USB input disabled\n");
    int64_t net_pid = mint_task_create("net", mint_net_task, 0);
    if (net_pid < 0) mint_serial_write("[net] could not create the network task; networking disabled\n");
    /* MintOS disk: mount after the vmalloc arena and DMA users have claimed their
       memory so the bitmap pages never overlap them. */
    if (paging_ok && arena_reserved) mint_mintos_disk_probe();
    mint_power_set_flush_hook(mint_mfs_flush_for_power);     /* safe shutdown: final MintFS commit */
    int64_t fs_pid = mint_task_create("fs", mint_mfs_task, 0);
    if (fs_pid < 0) mint_serial_write("[mintfs] could not create the commit task; files commit on close only\n");
    if (status_pid < 0 || heartbeat_pid < 0 || abi_pid < 0 || ring3_pid < 0 || ring3_pid_b < 0 || desktop_pid < 0) {
        mint_serial_write("[sched] task creation failed; halted\n");
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    }
    draw_runtime_status(0, 0, mint_task_live_count(),
                        mint_task_total_created(), mint_task_total_exited());
    mint_serial_write("[kernel] enabling interrupts and entering scheduler\n");
    __asm__ volatile("sti" ::: "memory");
    mint_scheduler_run();
}
