#ifndef MINTDE_NOTIFY_H
#define MINTDE_NOTIFY_H
/* Notification queue (26.5.014.0). Pure bookkeeping with no drawing so
 * tools/test_notify.c can test it on the host: a bounded newest-first list
 * for Notification Center plus the subset currently shown as banners. */
#include <stdint.h>

#define NOTIFY_MAX 24u
#define NOTIFY_BANNER_MS 5000u
#define NOTIFY_BANNERS_SHOWN 3u
#define NOTIFY_TITLE_MAX 48u
#define NOTIFY_BODY_MAX 112u

typedef struct mde_notification {
    uint32_t id, app;
    int64_t epoch;                /* wall clock when posted (0 if unknown) */
    uint64_t posted_ms;           /* monotonic */
    uint64_t banner_until_ms;
    char title[NOTIFY_TITLE_MAX];
    char body[NOTIFY_BODY_MAX];
    uint8_t read, banner;
    uint8_t reserved[6];
} mde_notification;

/* Adds a notification (oldest dropped when full); banner != 0 shows it as a
 * banner until now_ms + NOTIFY_BANNER_MS. Returns its id (never 0). */
uint32_t notify_queue_push(uint32_t app, const char *title, const char *body, int64_t epoch, uint64_t now_ms, int banner);
void notify_queue_reset(void);
uint32_t notify_count(void);
const mde_notification *notify_at(uint32_t index);       /* 0 = newest */
const mde_notification *notify_find(uint32_t id);
void notify_dismiss(uint32_t id);
void notify_hide_banner(uint32_t id);
void notify_hold_banner(uint32_t id, uint64_t now_ms);   /* pointer over it: keep it a while longer */
void notify_clear_all(void);
/* Banners on screen, newest first, at most max (NOTIFY_BANNERS_SHOWN). */
uint32_t notify_banners(uint32_t *ids, uint32_t max);
uint32_t notify_expire_banners(uint64_t now_ms);          /* returns how many were hidden */
uint32_t notify_unread(void);
void notify_mark_all_read(void);
/* "now", "5m ago", "2h ago", "Yesterday", "3d ago" */
void notify_age_text(char *out, uint32_t cap, uint64_t posted_ms, uint64_t now_ms);
#endif
