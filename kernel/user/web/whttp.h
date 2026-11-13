#ifndef MINT_WEB_WHTTP_H
#define MINT_WEB_WHTTP_H
/* HTTP/1.1 client pieces for the MintOS Web engine, independent of sockets:
 * URL parsing and resolution, request building, an incremental response
 * parser (status, headers, Content-Length / chunked / until-close bodies),
 * Content-Encoding decoding, an RFC 6265 cookie jar and
 * application/x-www-form-urlencoded encoding. Portable C (host-tested). */
#include <stddef.h>
#include <stdint.h>

#define WURL_MAX 1024u
#define WHOST_MAX 128u

typedef struct wurl {
    uint8_t secure;                /* https */
    uint16_t port;
    char host[WHOST_MAX];          /* lower case */
    char path[WURL_MAX];           /* absolute path + query, no fragment, ASCII only */
} wurl;

/* 1 if `in` is an absolute http(s) URL (parsed into u), 0 otherwise. */
int wurl_parse(const char *in, wurl *u);
void wurl_format(const wurl *u, char *out, size_t cap);
/* Resolves href against base into an absolute URL string. Non-http schemes
 * (mailto:, javascript:, data:, about:) are copied unchanged. Returns 1 if
 * the result is an http(s) URL. Spaces and non-ASCII bytes are %-encoded. */
int wurl_resolve(const wurl *base, const char *href, char *out, size_t cap);
int wurl_same_origin(const wurl *a, const wurl *b);
/* Last path segment, decoded, made safe as a file name ("index.html" if none). */
void wurl_file_name(const wurl *u, char *out, size_t cap);

/* ---------------------------------------------------------------- requests */
enum { WHTTP_GET = 0, WHTTP_POST = 1 };
typedef struct whttp_request_info {
    int method;
    const wurl *url;
    const char *accept;            /* NULL = default */
    const char *cookie;            /* "a=b; c=d" or NULL */
    const char *referer;           /* absolute URL or NULL */
    const char *content_type;      /* POST */
    size_t body_len;               /* POST; the body is sent separately */
    int keep_alive;
} whttp_request_info;
#define WHTTP_USER_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) MintOS/26.5 Web/2.0"
/* Returns the request length, or 0 if it does not fit. */
size_t whttp_build_request(char *out, size_t cap, const whttp_request_info *info);

/* ---------------------------------------------------------------- responses */
enum { WH_STATUS = 0, WH_HEADERS, WH_BODY_LENGTH, WH_BODY_CHUNKED, WH_BODY_CLOSE, WH_DONE, WH_ERROR };
enum { WENC_IDENTITY = 0, WENC_GZIP, WENC_DEFLATE, WENC_UNSUPPORTED };
typedef int (*whttp_sink)(void *ctx, const uint8_t *data, size_t len);   /* 0 = abort */

typedef struct whttp_resp {
    int state, error;
    int status, http_minor;
    int head_request;              /* HEAD: no body */
    char *head; size_t head_len, head_cap;    /* raw header block (wmem) */
    int64_t content_length;        /* -1 unknown */
    int chunked, conn_close, encoding;
    /* chunked decoding */
    int cstate; uint64_t chunk_left; char line[40]; size_t line_len;
    /* body (after Transfer-Encoding, before Content-Encoding) */
    uint8_t *body; size_t body_len, body_cap, body_max;
    int truncated;                 /* more than body_max bytes arrived (rest discarded) */
    uint64_t body_total;
    whttp_sink sink; void *sink_ctx;          /* if set, body bytes go here instead */
    int pause_after_headers;       /* whttp_feed returns right after the header block (once) */
    int headers_ready;             /* set when the header block has been parsed */
} whttp_resp;

void whttp_resp_init(whttp_resp *r, size_t body_max);
void whttp_resp_free(whttp_resp *r);
/* Feeds received bytes; returns how many were consumed (the rest belong to
 * nothing: we never pipeline). Check r->state for WH_DONE / WH_ERROR. */
size_t whttp_feed(whttp_resp *r, const uint8_t *data, size_t len);
/* The connection closed: completes an until-close body, else marks an error
 * (a partial body is kept and r->truncated set). */
void whttp_eof(whttp_resp *r);
/* Value of the idx-th header called name (case-insensitive), or NULL. */
const char *whttp_header(const whttp_resp *r, const char *name, uint32_t idx, char *out, size_t cap);
/* Decodes Content-Encoding in place of r->body (gzip/deflate). 0 ok, <0 WINF_ERR_*. */
int64_t whttp_decode_body(whttp_resp *r, size_t max_out);
/* Media type in lower case without parameters ("text/html"), and charset. */
void whttp_content_type(const whttp_resp *r, char *type, size_t tcap, char *charset, size_t ccap);
/* filename= from Content-Disposition (sanitised), 1 if "attachment". */
int whttp_disposition(const whttp_resp *r, char *name, size_t cap);
const char *whttp_status_text(int status);

/* ---------------------------------------------------------------- cookies */
#define WCOOKIE_MAX 300u
void wcookie_clear(void);
uint32_t wcookie_count(void);
/* Stores a Set-Cookie header value received for request url u. 1 = stored. */
int wcookie_set(const char *set_cookie, const wurl *u, int64_t now);
/* Builds the Cookie header value for a request to u; returns its length. */
size_t wcookie_header(const wurl *u, int64_t now, char *out, size_t cap);
/* Persistent (non-session, unexpired) cookies as text lines; returns length. */
size_t wcookie_save(char *out, size_t cap, int64_t now);
void wcookie_load(const char *text, size_t len, int64_t now);
uint32_t wcookie_generation(void);    /* changes whenever the jar changes */
/* "Wed, 21 Oct 2015 07:28:00 GMT" and the RFC 6265 variants; -1 if invalid. */
int64_t whttp_parse_date(const char *s);
int64_t whttp_days_from_civil(int64_t y, uint32_t m, uint32_t d);

/* ---------------------------------------------------------------- forms */
/* Appends "name=value" (with '&' when len > 0) in
 * application/x-www-form-urlencoded; returns the new length (unchanged if
 * it would not fit). */
size_t wform_append(char *out, size_t cap, size_t len, const char *name, const char *value);

/* Small string helpers shared by the engine. */
int wstr_ieq(const char *a, const char *b);
int wstr_iprefix(const char *s, const char *prefix);
size_t wstr_copy(char *out, size_t cap, const char *in);
size_t wstr_append(char *out, size_t cap, const char *in);
void wstr_u64(char *out, size_t cap, uint64_t v);
#endif
