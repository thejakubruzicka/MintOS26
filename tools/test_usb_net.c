/* Host tests for the pure USB HID helpers (kernel/usb/xhci.c) and the IPv4
 * helpers (kernel/net/netcore.c): HID report-descriptor parsing and field
 * extraction, HID usage -> set-1 mapping, xHCI interval encoding, Internet
 * checksums, IPv4 text, DHCP and DNS message building/parsing (including
 * hostile input) and TCP option parsing. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/usb.h"
#include "../kernel/include/net.h"

static int failures;
#define CHECK(cond, name) do { if (cond) printf("PASS: %s\n", name); else { printf("FAIL: %s\n", name); ++failures; } } while (0)

/* QEMU hw/usb/dev-hid.c tablet descriptor: 3 buttons, 16-bit absolute X/Y, relative wheel. */
static const uint8_t qemu_tablet[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
    0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,
    0x26,0xff,0x7f,0x35,0x00,0x46,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,0x05,0x01,0x09,0x38,0x15,0x81,
    0x25,0x7f,0x35,0x00,0x45,0x00,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0};
/* Standard boot-compatible mouse: 3 buttons, 8-bit relative X/Y. */
static const uint8_t boot_mouse[] = {
    0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
    0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,
    0x25,0x7F,0x75,0x08,0x95,0x02,0x81,0x06,0xC0,0xC0};
/* Mouse with Report ID 2 and 12-bit X/Y packed in three bytes (common gaming-mouse layout). */
static const uint8_t id_mouse[] = {
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x02,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,
    0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x03,0x81,0x01,0x05,0x01,0x16,0x01,0xF8,0x26,
    0xFF,0x07,0x75,0x0C,0x95,0x02,0x09,0x30,0x09,0x31,0x81,0x06,0xC0,0xC0};

static void test_hid(void) {
    MINT_HID_POINTER_LAYOUT t;
    int ok = mint_hid_parse_pointer(qemu_tablet, sizeof(qemu_tablet), &t);
    CHECK(ok && t.report_id == 0 && t.button_count == 3 && t.button_bit == 0, "tablet: three buttons at bit 0, no report ID");
    CHECK(t.x.present && t.x.bit == 8 && t.x.size == 16 && t.x.min == 0 && t.x.max == 32767 && !t.x.relative &&
          t.y.bit == 24 && t.y.size == 16 && !t.y.relative, "tablet: absolute 16-bit X/Y at bits 8 and 24, range 0..32767");
    CHECK(t.wheel.present && t.wheel.bit == 40 && t.wheel.size == 8 && t.wheel.min == -127 && t.wheel.relative,
          "tablet: relative signed wheel at bit 40");
    const uint8_t report[] = {0x01, 0x34, 0x12, 0x78, 0x56, 0xFF};
    CHECK(mint_hid_field_value(report, sizeof(report), &t.x) == 0x1234 && mint_hid_field_value(report, sizeof(report), &t.y) == 0x5678 &&
          mint_hid_field_value(report, sizeof(report), &t.wheel) == -1, "tablet report decodes X=0x1234, Y=0x5678, wheel=-1");
    CHECK(mint_hid_field_value(report, 3, &t.y) == 0, "field reads stop at the end of a short report");

    MINT_HID_POINTER_LAYOUT m;
    ok = mint_hid_parse_pointer(boot_mouse, sizeof(boot_mouse), &m);
    CHECK(ok && m.x.bit == 8 && m.x.size == 8 && m.x.relative && m.x.min == -127 && m.y.bit == 16 && !m.wheel.present,
          "boot mouse: relative signed 8-bit X/Y at bits 8 and 16, constant padding skipped");
    const uint8_t mr[] = {0x02, 0xFE, 0x05};
    CHECK(mint_hid_field_value(mr, sizeof(mr), &m.x) == -2 && mint_hid_field_value(mr, sizeof(mr), &m.y) == 5,
          "boot mouse report sign-extends X=-2, Y=5");

    MINT_HID_POINTER_LAYOUT r;
    ok = mint_hid_parse_pointer(id_mouse, sizeof(id_mouse), &r);
    CHECK(ok && r.report_id == 2 && r.button_count == 5 && r.x.bit == 8 && r.x.size == 12 && r.y.bit == 20 && r.x.min == -2047,
          "report-ID mouse: ID 2, five buttons, packed 12-bit X/Y");
    const uint8_t rr[] = {0x00, 0xFF, 0x0F, 0x00};          /* payload after the ID byte: X=-1 (0xFFF), Y=0 */
    CHECK(mint_hid_field_value(rr, sizeof(rr), &r.x) == -1 && mint_hid_field_value(rr, sizeof(rr), &r.y) == 0,
          "12-bit packed fields sign-extend across byte boundaries");

    int truncated_ok = 1;
    for (size_t n = 0; n < sizeof(qemu_tablet); ++n) {
        MINT_HID_POINTER_LAYOUT x;
        if (mint_hid_parse_pointer(qemu_tablet, n, &x) != (n >= 54u)) truncated_ok = 0;   /* X/Y item completes at byte 54 */
    }
    const uint8_t long_item[] = {0xFE, 0xFF, 0x00};
    MINT_HID_POINTER_LAYOUT x;
    CHECK(truncated_ok && !mint_hid_parse_pointer(long_item, sizeof(long_item), &x) && !mint_hid_parse_pointer(0, 4, &x),
          "truncated, oversized long-item and null descriptors are rejected without over-reading");

    CHECK(mint_hid_usage_to_set1(0x04) == 0x1E && mint_hid_usage_to_set1(0x1D) == 0x2C && mint_hid_usage_to_set1(0x27) == 0x0B &&
          mint_hid_usage_to_set1(0x28) == 0x1C && mint_hid_usage_to_set1(0x2C) == 0x39, "HID usages map letters, digits, Enter and Space to set-1");
    CHECK(mint_hid_usage_to_set1(0x52) == 0x148 && mint_hid_usage_to_set1(0x51) == 0x150 && mint_hid_usage_to_set1(0x50) == 0x14B &&
          mint_hid_usage_to_set1(0x4F) == 0x14D && mint_hid_usage_to_set1(0x4C) == 0x153, "arrow keys and Delete map to E0-prefixed set-1 codes");
    CHECK(mint_hid_usage_to_set1(0x3A) == 0x3B && mint_hid_usage_to_set1(0x45) == 0x58 && mint_hid_usage_to_set1(0x00) == 0 &&
          mint_hid_usage_to_set1(0xE0) == 0, "F1..F12 map; reserved and modifier usages are not reported as keys");
    CHECK(mint_xhci_interval_exponent(3, 4) == 3 && mint_xhci_interval_exponent(4, 1) == 0 && mint_xhci_interval_exponent(3, 40) == 15,
          "HS/SS interrupt interval exponent is bInterval-1, clamped to 15");
    CHECK(mint_xhci_interval_exponent(1, 10) == 6 && mint_xhci_interval_exponent(2, 1) == 3 && mint_xhci_interval_exponent(1, 255) == 10,
          "FS/LS interval converts frames to 125 us exponents (min 3, max 10)");
}

static void test_checksums(void) {
    uint8_t ip[20] = {0x45,0x00,0x00,0x73,0x00,0x00,0x40,0x00,0x40,0x11,0x00,0x00,0xc0,0xa8,0x00,0x01,0xc0,0xa8,0x00,0xc7};
    uint16_t c = mint_net_checksum_fold(mint_net_checksum_add(0, ip, sizeof(ip)));
    CHECK(c == 0xB861, "IPv4 header checksum matches the RFC 1071 worked example (0xB861)");
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    CHECK(mint_net_checksum_fold(mint_net_checksum_add(0, ip, sizeof(ip))) == 0, "a header with its checksum verifies to zero");
    uint8_t odd[3] = {0x01, 0x02, 0x03};
    CHECK(mint_net_checksum_fold(mint_net_checksum_add(0, odd, 3)) == (uint16_t)~(0x0102 + 0x0300), "odd-length data pads the last byte");
    uint8_t udp[12] = {0x30,0x39,0x00,0x35,0x00,0x0C,0x00,0x00,'p','i','n','g'};
    uint16_t uc = mint_net_transport_checksum(0x0A00020F, 0x0A000203, 17, udp, sizeof(udp));
    udp[6] = (uint8_t)(uc >> 8); udp[7] = (uint8_t)uc;
    CHECK(uc && mint_net_transport_checksum(0x0A00020F, 0x0A000203, 17, udp, sizeof(udp)) == 0,
          "UDP pseudo-header checksum verifies to zero once inserted");
    CHECK(mint_tcp_seq_lt(0xFFFFFFF0u, 0x10u) && !mint_tcp_seq_lt(0x10u, 0xFFFFFFF0u) && mint_tcp_seq_le(5, 5),
          "TCP sequence comparison is correct across 32-bit wrap-around");
}

static void test_ipv4_text(void) {
    uint32_t ip = 0;
    CHECK(mint_ipv4_parse("10.0.2.15", &ip) && ip == 0x0A00020Fu, "parses 10.0.2.15");
    CHECK(mint_ipv4_parse("255.255.255.255", &ip) && ip == 0xFFFFFFFFu && mint_ipv4_parse("0.0.0.0", &ip) && ip == 0,
          "parses the all-ones and all-zero addresses");
    CHECK(!mint_ipv4_parse("256.1.1.1", &ip) && !mint_ipv4_parse("1.2.3", &ip) && !mint_ipv4_parse("1.2.3.4.5", &ip) &&
          !mint_ipv4_parse("1..2.3", &ip) && !mint_ipv4_parse("1234.1.1.1", &ip) && !mint_ipv4_parse("example.com", &ip) &&
          !mint_ipv4_parse("1.2.3.4 ", &ip) && !mint_ipv4_parse("", &ip), "rejects malformed, out-of-range and host-name text");
    char text[16];
    mint_ipv4_format(text, sizeof(text), 0xFFFFFF00u);
    char tiny[6];
    mint_ipv4_format(tiny, sizeof(tiny), 0xC0A80001u);
    CHECK(!strcmp(text, "255.255.255.0") && strlen(tiny) == 5 && !strncmp(tiny, "192.1", 5), "formats dotted quads and truncates safely");
}

static void test_dhcp(void) {
    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t m[600];
    size_t n = mint_dhcp_build(m, sizeof(m), MINT_DHCP_DISCOVER, 0x11223344u, mac, 0, 0, 0, "mintos");
    CHECK(n >= 300u && m[0] == 1 && m[1] == 1 && m[2] == 6 && mint_net_get32(m + 4) == 0x11223344u &&
          mint_net_get16(m + 10) == 0x8000u && !memcmp(m + 28, mac, 6) && mint_net_get32(m + 236) == 0x63825363u &&
          m[240] == 53 && m[241] == 1 && m[242] == MINT_DHCP_DISCOVER, "DISCOVER: BOOTREQUEST, xid, broadcast flag, MAC, cookie, type option");
    n = mint_dhcp_build(m, sizeof(m), MINT_DHCP_REQUEST, 7u, mac, 0, 0x0A00020Fu, 0x0A000202u, "mintos");
    int has50 = 0, has54 = 0;
    for (size_t i = 240; i + 6 <= n; ++i) {
        if (m[i] == 50 && m[i + 1] == 4 && mint_net_get32(m + i + 2) == 0x0A00020Fu) has50 = 1;
        if (m[i] == 54 && m[i + 1] == 4 && mint_net_get32(m + i + 2) == 0x0A000202u) has54 = 1;
    }
    CHECK(has50 && has54, "REQUEST carries the requested address (50) and server identifier (54)");
    CHECK(mint_dhcp_build(m, 299, MINT_DHCP_DISCOVER, 1, mac, 0, 0, 0, 0) == 0, "builder refuses buffers shorter than a BOOTP message");

    uint8_t r[320]; memset(r, 0, sizeof(r));
    r[0] = 2; r[1] = 1; r[2] = 6; mint_net_put32(r + 4, 0xCAFEu); mint_net_put32(r + 16, 0x0A00020Fu); memcpy(r + 28, mac, 6);
    mint_net_put32(r + 236, 0x63825363u);
    size_t o = 240;
    const uint8_t opts[] = {53,1,2, 54,4,10,0,2,2, 1,4,255,255,255,0, 3,4,10,0,2,2, 6,4,10,0,2,3, 51,4,0,1,81,128, 0, 255};
    memcpy(r + o, opts, sizeof(opts)); o += sizeof(opts);
    MINT_DHCP_REPLY rep;
    int ok = mint_dhcp_parse(r, o, 0xCAFEu, mac, &rep);
    CHECK(ok && rep.type == MINT_DHCP_OFFER && rep.your_ip == 0x0A00020Fu && rep.server_id == 0x0A000202u &&
          rep.netmask == 0xFFFFFF00u && rep.router == 0x0A000202u && rep.dns == 0x0A000203u && rep.lease_seconds == 86400u,
          "OFFER parses address, server, mask, router, DNS and lease (pad option skipped)");
    CHECK(!mint_dhcp_parse(r, o, 0xBEEFu, mac, &rep), "reply for another transaction ID is ignored");
    uint8_t other[6] = {2, 0, 0, 0, 0, 1};
    CHECK(!mint_dhcp_parse(r, o, 0xCAFEu, other, &rep), "reply for another hardware address is ignored");
    r[244] = 200;                                            /* option 54 claims 200 bytes */
    CHECK(!mint_dhcp_parse(r, o, 0xCAFEu, mac, &rep), "option length running past the packet is rejected");
    CHECK(!mint_dhcp_parse(r, 100, 0xCAFEu, mac, &rep), "truncated BOOTP header is rejected");
}

static size_t put_name(uint8_t *p, const char *a, const char *b) {
    size_t n = 0;
    p[n++] = (uint8_t)strlen(a); memcpy(p + n, a, strlen(a)); n += strlen(a);
    p[n++] = (uint8_t)strlen(b); memcpy(p + n, b, strlen(b)); n += strlen(b);
    p[n++] = 0;
    return n;
}

static void test_dns(void) {
    uint8_t q[300];
    size_t n = mint_dns_build_query(q, sizeof(q), 0x1234u, "example.com");
    static const uint8_t expect[] = {0x12,0x34,0x01,0x00,0,1,0,0,0,0,0,0, 7,'e','x','a','m','p','l','e',3,'c','o','m',0, 0,1, 0,1};
    CHECK(n == sizeof(expect) && !memcmp(q, expect, n), "query for example.com encodes header, labels, QTYPE A and QCLASS IN");
    CHECK(mint_dns_build_query(q, sizeof(q), 1, "example.com.") == n, "a single trailing dot is accepted");
    char long_label[80]; memset(long_label, 'a', 64); long_label[64] = '\0';
    CHECK(!mint_dns_build_query(q, sizeof(q), 1, "") && !mint_dns_build_query(q, sizeof(q), 1, "a..b") &&
          !mint_dns_build_query(q, sizeof(q), 1, ".com") && !mint_dns_build_query(q, sizeof(q), 1, long_label) &&
          !mint_dns_build_query(q, 20, 1, "example.com"), "empty names, empty labels, 64-byte labels and small buffers are refused");

    /* Response: question, then CNAME www.example.com -> example.com (compressed), then A. */
    uint8_t r[512]; memset(r, 0, sizeof(r));
    mint_net_put16(r, 0x4242u); r[2] = 0x81; r[3] = 0x80; mint_net_put16(r + 4, 1); mint_net_put16(r + 6, 2);
    size_t o = 12;
    r[o++] = 3; memcpy(r + o, "www", 3); o += 3;
    size_t example_at = o;
    o += put_name(r + o, "example", "com");
    mint_net_put16(r + o, 1); mint_net_put16(r + o + 2, 1); o += 4;
    r[o++] = 0xC0; r[o++] = 12;                               /* name: pointer to www.example.com */
    mint_net_put16(r + o, 5); mint_net_put16(r + o + 2, 1); mint_net_put32(r + o + 4, 300); mint_net_put16(r + o + 8, 2); o += 10;
    r[o++] = 0xC0; r[o++] = (uint8_t)example_at;              /* CNAME rdata: pointer to example.com */
    r[o++] = 0xC0; r[o++] = (uint8_t)example_at;
    mint_net_put16(r + o, 1); mint_net_put16(r + o + 2, 1); mint_net_put32(r + o + 4, 120); mint_net_put16(r + o + 8, 4); o += 10;
    r[o++] = 93; r[o++] = 184; r[o++] = 215; r[o++] = 14;
    uint32_t ip = 0, ttl = 0;
    CHECK(mint_dns_parse_a(r, o, 0x4242u, &ip, &ttl) == 1 && ip == 0x5DB8D70Eu && ttl == 120,
          "parses an A record after a CNAME, following compression pointers");
    CHECK(mint_dns_parse_a(r, o, 0x4243u, &ip, &ttl) == -2, "a response with another ID is rejected");
    CHECK(mint_dns_parse_a(r, o - 3, 0x4242u, &ip, &ttl) == -2, "truncated answer data is rejected");
    r[3] = 0x83;
    CHECK(mint_dns_parse_a(r, o, 0x4242u, &ip, &ttl) == -1, "NXDOMAIN is reported as -1");
    r[3] = 0x80; mint_net_put16(r + 6, 1);
    CHECK(mint_dns_parse_a(r, o, 0x4242u, &ip, &ttl) == 0, "an answer with no A record is reported as 0");
    uint8_t loop[40]; memset(loop, 0, sizeof(loop));
    mint_net_put16(loop, 9); loop[2] = 0x81; mint_net_put16(loop + 4, 1);
    loop[12] = 0x01; loop[13] = 'a'; loop[14] = 0x01; loop[15] = 'b'; loop[16] = 0x01;   /* labels never terminate */
    CHECK(mint_dns_parse_a(loop, 17, 9, &ip, &ttl) == -2, "an unterminated question name is rejected");
    uint8_t q2[20]; memset(q2, 0, sizeof(q2));
    mint_net_put16(q2, 9); q2[2] = 0x80;
    CHECK(mint_dns_parse_a(q2, sizeof(q2), 9, &ip, &ttl) == 0 && mint_dns_parse_a(q2, 11, 9, &ip, &ttl) == -2,
          "empty answers parse; headers shorter than 12 bytes are rejected");
    q2[2] = 0x01;
    CHECK(mint_dns_parse_a(q2, sizeof(q2), 9, &ip, &ttl) == -2, "a query (QR=0) is not accepted as a response");
}

static void test_tcp_options(void) {
    const uint8_t o1[] = {1, 1, 2, 4, 0x05, 0xB4, 3, 3, 7};
    const uint8_t o2[] = {3, 3, 7, 2, 4, 0x02, 0x18};
    const uint8_t bad[] = {1, 8, 0};
    const uint8_t short_mss[] = {2, 4, 0x05};
    CHECK(mint_tcp_parse_mss(o1, sizeof(o1)) == 1460 && mint_tcp_parse_mss(o2, sizeof(o2)) == 536, "finds the MSS option among NOP and window-scale");
    CHECK(!mint_tcp_parse_mss(bad, sizeof(bad)) && !mint_tcp_parse_mss(short_mss, sizeof(short_mss)) && !mint_tcp_parse_mss(0, 4),
          "malformed or truncated options yield no MSS");
}

int main(void) {
    test_hid();
    test_checksums();
    test_ipv4_text();
    test_dhcp();
    test_dns();
    test_tcp_options();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    return 0;
}
