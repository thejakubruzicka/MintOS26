/* Host test for kernel/user/mintde/notify.c (notification queue). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/user/mintde/notify.h"

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void) {
    notify_queue_reset();
    CHECK(notify_count() == 0 && !notify_at(0), "empty queue");
    uint32_t a = notify_queue_push(1, "Disk Utility", "MintOS HD is ready.", 1000, 100, 1);
    uint32_t b = notify_queue_push(2, "Trash", "4 items deleted.", 1001, 200, 1);
    CHECK(a && b && a != b && notify_count() == 2, "push returns distinct ids");
    CHECK(notify_at(0)->id == b && notify_at(1)->id == a, "newest first");
    CHECK(!strcmp(notify_find(a)->title, "Disk Utility") && notify_find(a)->app == 1u && notify_find(a)->epoch == 1000, "fields kept");
    uint32_t ids[8];
    CHECK(notify_banners(ids, 8) == 2 && ids[0] == b, "both shown as banners, newest on top");
    CHECK(notify_expire_banners(100 + NOTIFY_BANNER_MS - 1) == 0, "banner stays until its time");
    CHECK(notify_expire_banners(100 + NOTIFY_BANNER_MS) == 1 && notify_banners(ids, 8) == 1 && ids[0] == b, "first banner auto-dismisses");
    notify_hold_banner(b, 200 + NOTIFY_BANNER_MS - 10);
    CHECK(notify_expire_banners(200 + NOTIFY_BANNER_MS + 100) == 0, "hovered banner is held");
    CHECK(notify_expire_banners(200 + NOTIFY_BANNER_MS + 2000) == 1 && notify_banners(ids, 8) == 0, "held banner leaves later");
    CHECK(notify_count() == 2, "expired banners stay in Notification Center");
    uint32_t c = notify_queue_push(3, "", 0, 0, 300, 0);
    CHECK(!strcmp(notify_find(c)->title, "Notification") && notify_find(c)->body[0] == '\0' && !notify_find(c)->banner,
          "defaults for empty title/body; no banner when banners are off");
    CHECK(notify_unread() == 3, "unread count");
    notify_mark_all_read();
    CHECK(notify_unread() == 0, "mark all read");
    notify_dismiss(a);
    CHECK(notify_count() == 2 && !notify_find(a) && notify_at(1)->id == b, "dismiss one keeps order");
    notify_dismiss(9999);
    CHECK(notify_count() == 2, "dismissing an unknown id is harmless");
    /* overflow drops the oldest */
    notify_queue_reset();
    uint32_t first = notify_queue_push(0, "first", "", 0, 0, 0);
    for (int i = 0; i < 30; ++i) notify_queue_push(0, "n", "", 0, (uint64_t)i, 0);
    CHECK(notify_count() == NOTIFY_MAX && !notify_find(first), "bounded at NOTIFY_MAX; oldest dropped");
    /* banner stacking limit */
    notify_queue_reset();
    for (int i = 0; i < 5; ++i) notify_queue_push(0, "b", "", 0, 1000, 1);
    CHECK(notify_expire_banners(1001) == 2 && notify_banners(ids, 8) == NOTIFY_BANNERS_SHOWN, "at most three banners stack");
    notify_hide_banner(ids[0]);
    CHECK(notify_banners(ids, 8) == 2, "closing a banner");
    notify_clear_all();
    CHECK(notify_count() == 0, "clear all");
    /* truncation keeps UTF-8 intact */
    char longt[200];
    memset(longt, 'a', sizeof(longt));
    memcpy(longt + NOTIFY_TITLE_MAX - 2, "\xE2\x80\x9C", 3);          /* a 3-byte quote straddling the limit */
    longt[sizeof(longt) - 1] = '\0';
    uint32_t t = notify_queue_push(0, longt, longt, 0, 0, 0);
    const mde_notification *n = notify_find(t);
    size_t tl = strlen(n->title);
    CHECK(tl < NOTIFY_TITLE_MAX && tl == NOTIFY_TITLE_MAX - 2 && strlen(n->body) == NOTIFY_BODY_MAX - 1, "long text truncated on character boundaries");
    char age[24];
    notify_age_text(age, sizeof(age), 0, 30000); CHECK(!strcmp(age, "now"), "age now");
    notify_age_text(age, sizeof(age), 0, 5 * 60000u); CHECK(!strcmp(age, "5m ago"), "age minutes");
    notify_age_text(age, sizeof(age), 0, 3 * 3600000u); CHECK(!strcmp(age, "3h ago"), "age hours");
    notify_age_text(age, sizeof(age), 0, 30 * 3600000u); CHECK(!strcmp(age, "Yesterday"), "age yesterday");
    notify_age_text(age, sizeof(age), 0, 4 * 86400000ull); CHECK(!strcmp(age, "4d ago"), "age days");
    notify_age_text(age, sizeof(age), 5000, 1000); CHECK(!strcmp(age, "now"), "clock going backwards");
    printf("%s: %d/%d notification checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
