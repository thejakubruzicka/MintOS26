#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/desktop_services.h"
#include "../kernel/include/ramfs.h"
#include "../kernel/include/uapi/mintos/syscall.h"

#define CHECK(expr, label) do { if (!(expr)) { fprintf(stderr, "FAIL: %s\n", label); return 1; } puts("PASS: " label); } while (0)

int main(void) {
    mint_desktop_settings_init();
    MINT_DESKTOP_SETTINGS settings = {0};
    CHECK(mint_desktop_settings_get(&settings) && settings.version == MINT_DESKTOP_SETTINGS_VERSION &&
          settings.theme == MINT_THEME_LIGHT && settings.accent == MINT_ACCENT_MINT && settings.ui_scale == 1u,
          "settings service exposes versioned defaults");
    uint32_t generation = settings.generation;
    settings.theme = MINT_THEME_DARK; settings.accent = MINT_ACCENT_ROSE;
    settings.ui_scale = 3u; settings.wallpaper = 2u;
    settings.flags = MINT_DESKTOP_SETTING_REDUCE_MOTION;
    CHECK(mint_desktop_settings_set(&settings), "settings service accepts valid theme preferences");
    CHECK(mint_desktop_settings_get(&settings) && settings.theme == MINT_THEME_DARK &&
          settings.accent == MINT_ACCENT_ROSE && settings.ui_scale == 3u &&
          settings.wallpaper == 2u && settings.flags == MINT_DESKTOP_SETTING_REDUCE_MOTION &&
          settings.generation > generation, "settings update increments generation and retains values");
    settings.theme = 99u;
    CHECK(!mint_desktop_settings_set(&settings), "settings service rejects unsupported theme enum");
    CHECK(mint_desktop_settings_get(&settings) && settings.theme == MINT_THEME_DARK,
          "rejected settings leave the active settings unchanged");

    mint_ramfs_init();
    MINT_FS_DIRENT entries[MINT_RAMFS_MAX_FILES];
    int64_t count = mint_ramfs_list(entries, MINT_RAMFS_MAX_FILES);
    CHECK(count == 3, "RAM filesystem starts with welcome, notes and about files");
    int64_t welcome = mint_ramfs_open(100u, "welcome.txt", MINT_FS_OPEN_READ);
    CHECK(welcome >= 3, "RAM filesystem normalizes case when opening names");
    char content[160] = {0};
    int64_t read_count = mint_ramfs_read(100u, (uint64_t)welcome, content, sizeof(content)-1u);
    CHECK(read_count > 0 && strstr(content, "WELCOME TO MINTOS26") != 0,
          "RAM filesystem reads a built-in file");
    CHECK(mint_ramfs_read(101u, (uint64_t)welcome, content, 1u) == -MINT_EBADF,
          "file handles are isolated by owner process");
    CHECK(mint_ramfs_close(100u, (uint64_t)welcome) == 0 &&
          mint_ramfs_read(100u, (uint64_t)welcome, content, 1u) == -MINT_EBADF,
          "closed handles cannot be reused");
    CHECK(mint_ramfs_open(100u, "WELCOME.TXT", MINT_FS_OPEN_WRITE) == -MINT_EROFS,
          "built-in files reject write access");

    int64_t notes = mint_ramfs_open(100u, "NOTES.TXT", MINT_FS_OPEN_READ | MINT_FS_OPEN_WRITE | MINT_FS_OPEN_TRUNC);
    const char note[] = "SESSION NOTE\nSECOND LINE";
    CHECK(notes >= 3 && mint_ramfs_write(100u, (uint64_t)notes, note, sizeof(note)-1u) == (int64_t)sizeof(note)-1,
          "RAM filesystem writes bounded note contents");
    CHECK(mint_ramfs_close(100u, (uint64_t)notes) == 0, "RAM filesystem closes writable handle");
    notes = mint_ramfs_open(100u, "NOTES.TXT", MINT_FS_OPEN_READ);
    memset(content, 0, sizeof(content));
    CHECK(notes >= 3 && mint_ramfs_read(100u, (uint64_t)notes, content, sizeof(content)-1u) == (int64_t)sizeof(note)-1 &&
          strcmp(content, note) == 0, "notes persist across app handles during the session");
    CHECK(mint_ramfs_close(100u, (uint64_t)notes) == 0, "RAM filesystem releases note handle");
    CHECK(mint_ramfs_open(100u, "../ESCAPE", MINT_FS_OPEN_READ | MINT_FS_OPEN_CREATE) == -MINT_EINVAL,
          "RAM filesystem rejects path traversal names");
    int64_t owner100 = mint_ramfs_open(100u, "WELCOME.TXT", MINT_FS_OPEN_READ);
    int64_t owner101 = mint_ramfs_open(101u, "WELCOME.TXT", MINT_FS_OPEN_READ);
    CHECK(owner100 >= 3 && owner101 >= 3, "two processes can hold independent file handles");
    mint_ramfs_close_owner(100u);
    CHECK(mint_ramfs_read(100u, (uint64_t)owner100, content, 1u) == -MINT_EBADF,
          "process exit reaps every file handle owned by that process");
    CHECK(mint_ramfs_read(101u, (uint64_t)owner101, content, 1u) == 1,
          "reaping one process handles leaves another process unaffected");
    mint_ramfs_close_owner(101u);
    CHECK(mint_ramfs_read(101u, (uint64_t)owner101, content, 1u) == -MINT_EBADF,
          "owner cleanup releases remaining handles");
    puts("PASS: session RAMFS and desktop settings tests complete");
    return 0;
}
