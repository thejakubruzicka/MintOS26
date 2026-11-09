/* Pure IPv4 helpers: checksums, address text, DHCP and DNS message
 * construction/parsing and TCP option parsing. No kernel state, no
 * allocation; every read is bounds-checked against the supplied length so
 * malformed packets from the network cannot over-read. Host-tested by
 * tools/test_net.c. */
#include <stdint.h>
#include <stddef.h>
#include "../include/net.h"

uint32_t mint_net_checksum_add(uint32_t sum, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    while (length > 1u) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; length -= 2u; }
    if (length) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t mint_net_checksum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t mint_net_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                     const uint8_t *segment, size_t length) {
    uint8_t pseudo[12];
    mint_net_put32(pseudo, src_ip); mint_net_put32(pseudo + 4, dst_ip);
    pseudo[8] = 0; pseudo[9] = protocol; mint_net_put16(pseudo + 10, (uint16_t)length);
    uint32_t sum = mint_net_checksum_add(0, pseudo, sizeof(pseudo));
    return mint_net_checksum_fold(mint_net_checksum_add(sum, segment, length));
}

int mint_ipv4_parse(const char *text, uint32_t *ip_out) {
    if (!text || !ip_out) return 0;
    uint32_t ip = 0, parts = 0;
    const char *p = text;
    while (parts < 4u) {
        uint32_t v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            if (++digits > 3u || v > 255u) return 0;
            ++p;
        }
        if (!digits) return 0;
        ip = (ip << 8) | v; ++parts;
        if (parts < 4u) { if (*p != '.') return 0; ++p; }
    }
    if (*p) return 0;
    *ip_out = ip;
    return 1;
}

size_t mint_ipv4_format(char *out, size_t capacity, uint32_t ip) {
    if (!out || !capacity) return 0;
    size_t n = 0;
    for (int part = 3; part >= 0; --part) {
        uint32_t v = (ip >> (part * 8)) & 0xFFu;
        char digits[3]; uint32_t d = 0;
        do { digits[d++] = (char)('0' + v % 10u); v /= 10u; } while (v && d < 3u);
        while (d && n + 1u < capacity) out[n++] = digits[--d];
        if (part && n + 1u < capacity) out[n++] = '.';
    }
    out[n] = '\0';
    return n;
}

/* ---------------------------------------------------------------- DHCP */
#define DHCP_FIXED 236u
#define DHCP_MAGIC 0x63825363u

size_t mint_dhcp_build(uint8_t *out, size_t capacity, uint8_t type, uint32_t xid, const uint8_t mac[6],
                       uint32_t client_ip, uint32_t requested_ip, uint32_t server_id, const char *hostname) {
    if (!out || !mac || capacity < 300u) return 0;
    for (size_t i = 0; i < 300u; ++i) out[i] = 0;
    out[0] = 1; out[1] = 1; out[2] = 6; out[3] = 0;       /* BOOTREQUEST, Ethernet */
    mint_net_put32(out + 4, xid);
    mint_net_put16(out + 10, client_ip ? 0u : 0x8000u);    /* ask for broadcast replies until configured */
    mint_net_put32(out + 12, client_ip);
    for (uint32_t i = 0; i < 6u; ++i) out[28 + i] = mac[i];
    mint_net_put32(out + DHCP_FIXED, DHCP_MAGIC);
    size_t n = DHCP_FIXED + 4u;
    out[n++] = 53; out[n++] = 1; out[n++] = type;
    out[n++] = 61; out[n++] = 7; out[n++] = 1;               /* client identifier: hardware type + MAC */
    for (uint32_t i = 0; i < 6u; ++i) out[n++] = mac[i];
    if (requested_ip) { out[n++] = 50; out[n++] = 4; mint_net_put32(out + n, requested_ip); n += 4; }
    if (server_id) { out[n++] = 54; out[n++] = 4; mint_net_put32(out + n, server_id); n += 4; }
    out[n++] = 57; out[n++] = 2; mint_net_put16(out + n, 1500u); n += 2;
    if (hostname && hostname[0]) {
        size_t len = 0;
        while (hostname[len] && len < 32u) ++len;
        out[n++] = 12; out[n++] = (uint8_t)len;
        for (size_t i = 0; i < len; ++i) out[n++] = (uint8_t)hostname[i];
    }
    out[n++] = 55; out[n++] = 5; out[n++] = 1; out[n++] = 3; out[n++] = 6; out[n++] = 51; out[n++] = 54;
    out[n++] = 255;
    return n < 300u ? 300u : n;
}

int mint_dhcp_parse(const uint8_t *p, size_t length, uint32_t xid, const uint8_t mac[6], MINT_DHCP_REPLY *out) {
    if (!p || !mac || !out || length < DHCP_FIXED + 4u) return 0;
    if (p[0] != 2 || p[1] != 1 || p[2] != 6 || mint_net_get32(p + 4) != xid) return 0;
    for (uint32_t i = 0; i < 6u; ++i) if (p[28 + i] != mac[i]) return 0;
    if (mint_net_get32(p + DHCP_FIXED) != DHCP_MAGIC) return 0;
    MINT_DHCP_REPLY r = {0, mint_net_get32(p + 16), 0, 0, 0, 0, 0};
    size_t i = DHCP_FIXED + 4u;
    while (i < length) {
        uint8_t code = p[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= length) return 0;
        uint8_t len = p[i++];
        if (len > length - i) return 0;
        const uint8_t *v = p + i;
        if (code == 53 && len >= 1) r.type = v[0];
        else if (code == 54 && len >= 4) r.server_id = mint_net_get32(v);
        else if (code == 1 && len >= 4) r.netmask = mint_net_get32(v);
        else if (code == 3 && len >= 4) r.router = mint_net_get32(v);
        else if (code == 6 && len >= 4) r.dns = mint_net_get32(v);
        else if (code == 51 && len >= 4) r.lease_seconds = mint_net_get32(v);
        i += len;
    }
    if (!r.type) return 0;
    *out = r;
    return 1;
}

/* ---------------------------------------------------------------- DNS */
size_t mint_dns_build_query(uint8_t *out, size_t capacity, uint16_t id, const char *name) {
    if (!out || !name || !name[0] || capacity < 18u) return 0;
    for (size_t i = 0; i < 12u; ++i) out[i] = 0;
    mint_net_put16(out, id);
    out[2] = 0x01;                      /* RD */
    mint_net_put16(out + 4, 1u);        /* QDCOUNT */
    size_t n = 12u, total = 0;
    const char *label = name;
    for (;;) {
        size_t len = 0;
        while (label[len] && label[len] != '.') ++len;
        if (!len) {
            if (!label[0] && total) break;           /* allow one trailing dot */
            return 0;                                 /* empty label */
        }
        if (len > 63u || n + 1u + len + 5u > capacity) return 0;
        out[n++] = (uint8_t)len;
        for (size_t i = 0; i < len; ++i) out[n++] = (uint8_t)label[i];
        total += len + 1u;
        if (total > 253u) return 0;
        if (!label[len]) break;
        label += len + 1u;
        if (!label[0]) break;
    }
    out[n++] = 0;
    mint_net_put16(out + n, 1u); n += 2;  /* QTYPE A */
    mint_net_put16(out + n, 1u); n += 2;  /* QCLASS IN */
    return n;
}

/* Skips a (possibly compressed) name; returns the offset after it or 0. */
static size_t dns_skip_name(const uint8_t *p, size_t length, size_t at) {
    for (uint32_t guard = 0; guard < 128u; ++guard) {
        if (at >= length) return 0;
        uint8_t len = p[at];
        if (len == 0) return at + 1u;
        if ((len & 0xC0u) == 0xC0u) return at + 2u <= length ? at + 2u : 0;
        if (len & 0xC0u) return 0;
        at += 1u + len;
    }
    return 0;
}

int mint_dns_parse_a(const uint8_t *p, size_t length, uint16_t id, uint32_t *ip_out, uint32_t *ttl_out) {
    if (!p || length < 12u || mint_net_get16(p) != id) return -2;
    if (!(p[2] & 0x80u)) return -2;                 /* not a response */
    uint8_t rcode = p[3] & 0x0Fu;
    if (rcode == 3) return -1;                      /* NXDOMAIN */
    if (rcode != 0) return -1;
    uint16_t qd = mint_net_get16(p + 4), an = mint_net_get16(p + 6);
    size_t at = 12u;
    for (uint16_t i = 0; i < qd; ++i) {
        at = dns_skip_name(p, length, at);
        if (!at || at + 4u > length) return -2;
        at += 4u;
    }
    for (uint16_t i = 0; i < an; ++i) {
        at = dns_skip_name(p, length, at);
        if (!at || at + 10u > length) return -2;
        uint16_t type = mint_net_get16(p + at), cls = mint_net_get16(p + at + 2);
        uint32_t ttl = mint_net_get32(p + at + 4);
        uint16_t rdlen = mint_net_get16(p + at + 8);
        at += 10u;
        if (rdlen > length - at) return -2;
        if (type == 1u && cls == 1u && rdlen == 4u) {
            if (ip_out) *ip_out = mint_net_get32(p + at);
            if (ttl_out) *ttl_out = ttl;
            return 1;
        }
        at += rdlen;
    }
    return 0;
}

uint16_t mint_tcp_parse_mss(const uint8_t *o, size_t length) {
    size_t i = 0;
    while (o && i < length) {
        uint8_t kind = o[i];
        if (kind == 0) break;
        if (kind == 1) { ++i; continue; }
        if (i + 1u >= length) break;
        uint8_t len = o[i + 1];
        if (len < 2u || len > length - i) break;
        if (kind == 2 && len == 4) return mint_net_get16(o + i + 2);
        i += len;
    }
    return 0;
}
