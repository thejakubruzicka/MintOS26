#ifndef MINT_WEB_INFLATE_H
#define MINT_WEB_INFLATE_H
/* DEFLATE decoder (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952)
 * framing for HTTP Content-Encoding. Integer-only, bounded: the output grows
 * on the wmem heap up to a caller-given maximum and every input read is
 * bounds-checked, so corrupt or hostile streams fail with an error code. */
#include <stddef.h>
#include <stdint.h>

enum {
    WINF_OK = 0, WINF_ERR_DATA = -1, WINF_ERR_TRUNCATED = -2, WINF_ERR_TOO_BIG = -3,
    WINF_ERR_MEMORY = -4, WINF_ERR_CHECKSUM = -5, WINF_ERR_HEADER = -6
};
enum { WINF_RAW = 0, WINF_ZLIB = 1, WINF_GZIP = 2, WINF_AUTO_DEFLATE = 3 };

uint32_t winf_crc32(uint32_t crc, const uint8_t *p, size_t n);
uint32_t winf_adler32(uint32_t a, const uint8_t *p, size_t n);

/* Decodes `in` (format WINF_*) into a new wmem buffer stored in *out (caller
 * frees with wmem_free). WINF_AUTO_DEFLATE accepts zlib-wrapped or raw deflate
 * (servers disagree on what "Content-Encoding: deflate" means).
 * Returns the decoded length or a negative WINF_ERR_*. */
int64_t winf_decode(const uint8_t *in, size_t in_len, int format, size_t max_out, uint8_t **out);
const char *winf_error_text(int64_t code);
#endif
