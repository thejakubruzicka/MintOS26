/* Notification queue — see notify.h. */
#include <stdint.h>
#include <stddef.h>
#include "notify.h"

static mde_notification queue[NOTIFY_MAX];     /* queue[0] is the newest */
static uint32_t count, next_id;

static void copy_trunc(char *out, uint32_t cap, const char *in) {
    uint32_t i = 0;
    if (in) while (in[i] && i + 1u < cap) { out[i] = in[i]; ++i; }
    /* never cut a UTF-8 sequence in half: if the next input byte continues a
       character, drop that character's bytes already copied */
    if (in && in[i] && ((unsigned char)in[i] & 0xC0u) == 0x80u) {
        while (i && ((unsigned char)out[i - 1u] & 0xC0u) == 0x80u) --i;
        if (i) --i;
    }
    out[i] = '\0';
}

void notify_queue_reset(void) {
    for (size_t i = 0; i < sizeof(queue); ++i) ((uint8_t *)queue)[i] = 0;
    count = 0;
    next_id = 0;
}

uint32_t notify_queue_push(uint32_t app, const char *title, const char *body, int64_t epoch, uint64_t now_ms, int banner) {
    if (count == NOTIFY_MAX) --count;                        /* drop the oldest */
    for (uint32_t i = count; i > 0; --i) queue[i] = queue[i - 1u];
    ++count;
    mde_notification *n = &queue[0];
    for (size_t i = 0; i < sizeof(*n); ++i) ((uint8_t *)n)[i] = 0;
    if (++next_id == 0u) next_id = 1u;
    n->id = next_id;
    n->app = app;
    n->epoch = epoch;
    n->posted_ms = now_ms;
    copy_trunc(n->title, sizeof(n->title), title && title[0] ? title : "Notification");
    copy_trunc(n->body, sizeof(n->body), body);
    n->banner = banner ? 1u : 0u;
    n->banner_until_ms = banner ? now_ms + NOTIFY_BANNER_MS : 0u;
    return n->id;
}

uint32_t notify_count(void) { return count; }
const mde_notification *notify_at(uint32_t index) { return index < count ? &queue[index] : 0; }
const mde_notification *notify_find(uint32_t id) {
    for (uint32_t i = 0; i < count; ++i) if (queue[i].id == id) return &queue[i];
    return 0;
}

void notify_dismiss(uint32_t id) {
    for (uint32_t i = 0; i < count; ++i)
        if (queue[i].id == id) {
            for (uint32_t j = i; j + 1u < count; ++j) queue[j] = queue[j + 1u];
            --count;
            return;
        }
}

void notify_hide_banner(uint32_t id) {
    for (uint32_t i = 0; i < count; ++i) if (queue[i].id == id) queue[i].banner = 0;
}

void notify_hold_banner(uint32_t id, uint64_t now_ms) {
    for (uint32_t i = 0; i < count; ++i)
        if (queue[i].id == id && queue[i].banner && queue[i].banner_until_ms < now_ms + 1500u)
            queue[i].banner_until_ms = now_ms + 1500u;
}

void notify_clear_all(void) { count = 0; }

uint32_t notify_banners(uint32_t *ids, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < max; ++i) if (queue[i].banner) ids[n++] = queue[i].id;
    return n;
}

uint32_t notify_expire_banners(uint64_t now_ms) {
    uint32_t hidden = 0, shown = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!queue[i].banner) continue;
        /* only NOTIFY_BANNERS_SHOWN banners stack; older ones leave at once */
        if (now_ms >= queue[i].banner_until_ms || shown >= NOTIFY_BANNERS_SHOWN) { queue[i].banner = 0; ++hidden; }
        else ++shown;
    }
    return hidden;
}

uint32_t notify_unread(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) n += queue[i].read ? 0u : 1u;
    return n;
}

void notify_mark_all_read(void) {
    for (uint32_t i = 0; i < count; ++i) queue[i].read = 1u;
}

static void append_num(char *out, uint32_t cap, uint32_t *len, uint64_t v) {
    char tmp[24]; uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < sizeof(tmp));
    while (n && *len + 1u < cap) out[(*len)++] = tmp[--n];
    out[*len] = '\0';
}
static void append_str(char *out, uint32_t cap, uint32_t *len, const char *s) {
    while (*s && *len + 1u < cap) out[(*len)++] = *s++;
    out[*len] = '\0';
}

void notify_age_text(char *out, uint32_t cap, uint64_t posted_ms, uint64_t now_ms) {
    if (!out || !cap) return;
    uint32_t len = 0;
    out[0] = '\0';
    uint64_t s = now_ms > posted_ms ? (now_ms - posted_ms) / 1000u : 0u;
    if (s < 60u) { append_str(out, cap, &len, "now"); return; }
    if (s < 3600u) { append_num(out, cap, &len, s / 60u); append_str(out, cap, &len, "m ago"); return; }
    if (s < 86400u) { append_num(out, cap, &len, s / 3600u); append_str(out, cap, &len, "h ago"); return; }
    if (s < 2u * 86400u) { append_str(out, cap, &len, "Yesterday"); return; }
    append_num(out, cap, &len, s / 86400u);
    append_str(out, cap, &len, "d ago");
}
