/* Host harness: runs the MintOS TLS 1.3 client over a non-blocking BSD socket.
 *   test-tls HOST [PORT] [--suite HEX] [--root SUBJECTHEX:SPKIHEX] [--connect ADDR] [--path /x]
 * Prints "OK <suite> <HTTP status line> <bytes>" or "ERR <code> <reason>". */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../kernel/user/crypto/tls.h"

void mint_random_bytes(void *out, size_t len) { arc4random_buf(out, len); }

static int64_t io_send(void *ctx, const void *d, size_t n) {
    ssize_t r = send(*(int *)ctx, d, n, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return r;
}
static int64_t io_recv(void *ctx, void *o, size_t n) {
    ssize_t r = recv(*(int *)ctx, o, n, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (r == 0) return -1;
    return r;
}
static size_t unhex(const char *s, uint8_t *out) { size_t n = strlen(s) / 2; for (size_t i = 0; i < n; ++i) { unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (uint8_t)v; } return n; }

static MINT_TLS tls;
static uint8_t buf[1 << 16];

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *host = argv[1], *port = "443", *connect_to = argv[1], *path = "/";
    static uint8_t subj[4096], spki[4096];
    MINT_TLS_ROOT custom; int use_custom = 0; unsigned suite = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--suite") && i + 1 < argc) suite = (unsigned)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc) connect_to = argv[++i];
        else if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            char *r = argv[++i], *c = strchr(r, ':'); *c = 0;
            custom.subject_len = (uint32_t)unhex(r, subj); custom.subject = subj;
            custom.spki_len = (uint32_t)unhex(c + 1, spki); custom.spki = spki; custom.label = "test";
            use_custom = 1;
        } else port = argv[i];
    }
    struct addrinfo hints = {0}, *ai;
    hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_INET;
    if (getaddrinfo(connect_to, port, &hints, &ai)) { printf("ERR dns\n"); return 0; }
    int fd = socket(ai->ai_family, ai->ai_socktype, 0);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) { printf("ERR connect\n"); return 0; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    MINT_TLS_IO io = {&fd, io_send, io_recv};
    mint_tls_init(&tls, &io, host, (int64_t)time(NULL));
    if (use_custom) { tls.roots = &custom; tls.root_count = 1; }
    if (suite) { tls.suites[0] = (uint16_t)suite; tls.suite_count = 1; }
    time_t start = time(NULL);
    int rc;
    while ((rc = mint_tls_step(&tls)) == MINT_TLS_AGAIN) {
        if (time(NULL) - start > 20) { printf("ERR timeout\n"); return 0; }
        struct pollfd p = {fd, POLLIN | (tls.wlen ? POLLOUT : 0), 0}; poll(&p, 1, 200);
    }
    if (rc != MINT_TLS_DONE) { printf("ERR %d %s (cert %d, alert %d)\n", rc, mint_tls_error_text(&tls), tls.cert_error, tls.alert); return 0; }
    char req[512];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MintOS26-test\r\nConnection: close\r\n\r\n", path, host);
    if (mint_tls_write(&tls, req, strlen(req)) != (int64_t)strlen(req)) { printf("ERR write\n"); return 0; }
    size_t total = 0; char status[128] = "";
    for (;;) {
        int64_t n = mint_tls_read(&tls, buf, sizeof(buf));
        if (n > 0) {
            if (!total) { size_t k = 0; while (k < (size_t)n && k < sizeof(status) - 1 && buf[k] != '\r') { status[k] = (char)buf[k]; ++k; } status[k] = 0; }
            total += (size_t)n; continue;
        }
        if (n == 0) break;
        if (n != MINT_TLS_WOULD_BLOCK) { printf("ERR read %lld %s alert=%d after %zu bytes\n", (long long)n, mint_tls_error_text(&tls), tls.alert, total); return 0; }
        if (time(NULL) - start > 30) break;
        struct pollfd p = {fd, POLLIN, 0}; poll(&p, 1, 200);
    }
    printf("OK %s | %s | %zu bytes\n", mint_tls_suite_name(tls.suite), status, total);
    mint_tls_close(&tls);
    close(fd);
    return 0;
}
