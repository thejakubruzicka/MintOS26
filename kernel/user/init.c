#include "include/mintos/runtime.h"

#define MINT_RUNTIME_SMOKE_APP_ID 0x4D494E54u
#define MINT_RUNTIME_SMOKE_ARGUMENT 0x00260053u

static const char banner[] = "[user-runtime] libmint runtime v1 online\n";
static const char passed[] = "[user-runtime] syscall wrappers, event poll and display-info passed\n";
static volatile uint64_t runtime_probe_state;

static int write_all(const char *text, size_t length) {
    return mint_write(1, text, length) == (int64_t)length;
}

int64_t mint_user_main(void) {
    runtime_probe_state = 1u;
    if (!write_all(banner, sizeof(banner) - 1u)) return 1;
    runtime_probe_state = 2u;
    if (mint_sched_yield() != 0) return 2;
    runtime_probe_state = 3u;
    if (mint_getpid() <= 0) return 3;

    /* Probe user-copy validation without dereferencing the deliberately bad VA. */
    if (mint_write(1, (const void *)(uintptr_t)0x1000230000ULL, 1) != -MINT_EFAULT)
        return 4;

    mint_timespec64 now = {0, 0};
    if (mint_clock_gettime(MINT_CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000LL)
        return 5;

    MINT_DISPLAY_INFO display = {0};
    if (mint_display_get_info(&display) != 0 || !display.width || !display.height ||
        !display.stride_pixels || display.stride_pixels < display.width ||
        display.width > 16384u || display.height > 16384u)
        return 6;
    runtime_probe_state = ((uint64_t)display.width << 32) | display.height;

    MINT_DESKTOP_EVENT event = {0};
    if (mint_desktop_event_poll(&event) != 1 ||
        event.type != MINT_DESKTOP_EVENT_APP_REQUEST ||
        event.data.app.app_id != MINT_RUNTIME_SMOKE_APP_ID ||
        event.data.app.argument != MINT_RUNTIME_SMOKE_ARGUMENT)
        return 7;

    if (mint_sched_yield() != 0) return 8;
    runtime_probe_state ^= event.timestamp_ms;
    if (!write_all(passed, sizeof(passed) - 1u)) return 9;
    runtime_probe_state = 0x600D0053u;
    return 0;
}
