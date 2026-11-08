#include <stdint.h>
#include <stddef.h>
#include "../include/ramfs.h"
#include "../include/uapi/mintos/syscall.h"

#define MINT_RAMFS_HANDLE_MAX 16u
#define MINT_RAMFS_VALID_FLAGS (MINT_FS_OPEN_READ | MINT_FS_OPEN_WRITE | MINT_FS_OPEN_CREATE | MINT_FS_OPEN_TRUNC)

typedef struct mint_ramfs_node {
    char name[MINT_RAMFS_NAME_MAX];
    uint8_t bytes[MINT_RAMFS_FILE_CAPACITY];
    uint32_t size;
    uint32_t readonly;
    uint32_t used;
} MINT_RAMFS_NODE;

typedef struct mint_ramfs_handle {
    uint64_t owner;
    uint32_t node;
    uint32_t position;
    uint32_t flags;
    uint32_t active;
    uint32_t token;
} MINT_RAMFS_HANDLE;

static MINT_RAMFS_NODE nodes[MINT_RAMFS_MAX_FILES];
static MINT_RAMFS_HANDLE handles[MINT_RAMFS_HANDLE_MAX];
static uint32_t next_token = 3u;
static int ready;

static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    if (!s) return 0;
    while (n < max && s[n]) ++n;
    return n;
}
static void zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}
static char upper_ascii(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}
static int normalize_name(const char *input, char output[MINT_RAMFS_NAME_MAX]) {
    size_t n = bounded_len(input, MINT_RAMFS_NAME_MAX);
    if (!n || n >= MINT_RAMFS_NAME_MAX) return 0;
    for (size_t i = 0; i < MINT_RAMFS_NAME_MAX; ++i) output[i] = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = upper_ascii(input[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return 0;
        output[i] = c;
    }
    return 1;
}
static int find_node(const char *name) {
    for (uint32_t i = 0; i < MINT_RAMFS_MAX_FILES; ++i)
        if (nodes[i].used && bounded_len(nodes[i].name, MINT_RAMFS_NAME_MAX) ==
            bounded_len(name, MINT_RAMFS_NAME_MAX)) {
            size_t n = bounded_len(name, MINT_RAMFS_NAME_MAX);
            size_t j = 0;
            while (j < n && nodes[i].name[j] == name[j]) ++j;
            if (j == n) return (int)i;
        }
    return -1;
}
static int find_free_node(void) {
    for (uint32_t i = 0; i < MINT_RAMFS_MAX_FILES; ++i)
        if (!nodes[i].used) return (int)i;
    return -1;
}
static MINT_RAMFS_HANDLE *find_handle(uint64_t owner, uint64_t handle) {
    if (handle > UINT32_MAX) return 0;
    for (uint32_t i = 0; i < MINT_RAMFS_HANDLE_MAX; ++i)
        if (handles[i].active && handles[i].token == (uint32_t)handle && handles[i].owner == owner)
            return &handles[i];
    return 0;
}
static void make_node(uint32_t index, const char *name, const char *contents, uint32_t readonly) {
    MINT_RAMFS_NODE *n = &nodes[index];
    zero(n, sizeof(*n));
    size_t name_len = bounded_len(name, sizeof(n->name) - 1u);
    for (size_t i = 0; i < name_len; ++i) n->name[i] = name[i];
    size_t content_len = bounded_len(contents, sizeof(n->bytes));
    for (size_t i = 0; i < content_len; ++i) n->bytes[i] = (uint8_t)contents[i];
    n->size = (uint32_t)content_len;
    n->readonly = readonly;
    n->used = 1u;
}

void mint_ramfs_init(void) {
    zero(nodes, sizeof(nodes));
    zero(handles, sizeof(handles));
    next_token = 3u;
    make_node(0, "WELCOME.TXT", "WELCOME TO MINTOS26\nTHIS IS THE NATIVE DESKTOP SESSION.\nFILES ARE STORED IN THE SESSION RAM FILESYSTEM FOR NOW.\n", 1u);
    make_node(1, "NOTES.TXT", "", 0u);
    make_node(2, "ABOUT.TXT", "MINTOS26\nEXPERIMENTAL KERNEL AND USERSPACE.\nSETTINGS AND RAM FILES RESET AFTER REBOOT IN THIS RELEASE.\n", 1u);
    ready = 1;
}

int64_t mint_ramfs_open(uint64_t owner, const char *name, uint32_t flags) {
    if (!ready) return -MINT_ENODEV;
    if (!owner || !name || (flags & ~MINT_RAMFS_VALID_FLAGS) ||
        !(flags & (MINT_FS_OPEN_READ | MINT_FS_OPEN_WRITE)) ||
        ((flags & MINT_FS_OPEN_TRUNC) && !(flags & MINT_FS_OPEN_WRITE))) return -MINT_EINVAL;
    char normalized[MINT_RAMFS_NAME_MAX];
    if (!normalize_name(name, normalized)) return -MINT_EINVAL;
    int free_handle = -1;
    for (uint32_t i = 0; i < MINT_RAMFS_HANDLE_MAX; ++i)
        if (!handles[i].active) { free_handle = (int)i; break; }
    if (free_handle < 0) return -MINT_ENOSPC;
    int node_index = find_node(normalized);
    if (node_index < 0 && (flags & MINT_FS_OPEN_CREATE)) {
        node_index = find_free_node();
        if (node_index < 0) return -MINT_ENOSPC;
        make_node((uint32_t)node_index, normalized, "", 0u);
    }
    if (node_index < 0) return -MINT_ENOENT;
    MINT_RAMFS_NODE *node = &nodes[(uint32_t)node_index];
    if ((flags & MINT_FS_OPEN_WRITE) && node->readonly) return -MINT_EROFS;
    if (flags & MINT_FS_OPEN_TRUNC) { node->size = 0; zero(node->bytes, sizeof(node->bytes)); }
    uint32_t token = next_token++;
    if (token < 3u || token > 0x7FFFFFFFu) { next_token = 4u; token = 3u; }
    /* Avoid reusing a live token. */
    int collision;
    do {
        collision = 0;
        for (uint32_t j = 0; j < MINT_RAMFS_HANDLE_MAX; ++j)
            if (handles[j].active && handles[j].token == token) collision = 1;
        if (collision) { token = next_token++; if (token < 3u || token > 0x7FFFFFFFu) token = next_token = 3u; }
    } while (collision);
    MINT_RAMFS_HANDLE *handle = &handles[(uint32_t)free_handle];
    handle->owner = owner; handle->node = (uint32_t)node_index;
    handle->position = 0; handle->flags = flags;
    handle->token = token; handle->active = 1u;
    return (int64_t)token;
}

int64_t mint_ramfs_read(uint64_t owner, uint64_t handle, void *buffer, size_t length) {
    MINT_RAMFS_HANDLE *h = find_handle(owner, handle);
    if (!h) return -MINT_EBADF;
    if (!(h->flags & MINT_FS_OPEN_READ)) return -MINT_EPERM;
    if ((!buffer && length) || length > MINT_RAMFS_FILE_CAPACITY) return -MINT_EINVAL;
    MINT_RAMFS_NODE *n = &nodes[h->node];
    uint32_t available = h->position < n->size ? n->size - h->position : 0u;
    size_t count = length < available ? length : available;
    uint8_t *out = (uint8_t *)buffer;
    for (size_t i = 0; i < count; ++i) out[i] = n->bytes[h->position + (uint32_t)i];
    h->position += (uint32_t)count;
    return (int64_t)count;
}

int64_t mint_ramfs_write(uint64_t owner, uint64_t handle, const void *buffer, size_t length) {
    MINT_RAMFS_HANDLE *h = find_handle(owner, handle);
    if (!h) return -MINT_EBADF;
    if (!(h->flags & MINT_FS_OPEN_WRITE)) return -MINT_EPERM;
    if ((!buffer && length) || length > MINT_RAMFS_FILE_CAPACITY) return -MINT_EINVAL;
    MINT_RAMFS_NODE *n = &nodes[h->node];
    if (h->position > MINT_RAMFS_FILE_CAPACITY || length > MINT_RAMFS_FILE_CAPACITY - h->position)
        return -MINT_ENOSPC;
    const uint8_t *in = (const uint8_t *)buffer;
    for (size_t i = 0; i < length; ++i) n->bytes[h->position + (uint32_t)i] = in[i];
    h->position += (uint32_t)length;
    if (h->position > n->size) n->size = h->position;
    return (int64_t)length;
}

int64_t mint_ramfs_close(uint64_t owner, uint64_t handle) {
    MINT_RAMFS_HANDLE *h = find_handle(owner, handle);
    if (!h) return -MINT_EBADF;
    zero(h, sizeof(*h));
    return 0;
}

void mint_ramfs_close_owner(uint64_t owner) {
    if (!owner) return;
    for (uint32_t i = 0; i < MINT_RAMFS_HANDLE_MAX; ++i) {
        if (handles[i].active && handles[i].owner == owner)
            zero(&handles[i], sizeof(handles[i]));
    }
}

int64_t mint_ramfs_list(MINT_FS_DIRENT *out, uint32_t capacity) {
    if (!ready) return -MINT_ENODEV;
    if (capacity > MINT_RAMFS_MAX_FILES || (!out && capacity)) return -MINT_EINVAL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < MINT_RAMFS_MAX_FILES && count < capacity; ++i) {
        if (!nodes[i].used) continue;
        MINT_FS_DIRENT *d = &out[count++];
        zero(d, sizeof(*d));
        size_t n = bounded_len(nodes[i].name, sizeof(d->name) - 1u);
        for (size_t j = 0; j < n; ++j) d->name[j] = nodes[i].name[j];
        d->size = nodes[i].size;
        d->flags = nodes[i].readonly ? MINT_FS_DIRENT_READONLY : 0u;
    }
    return (int64_t)count;
}
