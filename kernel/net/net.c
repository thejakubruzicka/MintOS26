/* MintOS26 IPv4 network stack.
 *
 * Runs on the cooperative scheduler: the "net" task polls the NIC once per
 * timer tick (100 Hz) and drives every timer; syscalls call straight into the
 * same state from the desktop process. Neither side yields while touching the
 * stack, so no locking is needed. Everything is statically sized:
 *   ARP cache 16, one frame parked per ARP miss, DNS cache 8, 16 ping slots,
 *   8 TCP sockets with 32 KiB receive and 16 KiB send buffers.
 * Not implemented: IP fragmentation/reassembly, IPv6, TCP listen/accept,
 * window scaling, SACK, out-of-order queueing (such segments are dropped and
 * re-acknowledged), user UDP sockets and TLS. */
#include <stdint.h>
#include <stddef.h>
#include "../include/net.h"
#include "../include/e1000.h"
#include "../include/arch.h"
#include "../include/task.h"
#include "../include/serial.h"
#include "../include/log.h"
#include "../include/uapi/mintos/syscall.h"

#define HZ 100u
#define ETH_HDR 14u
#define IP_HDR 20u
#define FRAME_MAX 1514u
#define MTU 1500u
#define ETH_ARP 0x0806u
#define ETH_IPV4 0x0800u
#define PROTO_ICMP 1u
#define PROTO_TCP 6u
#define PROTO_UDP 17u
#define BROADCAST_IP 0xFFFFFFFFu
#define PING_ID 0x4D49u
#define DNS_LOCAL_PORT 49153u
#define ARP_SLOTS 16u
#define DNS_SLOTS 8u
#define PING_SLOTS 16u
#define TCP_SLOTS 8u
#define TCP_RX_BUF 32768u
#define TCP_TX_BUF 16384u
#define TCP_MSS 1460u
#define TCP_RTO_INITIAL 100u          /* ticks */
#define TCP_RTO_MAX 600u
#define TCP_SYN_RETRIES 5u
#define TCP_DATA_RETRIES 8u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ------------------------------------------------------------------ state */
enum { DHCP_IDLE = 0, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

static struct {
    uint32_t state;
    uint8_t nic_started, link, wanted;
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns, server;
    uint32_t lease;
    uint64_t lease_tick, link_lost_tick;
    uint32_t dhcp_phase, dhcp_tries, xid, offered, offer_server;
    uint64_t dhcp_next;
    uint16_t ip_id, next_port;
    uint32_t rng;
    char status[64];
} net;
static MINT_NET_INFO stats;

static uint8_t rx_frame[2048];
static uint8_t tx_frame[FRAME_MAX + 2u];

typedef struct arp_entry { uint32_t ip; uint8_t mac[6]; uint8_t valid; uint64_t tick; } ARP_ENTRY;
static ARP_ENTRY arp_cache[ARP_SLOTS];
static struct { uint8_t frame[FRAME_MAX]; uint32_t length, next_hop; uint64_t tick; uint8_t used; } arp_pending;
static uint32_t arp_last_request_ip;
static uint64_t arp_last_request_tick;

enum { DNS_FREE = 0, DNS_PENDING, DNS_DONE, DNS_MISSING, DNS_TIMEOUT };
typedef struct dns_entry {
    char name[64];
    uint8_t state; uint16_t id; uint32_t ip, tries;
    uint64_t next, expires, used;
} DNS_ENTRY;
static DNS_ENTRY dns_cache[DNS_SLOTS];

typedef struct ping_slot {
    uint8_t used, done, ttl; uint16_t seq; uint32_t ip, rtt_us;
    uint64_t tsc_sent, tick_sent;
} PING_SLOT;
static PING_SLOT pings[PING_SLOTS];

enum { TS_FREE = 0, TS_SYN_SENT, TS_ESTABLISHED, TS_FIN_WAIT_1, TS_FIN_WAIT_2, TS_CLOSING,
       TS_TIME_WAIT, TS_CLOSE_WAIT, TS_LAST_ACK, TS_CLOSED };
typedef struct tcp_sock {
    uint8_t state, app_closed, peer_fin, fin_acked, error, generation, probe;
    uint16_t lport, rport, mss;
    uint32_t rip;
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, rcv_nxt, adv_wnd;
    uint32_t rto, retries;
    uint64_t rto_deadline, linger_deadline;
    uint32_t tx_len, rx_head, rx_len;
    uint8_t rx[TCP_RX_BUF];
    uint8_t tx[TCP_TX_BUF];
} TCP_SOCK;
static TCP_SOCK tcps[TCP_SLOTS];

/* ------------------------------------------------------------------ helpers */
static uint64_t ticks(void) { return mint_timer_ticks(); }
static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t tsc_per_us, cal_tick0, cal_tsc0;

static void bytes_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}
static void bytes_zero(void *dst, size_t n) { uint8_t *d = (uint8_t *)dst; while (n--) *d++ = 0; }
static int mac_equal(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 6u; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static uint32_t random32(void) {
    if (!net.rng) net.rng = (uint32_t)rdtsc() | 1u;
    net.rng ^= net.rng << 13; net.rng ^= net.rng >> 17; net.rng ^= net.rng << 5;
    return net.rng ^ (uint32_t)rdtsc();
}
static void set_status(const char *text) {
    uint32_t i = 0;
    for (; text && text[i] && i + 1u < sizeof(net.status); ++i) net.status[i] = text[i];
    net.status[i] = '\0';
}
static void log_ip(const char *prefix, uint32_t ip) {
    char text[16];
    (void)mint_ipv4_format(text, sizeof(text), ip);
    mint_serial_write(prefix); mint_serial_write(text);
}
static uint8_t *ip_payload(void) { return tx_frame + ETH_HDR + IP_HDR; }

/* ------------------------------------------------------------------ link layer */
static int eth_send(const uint8_t dst[6], uint16_t type, uint8_t *frame, uint32_t payload_length) {
    bytes_copy(frame, dst, 6); bytes_copy(frame + 6, net.mac, 6);
    mint_net_put16(frame + 12, type);
    uint32_t length = ETH_HDR + payload_length;
    if (!mint_e1000_send(frame, length)) return 0;
    ++stats.tx_frames; stats.tx_bytes += length;
    return 1;
}

static void arp_send(uint16_t op, const uint8_t target_mac[6], uint32_t target_ip, const uint8_t dst[6]) {
    uint8_t frame[64];
    uint8_t *a = frame + ETH_HDR;
    mint_net_put16(a, 1u); mint_net_put16(a + 2, ETH_IPV4); a[4] = 6; a[5] = 4;
    mint_net_put16(a + 6, op);
    bytes_copy(a + 8, net.mac, 6); mint_net_put32(a + 14, net.ip);
    bytes_copy(a + 18, target_mac, 6); mint_net_put32(a + 24, target_ip);
    (void)eth_send(dst, ETH_ARP, frame, 28u);
}

static ARP_ENTRY *arp_find(uint32_t ip) {
    for (uint32_t i = 0; i < ARP_SLOTS; ++i) if (arp_cache[i].valid && arp_cache[i].ip == ip) return &arp_cache[i];
    return 0;
}
static void arp_store(uint32_t ip, const uint8_t mac[6]) {
    ARP_ENTRY *e = arp_find(ip);
    if (!e) {
        e = &arp_cache[0];
        for (uint32_t i = 0; i < ARP_SLOTS; ++i) {
            if (!arp_cache[i].valid) { e = &arp_cache[i]; break; }
            if (arp_cache[i].tick < e->tick) e = &arp_cache[i];
        }
    }
    e->ip = ip; bytes_copy(e->mac, mac, 6); e->valid = 1; e->tick = ticks();
}

static void arp_input(const uint8_t *a, uint32_t length) {
    if (length < 28u || mint_net_get16(a) != 1u || mint_net_get16(a + 2) != ETH_IPV4 || a[4] != 6 || a[5] != 4) return;
    uint16_t op = mint_net_get16(a + 6);
    uint32_t spa = mint_net_get32(a + 14), tpa = mint_net_get32(a + 24);
    const uint8_t *sha = a + 8;
    if (!spa || (sha[0] & 1u)) return;
    if (net.ip && tpa == net.ip) arp_store(spa, sha);
    else if (arp_find(spa)) arp_store(spa, sha);
    if (op == 1u && net.ip && tpa == net.ip) arp_send(2u, sha, spa, sha);
    if (op == 2u) ++stats.arp_replies;
    if (arp_pending.used && arp_pending.next_hop == spa) {
        bytes_copy(arp_pending.frame, sha, 6);
        if (mint_e1000_send(arp_pending.frame, arp_pending.length)) {
            ++stats.tx_frames; stats.tx_bytes += arp_pending.length;
        }
        arp_pending.used = 0;
    }
}

static void arp_request(uint32_t ip) {
    uint64_t now = ticks();
    if (ip == arp_last_request_ip && now - arp_last_request_tick < HZ / 4u) return;
    arp_last_request_ip = ip; arp_last_request_tick = now;
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    arp_send(1u, zero, ip, broadcast_mac);
}

/* ------------------------------------------------------------------ IPv4 */
static int is_broadcast(uint32_t ip) {
    return ip == BROADCAST_IP || (net.ip && net.mask && net.mask != BROADCAST_IP && ip == (net.ip | ~net.mask));
}

/* Sends tx_frame's IP payload (already written at ip_payload()). */
static int ip_output(uint32_t dst, uint8_t protocol, uint32_t payload_length) {
    if (payload_length > MTU - IP_HDR) return 0;
    uint8_t *h = tx_frame + ETH_HDR;
    h[0] = 0x45; h[1] = 0;
    mint_net_put16(h + 2, (uint16_t)(IP_HDR + payload_length));
    mint_net_put16(h + 4, ++net.ip_id);
    mint_net_put16(h + 6, protocol == PROTO_TCP ? 0x4000u : 0u);    /* DF for TCP */
    h[8] = 64; h[9] = protocol;
    mint_net_put16(h + 10, 0);
    mint_net_put32(h + 12, net.ip); mint_net_put32(h + 16, dst);
    mint_net_put16(h + 10, mint_net_checksum_fold(mint_net_checksum_add(0, h, IP_HDR)));
    uint32_t total = IP_HDR + payload_length;
    if (is_broadcast(dst)) return eth_send(broadcast_mac, ETH_IPV4, tx_frame, total);
    uint32_t hop = (net.mask && (dst & net.mask) == (net.ip & net.mask)) ? dst : net.gw;
    if (!hop) return 0;
    ARP_ENTRY *e = arp_find(hop);
    if (e) return eth_send(e->mac, ETH_IPV4, tx_frame, total);
    /* Park this frame until the ARP reply arrives; upper layers retry anyway. */
    bytes_copy(tx_frame + 6, net.mac, 6); mint_net_put16(tx_frame + 12, ETH_IPV4);
    bytes_copy(arp_pending.frame, tx_frame, ETH_HDR + total);
    arp_pending.length = ETH_HDR + total; arp_pending.next_hop = hop;
    arp_pending.tick = ticks(); arp_pending.used = 1;
    arp_request(hop);
    return 1;
}

/* ------------------------------------------------------------------ UDP */
static int udp_output(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t length) {
    uint8_t *u = ip_payload();
    mint_net_put16(u, sport); mint_net_put16(u + 2, dport);
    mint_net_put16(u + 4, (uint16_t)(8u + length)); mint_net_put16(u + 6, 0);
    uint16_t c = mint_net_transport_checksum(net.ip, dst, PROTO_UDP, u, 8u + length);
    mint_net_put16(u + 6, c ? c : 0xFFFFu);
    return ip_output(dst, PROTO_UDP, 8u + length);
}

/* ------------------------------------------------------------------ DHCP */
static void dhcp_send(uint8_t type) {
    uint8_t *payload = ip_payload() + 8;
    uint32_t requested = type == MINT_DHCP_REQUEST ? (net.dhcp_phase == DHCP_REQUESTING && net.offered ? net.offered : net.ip) : 0u;
    size_t n = mint_dhcp_build(payload, MTU - IP_HDR - 8u, type, net.xid, net.mac, 0u, requested,
                               type == MINT_DHCP_REQUEST ? net.offer_server : 0u, "mintos");
    if (n) (void)udp_output(BROADCAST_IP, 68u, 67u, (uint32_t)n);
}

static void dhcp_start(void) {
    net.xid = random32();
    net.dhcp_phase = DHCP_SELECTING; net.dhcp_tries = 0; net.dhcp_next = ticks();
    net.offered = 0; net.offer_server = 0;
    if (net.state != MINT_NET_STATE_UP) { net.state = MINT_NET_STATE_DHCP; set_status("Looking for a DHCP server"); }
}

static void dhcp_input(const uint8_t *p, uint32_t length, uint32_t source) {
    MINT_DHCP_REPLY r;
    if (net.dhcp_phase == DHCP_IDLE || net.dhcp_phase == DHCP_BOUND || !mint_dhcp_parse(p, length, net.xid, net.mac, &r)) return;
    if (r.type == MINT_DHCP_OFFER && net.dhcp_phase == DHCP_SELECTING && r.your_ip) {
        net.offered = r.your_ip; net.offer_server = r.server_id ? r.server_id : source;
        net.dhcp_phase = DHCP_REQUESTING; net.dhcp_tries = 1; net.dhcp_next = ticks() + 2u * HZ;
        dhcp_send(MINT_DHCP_REQUEST);
        return;
    }
    if (r.type == MINT_DHCP_NAK) {
        mint_serial_write("[net] DHCP server declined the request; restarting\n");
        if (net.state == MINT_NET_STATE_UP) { net.ip = 0; net.state = MINT_NET_STATE_DHCP; }
        dhcp_start();
        return;
    }
    if (r.type == MINT_DHCP_ACK && net.dhcp_phase == DHCP_REQUESTING && r.your_ip) {
        int renewed = net.state == MINT_NET_STATE_UP && net.ip == r.your_ip;
        net.ip = r.your_ip;
        net.mask = r.netmask ? r.netmask : 0xFFFFFF00u;
        net.gw = r.router;
        net.dns = r.dns ? r.dns : r.router;
        net.server = r.server_id ? r.server_id : source;
        net.lease = r.lease_seconds ? r.lease_seconds : 3600u;
        net.lease_tick = ticks();
        net.dhcp_phase = DHCP_BOUND;
        net.state = MINT_NET_STATE_UP;
        set_status("Connected");
        if (!renewed) {
            log_ip("[net] DHCP lease ", net.ip); log_ip(" mask ", net.mask);
            log_ip(" gateway ", net.gw); log_ip(" dns ", net.dns); mint_serial_write("\n");
            mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_NET, "DHCP lease obtained; IPv4 network is up");
            if (net.gw) arp_request(net.gw);
        }
    }
}

static void dhcp_timer(void) {
    uint64_t now = ticks();
    if (net.dhcp_phase == DHCP_BOUND) {
        if (net.lease && now - net.lease_tick >= (uint64_t)net.lease * HZ / 2u) {
            net.xid = random32(); net.offered = net.ip; net.offer_server = net.server;
            net.dhcp_phase = DHCP_REQUESTING; net.dhcp_tries = 0; net.dhcp_next = now;
        }
        return;
    }
    if (net.dhcp_phase != DHCP_SELECTING && net.dhcp_phase != DHCP_REQUESTING) return;
    if (now < net.dhcp_next) return;
    if (net.dhcp_tries >= 6u) {
        if (net.state == MINT_NET_STATE_UP && net.dhcp_phase == DHCP_REQUESTING &&
            now - net.lease_tick < (uint64_t)net.lease * HZ) {
            net.dhcp_phase = DHCP_BOUND; net.lease_tick = now - (uint64_t)net.lease * HZ / 4u;  /* retry later */
            return;
        }
        net.dhcp_phase = DHCP_IDLE; net.state = MINT_NET_STATE_FAILED; net.ip = 0;
        set_status("No DHCP server answered");
        mint_serial_write("[net] DHCP: no server answered after 6 attempts\n");
        mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_NET, "DHCP gave up: no server answered");
        return;
    }
    ++net.dhcp_tries;
    net.dhcp_next = now + (uint64_t)(net.dhcp_tries < 3u ? 2u : 4u) * HZ;
    dhcp_send(net.dhcp_phase == DHCP_SELECTING ? MINT_DHCP_DISCOVER : MINT_DHCP_REQUEST);
}

/* ------------------------------------------------------------------ DNS */
static int name_equal(const char *a, const char *b) {
    for (uint32_t i = 0; i < 64u; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 0;
}

static void dns_send(DNS_ENTRY *e) {
    uint8_t *payload = ip_payload() + 8;
    size_t n = mint_dns_build_query(payload, 512u, e->id, e->name);
    if (!n) { e->state = DNS_MISSING; return; }
    ++e->tries; e->next = ticks() + (uint64_t)e->tries * HZ;
    ++stats.dns_queries;
    (void)udp_output(net.dns, DNS_LOCAL_PORT, 53u, (uint32_t)n);
}

static void dns_input(const uint8_t *p, uint32_t length) {
    if (length < 12u) return;
    uint16_t id = mint_net_get16(p);
    for (uint32_t i = 0; i < DNS_SLOTS; ++i) {
        DNS_ENTRY *e = &dns_cache[i];
        if (e->state != DNS_PENDING || e->id != id) continue;
        uint32_t ip = 0, ttl = 0;
        int rc = mint_dns_parse_a(p, length, id, &ip, &ttl);
        if (rc == -2) return;
        if (rc == 1) {
            if (ttl < 30u) ttl = 30u;
            if (ttl > 600u) ttl = 600u;
            e->state = DNS_DONE; e->ip = ip; e->expires = ticks() + (uint64_t)ttl * HZ;
        } else e->state = DNS_MISSING;
        return;
    }
}

static void dns_timer(void) {
    uint64_t now = ticks();
    for (uint32_t i = 0; i < DNS_SLOTS; ++i) {
        DNS_ENTRY *e = &dns_cache[i];
        if (e->state != DNS_PENDING || now < e->next) continue;
        if (e->tries >= 4u) { e->state = DNS_TIMEOUT; continue; }
        dns_send(e);
    }
}

int64_t mint_net_resolve(const char *name, uint32_t *ip_out) {
    if (!name || !ip_out || !name[0]) return -MINT_EINVAL;
    if (mint_ipv4_parse(name, ip_out)) return 0;
    size_t len = 0;
    while (name[len] && len < 64u) {
        char c = name[len];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
            return -MINT_EINVAL;
        ++len;
    }
    if (len >= 64u) return -MINT_EINVAL;
    if (net.state != MINT_NET_STATE_UP || !net.dns) return -MINT_ENETDOWN;
    uint64_t now = ticks();
    for (uint32_t i = 0; i < DNS_SLOTS; ++i) {
        DNS_ENTRY *e = &dns_cache[i];
        if (e->state == DNS_FREE || !name_equal(e->name, name)) continue;
        e->used = now;
        if (e->state == DNS_DONE && now < e->expires) { *ip_out = e->ip; return 0; }
        if (e->state == DNS_PENDING) return -MINT_EAGAIN;
        if (e->state == DNS_MISSING) { e->state = DNS_FREE; return -MINT_ENOENT; }
        if (e->state == DNS_TIMEOUT) { e->state = DNS_FREE; return -MINT_ETIMEDOUT; }
        e->state = DNS_PENDING; e->tries = 0; e->id = (uint16_t)random32();   /* expired */
        dns_send(e);
        return -MINT_EAGAIN;
    }
    DNS_ENTRY *slot = 0;
    for (uint32_t i = 0; i < DNS_SLOTS; ++i) {
        DNS_ENTRY *e = &dns_cache[i];
        if (e->state == DNS_FREE) { slot = e; break; }
        if (e->state != DNS_PENDING && (!slot || e->used < slot->used)) slot = e;
    }
    if (!slot) return -MINT_EBUSY;
    bytes_zero(slot, sizeof(*slot));
    bytes_copy(slot->name, name, len); slot->name[len] = '\0';
    slot->state = DNS_PENDING; slot->id = (uint16_t)random32(); slot->used = now;
    dns_send(slot);
    return -MINT_EAGAIN;
}

/* ------------------------------------------------------------------ ICMP */
static void tcp_icmp_unreachable(uint32_t dst, const uint8_t *orig, uint32_t length);

static void icmp_input(uint32_t src, uint8_t ttl, const uint8_t *p, uint32_t length) {
    if (length < 8u || mint_net_checksum_fold(mint_net_checksum_add(0, p, length)) != 0) return;
    if (p[0] == 8u && net.ip) {                                     /* echo request */
        if (length > MTU - IP_HDR) return;
        uint8_t *o = ip_payload();
        bytes_copy(o, p, length);
        o[0] = 0; o[2] = 0; o[3] = 0;
        mint_net_put16(o + 2, mint_net_checksum_fold(mint_net_checksum_add(0, o, length)));
        if (ip_output(src, PROTO_ICMP, length)) ++stats.icmp_echo_answered;
    } else if (p[0] == 0u && mint_net_get16(p + 4) == PING_ID) {   /* echo reply */
        uint16_t seq = mint_net_get16(p + 6);
        PING_SLOT *s = &pings[seq % PING_SLOTS];
        if (!s->used || s->done || s->seq != seq || s->ip != src) return;
        uint64_t us = tsc_per_us ? (rdtsc() - s->tsc_sent) / tsc_per_us : (ticks() - s->tick_sent) * 10000u;
        s->rtt_us = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us;
        s->ttl = ttl; s->done = 1;
    } else if (p[0] == 3u && length >= 8u + 20u + 8u) {             /* destination unreachable */
        const uint8_t *orig = p + 8;
        uint32_t ohl = (uint32_t)(orig[0] & 0x0Fu) * 4u;
        if (orig[9] == PROTO_TCP && ohl >= 20u && length >= 8u + ohl + 8u)
            tcp_icmp_unreachable(mint_net_get32(orig + 16), orig + ohl, length - 8u - ohl);
    }
}

int64_t mint_net_ping(uint32_t op, uint32_t ip, uint32_t seq) {
    if (op == MINT_NET_PING_SEND) {
        if (net.state != MINT_NET_STATE_UP) return -MINT_ENETDOWN;
        if (!ip || ip == BROADCAST_IP || seq > 0xFFFFu) return -MINT_EINVAL;
        PING_SLOT *s = &pings[seq % PING_SLOTS];
        bytes_zero(s, sizeof(*s));
        s->used = 1; s->seq = (uint16_t)seq; s->ip = ip;
        uint8_t *o = ip_payload();
        o[0] = 8; o[1] = 0; mint_net_put16(o + 2, 0);
        mint_net_put16(o + 4, PING_ID); mint_net_put16(o + 6, (uint16_t)seq);
        for (uint32_t i = 0; i < 56u; ++i) o[8 + i] = (uint8_t)(0x20u + i);
        mint_net_put16(o + 2, mint_net_checksum_fold(mint_net_checksum_add(0, o, 64u)));
        s->tsc_sent = rdtsc(); s->tick_sent = ticks();
        return ip_output(ip, PROTO_ICMP, 64u) ? 0 : -MINT_EHOSTUNREACH;
    }
    if (op == MINT_NET_PING_POLL) {
        PING_SLOT *s = &pings[seq % PING_SLOTS];
        if (!s->used || s->seq != (uint16_t)seq) return -MINT_EINVAL;
        if (!s->done) return -MINT_EAGAIN;
        return (int64_t)(((uint64_t)s->ttl << 32) | s->rtt_us);
    }
    return -MINT_EINVAL;
}

/* ------------------------------------------------------------------ TCP */
static uint32_t tcp_window(const TCP_SOCK *s) {
    uint32_t free = TCP_RX_BUF - s->rx_len;
    return free > 0xFFFFu ? 0xFFFFu : free;
}

static int tcp_raw(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                   uint16_t window, const uint8_t *data, uint32_t length, int with_mss) {
    uint32_t hl = with_mss ? 24u : 20u;
    if (length > MTU - IP_HDR - hl) return 0;
    uint8_t *t = ip_payload();
    mint_net_put16(t, sport); mint_net_put16(t + 2, dport);
    mint_net_put32(t + 4, seq); mint_net_put32(t + 8, (flags & TCP_ACK) ? ack : 0u);
    t[12] = (uint8_t)((hl / 4u) << 4); t[13] = flags;
    mint_net_put16(t + 14, window); mint_net_put16(t + 16, 0); mint_net_put16(t + 18, 0);
    if (with_mss) { t[20] = 2; t[21] = 4; mint_net_put16(t + 22, TCP_MSS); }
    if (length) bytes_copy(t + hl, data, length);
    mint_net_put16(t + 16, mint_net_transport_checksum(net.ip, dst, PROTO_TCP, t, hl + length));
    ++stats.tcp_segments_out;
    return ip_output(dst, PROTO_TCP, hl + length);
}

static int tcp_segment(TCP_SOCK *s, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t length) {
    s->adv_wnd = tcp_window(s);
    return tcp_raw(s->rip, s->lport, s->rport, seq, s->rcv_nxt, flags, (uint16_t)s->adv_wnd, data, length,
                   (flags & TCP_SYN) != 0);
}
static void tcp_ack_now(TCP_SOCK *s) { (void)tcp_segment(s, s->snd_nxt, TCP_ACK, 0, 0); }

static void tcp_free(TCP_SOCK *s) { s->state = TS_FREE; s->rto_deadline = 0; s->tx_len = 0; s->rx_len = 0; }

static void tcp_fail(TCP_SOCK *s, uint8_t error) {
    s->state = TS_CLOSED; s->error = error; s->rto_deadline = 0;
    if (s->app_closed) tcp_free(s);
}

static void tcp_arm(TCP_SOCK *s) { if (!s->rto_deadline) s->rto_deadline = ticks() + s->rto; }

static void tcp_enter_time_wait(TCP_SOCK *s) {
    s->state = TS_TIME_WAIT; s->rto_deadline = 0;
    s->linger_deadline = ticks() + 2u * HZ;
}

static void tcp_output(TCP_SOCK *s) {
    if (s->state != TS_ESTABLISHED && s->state != TS_CLOSE_WAIT && s->state != TS_FIN_WAIT_1 &&
        s->state != TS_LAST_ACK && s->state != TS_CLOSING) return;
    for (uint32_t budget = 0; budget < 24u; ++budget) {
        uint32_t data_end = s->snd_una + s->tx_len;
        if (s->fin_acked) return;
        if (mint_tcp_seq_lt(s->snd_nxt, data_end)) {
            uint32_t offset = s->snd_nxt - s->snd_una, in_flight = offset;
            uint32_t window = s->snd_wnd ? s->snd_wnd : (s->probe ? 1u : 0u);
            if (in_flight >= window) { if (!s->snd_wnd) tcp_arm(s); return; }
            uint32_t n = s->tx_len - offset;
            if (n > s->mss) n = s->mss;
            if (n > window - in_flight) n = window - in_flight;
            if (!tcp_segment(s, s->snd_nxt, TCP_ACK | TCP_PSH, s->tx + offset, n)) return;
            s->snd_nxt += n;
            if (mint_tcp_seq_lt(s->snd_max, s->snd_nxt)) s->snd_max = s->snd_nxt;
            tcp_arm(s);
            continue;
        }
        if (s->app_closed && s->snd_nxt == data_end) {
            if (!tcp_segment(s, s->snd_nxt, TCP_ACK | TCP_FIN, 0, 0)) return;
            s->snd_nxt += 1u;
            if (mint_tcp_seq_lt(s->snd_max, s->snd_nxt)) s->snd_max = s->snd_nxt;
            if (s->state == TS_ESTABLISHED) s->state = TS_FIN_WAIT_1;
            else if (s->state == TS_CLOSE_WAIT) s->state = TS_LAST_ACK;
            tcp_arm(s);
        }
        return;
    }
}

static TCP_SOCK *tcp_lookup(int64_t handle) {
    if (handle < 0) return 0;
    uint32_t index = (uint32_t)handle & 0xFFu, generation = ((uint32_t)handle >> 8) & 0xFFu;
    if (index >= TCP_SLOTS) return 0;
    TCP_SOCK *s = &tcps[index];
    if (s->state == TS_FREE || s->app_closed || s->generation != generation) return 0;
    return s;
}

static void tcp_icmp_unreachable(uint32_t dst, const uint8_t *orig_tcp, uint32_t length) {
    (void)length;
    uint16_t sport = mint_net_get16(orig_tcp), dport = mint_net_get16(orig_tcp + 2);
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) {
        TCP_SOCK *s = &tcps[i];
        if (s->state == TS_SYN_SENT && s->rip == dst && s->lport == sport && s->rport == dport)
            tcp_fail(s, MINT_TCP_STATE_UNREACHABLE);
    }
}

static void tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, uint32_t length) {
    if (length < 20u || mint_net_transport_checksum(src, dst, PROTO_TCP, p, length) != 0) return;
    uint16_t sport = mint_net_get16(p), dport = mint_net_get16(p + 2);
    uint32_t seq = mint_net_get32(p + 4), ack = mint_net_get32(p + 8);
    uint32_t hl = (uint32_t)(p[12] >> 4) * 4u;
    uint8_t flags = p[13];
    uint32_t window = mint_net_get16(p + 14);
    if (hl < 20u || hl > length) return;
    const uint8_t *data = p + hl;
    uint32_t dlen = length - hl;
    ++stats.tcp_segments_in;

    TCP_SOCK *s = 0;
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) {
        TCP_SOCK *c = &tcps[i];
        if (c->state != TS_FREE && c->state != TS_CLOSED && c->lport == dport && c->rport == sport && c->rip == src) { s = c; break; }
    }
    if (!s) {
        if (flags & TCP_RST) return;
        if (flags & TCP_ACK) (void)tcp_raw(src, dport, sport, ack, 0, TCP_RST, 0, 0, 0, 0);
        else (void)tcp_raw(src, dport, sport, 0, seq + dlen + ((flags & TCP_SYN) ? 1u : 0u) + ((flags & TCP_FIN) ? 1u : 0u),
                           TCP_RST | TCP_ACK, 0, 0, 0, 0);
        return;
    }

    if (s->state == TS_SYN_SENT) {
        if ((flags & TCP_ACK) && ack != s->iss + 1u) {
            if (!(flags & TCP_RST)) (void)tcp_raw(src, dport, sport, ack, 0, TCP_RST, 0, 0, 0, 0);
            return;
        }
        if (flags & TCP_RST) { if (flags & TCP_ACK) tcp_fail(s, MINT_TCP_STATE_REFUSED); return; }
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            s->rcv_nxt = seq + 1u;
            s->snd_una = ack; s->snd_nxt = ack; s->snd_max = ack;
            s->snd_wnd = window;
            uint16_t mss = mint_tcp_parse_mss(p + 20, hl - 20u);
            s->mss = mss ? (mss < TCP_MSS ? mss : TCP_MSS) : 536u;
            if (s->mss < 64u) s->mss = 64u;
            s->state = TS_ESTABLISHED; s->retries = 0; s->rto = TCP_RTO_INITIAL; s->rto_deadline = 0;
            tcp_ack_now(s);
            tcp_output(s);
        }
        return;
    }

    if (flags & TCP_RST) {
        uint32_t wnd = tcp_window(s);
        if (seq == s->rcv_nxt || (mint_tcp_seq_le(s->rcv_nxt, seq) && mint_tcp_seq_lt(seq, s->rcv_nxt + (wnd ? wnd : 1u))))
            tcp_fail(s, MINT_TCP_STATE_RESET);
        return;
    }
    if (flags & TCP_SYN) {
        if (seq + 1u == s->rcv_nxt) tcp_ack_now(s);       /* our handshake ACK was lost */
        else { (void)tcp_segment(s, s->snd_nxt, TCP_RST, 0, 0); tcp_fail(s, MINT_TCP_STATE_RESET); }
        return;
    }
    if (!(flags & TCP_ACK)) return;

    /* Acknowledgement. */
    if (mint_tcp_seq_lt(s->snd_una, ack) && mint_tcp_seq_le(ack, s->snd_max)) {
        uint32_t acked = ack - s->snd_una;
        uint32_t data_acked = acked > s->tx_len ? s->tx_len : acked;
        if (acked > s->tx_len && s->app_closed) s->fin_acked = 1;
        if (data_acked) {
            for (uint32_t i = data_acked; i < s->tx_len; ++i) s->tx[i - data_acked] = s->tx[i];
            s->tx_len -= data_acked;
        }
        s->snd_una = ack;
        if (mint_tcp_seq_lt(s->snd_nxt, ack)) s->snd_nxt = ack;
        s->retries = 0; s->rto = TCP_RTO_INITIAL;
        s->rto_deadline = s->snd_una == s->snd_nxt ? 0 : ticks() + s->rto;
    } else if (mint_tcp_seq_lt(s->snd_max, ack)) {
        tcp_ack_now(s);
        return;
    }
    if (mint_tcp_seq_le(s->snd_una, ack)) s->snd_wnd = window;
    if (s->fin_acked) {
        if (s->state == TS_FIN_WAIT_1) s->state = TS_FIN_WAIT_2;
        else if (s->state == TS_CLOSING) tcp_enter_time_wait(s);
        else if (s->state == TS_LAST_ACK) { tcp_free(s); return; }
    }

    /* Data and FIN, accepted only in order. */
    int need_ack = 0;
    uint8_t fin = (flags & TCP_FIN) != 0;
    if (dlen || fin) {
        if (mint_tcp_seq_lt(seq, s->rcv_nxt)) {
            uint32_t dup = s->rcv_nxt - seq;
            if (dup >= dlen + fin) { dlen = 0; fin = 0; need_ack = 1; }
            else if (dup <= dlen) { data += dup; dlen -= dup; seq = s->rcv_nxt; }
            else { dlen = 0; seq = s->rcv_nxt; }
        }
        if (seq != s->rcv_nxt) { need_ack = 1; dlen = 0; fin = 0; }
        else if (s->state == TS_ESTABLISHED || s->state == TS_FIN_WAIT_1 || s->state == TS_FIN_WAIT_2) {
            uint32_t n = dlen;
            if (!s->app_closed) {
                uint32_t space = TCP_RX_BUF - s->rx_len;
                if (n > space) n = space;
                for (uint32_t i = 0; i < n; ++i) s->rx[(s->rx_head + s->rx_len + i) % TCP_RX_BUF] = data[i];
                s->rx_len += n;
            }
            s->rcv_nxt += n;
            if (dlen || fin) need_ack = 1;
            if (fin && n == dlen) {
                s->rcv_nxt += 1u; s->peer_fin = 1;
                if (s->state == TS_ESTABLISHED) s->state = TS_CLOSE_WAIT;
                else if (s->state == TS_FIN_WAIT_1) { if (s->fin_acked) tcp_enter_time_wait(s); else s->state = TS_CLOSING; }
                else if (s->state == TS_FIN_WAIT_2) tcp_enter_time_wait(s);
            }
        } else need_ack = 1;                                   /* retransmitted FIN */
    }
    if (need_ack) tcp_ack_now(s);
    tcp_output(s);
}

static void tcp_timer(void) {
    uint64_t now = ticks();
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) {
        TCP_SOCK *s = &tcps[i];
        if (s->state == TS_FREE) continue;
        if (s->state == TS_TIME_WAIT && now >= s->linger_deadline) {
            if (s->app_closed) tcp_free(s); else { s->state = TS_CLOSED; }
            continue;
        }
        if (s->app_closed && s->linger_deadline && now >= s->linger_deadline && s->state != TS_TIME_WAIT) {
            (void)tcp_segment(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);   /* peer never finished closing */
            tcp_free(s);
            continue;
        }
        if (!s->rto_deadline || now < s->rto_deadline) continue;
        s->rto_deadline = 0;
        if (s->state == TS_SYN_SENT) {
            if (++s->retries > TCP_SYN_RETRIES) { tcp_fail(s, MINT_TCP_STATE_TIMEOUT); continue; }
            s->rto = s->rto * 2u > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2u;
            ++stats.tcp_retransmits;
            (void)tcp_segment(s, s->iss, TCP_SYN, 0, 0);
            tcp_arm(s);
            continue;
        }
        int outstanding = s->snd_nxt != s->snd_una;
        int zero_window = !s->snd_wnd && s->tx_len && !outstanding;
        if (!outstanding && !zero_window) continue;
        if (++s->retries > TCP_DATA_RETRIES) {
            (void)tcp_segment(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
            tcp_fail(s, MINT_TCP_STATE_TIMEOUT);
            continue;
        }
        s->rto = s->rto * 2u > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2u;
        if (outstanding) ++stats.tcp_retransmits;
        s->snd_nxt = s->snd_una;                                /* go back N */
        s->probe = (uint8_t)zero_window;
        tcp_output(s);
        s->probe = 0;
        tcp_arm(s);
    }
}

int64_t mint_net_tcp_connect(uint32_t ip, uint16_t port) {
    if (net.state != MINT_NET_STATE_UP) return -MINT_ENETDOWN;
    if (!ip || !port || ip == BROADCAST_IP || (ip >> 24) == 127u || (ip >> 28) == 0xEu) return -MINT_EINVAL;
    TCP_SOCK *s = 0; uint32_t index = 0;
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) if (tcps[i].state == TS_FREE) { s = &tcps[i]; index = i; break; }
    if (!s) return -MINT_EBUSY;
    uint8_t generation = (uint8_t)(s->generation + 1u);
    uint16_t lport = 0;
    for (uint32_t tries = 0; tries < 64u && !lport; ++tries) {
        if (net.next_port < 49152u) net.next_port = (uint16_t)(49152u + (random32() & 0x3FFFu));
        uint16_t candidate = net.next_port++;
        int used = 0;
        for (uint32_t i = 0; i < TCP_SLOTS; ++i) if (tcps[i].state != TS_FREE && tcps[i].lport == candidate) used = 1;
        if (!used) lport = candidate;
    }
    if (!lport) return -MINT_EBUSY;
    bytes_zero(s, (size_t)((uint8_t *)s->rx - (uint8_t *)s));
    s->generation = generation;
    s->lport = lport; s->rport = port; s->rip = ip; s->mss = 536u;
    s->iss = random32();
    s->snd_una = s->iss; s->snd_nxt = s->iss + 1u; s->snd_max = s->snd_nxt;
    s->rto = TCP_RTO_INITIAL; s->state = TS_SYN_SENT;
    (void)tcp_segment(s, s->iss, TCP_SYN, 0, 0);
    s->rto_deadline = ticks() + s->rto;
    return (int64_t)(((uint32_t)generation << 8) | index);
}

int64_t mint_net_tcp_send(int64_t handle, const uint8_t *data, uint32_t length) {
    TCP_SOCK *s = tcp_lookup(handle);
    if (!s) return -MINT_EBADF;
    if (s->state == TS_CLOSED) return s->error == MINT_TCP_STATE_RESET ? -MINT_ECONNRESET : -MINT_ENOTCONN;
    if (s->state != TS_SYN_SENT && s->state != TS_ESTABLISHED && s->state != TS_CLOSE_WAIT) return -MINT_EPIPE;
    uint32_t n = TCP_TX_BUF - s->tx_len;
    if (n > length) n = length;
    if (n && data) bytes_copy(s->tx + s->tx_len, data, n);
    s->tx_len += n;
    tcp_output(s);
    return n;
}

int64_t mint_net_tcp_recv(int64_t handle, uint8_t *out, uint32_t capacity) {
    TCP_SOCK *s = tcp_lookup(handle);
    if (!s) return -MINT_EBADF;
    if (s->rx_len) {
        uint32_t n = s->rx_len < capacity ? s->rx_len : capacity;
        for (uint32_t i = 0; i < n; ++i) out[i] = s->rx[(s->rx_head + i) % TCP_RX_BUF];
        s->rx_head = (s->rx_head + n) % TCP_RX_BUF; s->rx_len -= n;
        /* Window update once a closed or nearly closed window has room again. */
        if (s->adv_wnd < s->mss * 2u && tcp_window(s) >= s->mss * 2u &&
            (s->state == TS_ESTABLISHED || s->state == TS_FIN_WAIT_1 || s->state == TS_FIN_WAIT_2))
            tcp_ack_now(s);
        return n;
    }
    if (s->state == TS_CLOSED) {
        if (s->error == MINT_TCP_STATE_REFUSED) return -MINT_ECONNREFUSED;
        if (s->error == MINT_TCP_STATE_RESET) return -MINT_ECONNRESET;
        if (s->error == MINT_TCP_STATE_TIMEOUT) return -MINT_ETIMEDOUT;
        if (s->error == MINT_TCP_STATE_UNREACHABLE) return -MINT_EHOSTUNREACH;
        return 0;
    }
    if (s->peer_fin) return 0;
    return -MINT_EAGAIN;
}

int64_t mint_net_tcp_state(int64_t handle) {
    TCP_SOCK *s = tcp_lookup(handle);
    if (!s) return -MINT_EBADF;
    switch (s->state) {
    case TS_SYN_SENT: return MINT_TCP_STATE_CONNECTING;
    case TS_ESTABLISHED: case TS_CLOSE_WAIT: return MINT_TCP_STATE_ESTABLISHED;
    case TS_CLOSED: return s->error ? s->error : MINT_TCP_STATE_CLOSED;
    default: return MINT_TCP_STATE_CLOSING;
    }
}

int64_t mint_net_tcp_close(int64_t handle) {
    TCP_SOCK *s = tcp_lookup(handle);
    if (!s) return -MINT_EBADF;
    s->app_closed = 1; s->rx_len = 0;
    s->linger_deadline = ticks() + 30u * HZ;
    if (s->state == TS_SYN_SENT || s->state == TS_CLOSED) { tcp_free(s); return 0; }
    tcp_output(s);
    return 0;
}

/* ------------------------------------------------------------------ input */
static void ip_input(const uint8_t *h, uint32_t length) {
    if (length < IP_HDR || (h[0] >> 4) != 4u) return;
    uint32_t hl = (uint32_t)(h[0] & 0x0Fu) * 4u, total = mint_net_get16(h + 2);
    if (hl < IP_HDR || total < hl || total > length) return;
    if (mint_net_checksum_fold(mint_net_checksum_add(0, h, hl)) != 0) return;
    if (mint_net_get16(h + 6) & 0x3FFFu) return;               /* fragments are not reassembled */
    uint32_t src = mint_net_get32(h + 12), dst = mint_net_get32(h + 16);
    uint8_t protocol = h[9];
    const uint8_t *payload = h + hl;
    uint32_t plen = total - hl;
    if (!(dst == net.ip || is_broadcast(dst) || (!net.ip && protocol == PROTO_UDP) ||
          (net.dhcp_phase == DHCP_REQUESTING && dst == net.offered))) return;
    if (protocol == PROTO_ICMP) icmp_input(src, h[8], payload, plen);
    else if (protocol == PROTO_UDP) {
        if (plen < 8u) return;
        uint32_t ulen = mint_net_get16(payload + 4);
        if (ulen < 8u || ulen > plen) return;
        if (mint_net_get16(payload + 6) && mint_net_transport_checksum(src, dst, PROTO_UDP, payload, ulen) != 0) return;
        ++stats.udp_received;
        uint16_t sport = mint_net_get16(payload), dport = mint_net_get16(payload + 2);
        if (dport == 68u && sport == 67u) dhcp_input(payload + 8, ulen - 8u, src);
        else if (dport == DNS_LOCAL_PORT && sport == 53u && src == net.dns) dns_input(payload + 8, ulen - 8u);
    } else if (protocol == PROTO_TCP && net.ip && dst == net.ip) tcp_input(src, dst, payload, plen);
}

static void eth_input(const uint8_t *f, uint32_t length) {
    if (length < ETH_HDR) return;
    ++stats.rx_frames; stats.rx_bytes += length;
    if (!mac_equal(f, net.mac) && !mac_equal(f, broadcast_mac)) return;
    uint16_t type = mint_net_get16(f + 12);
    if (type == ETH_ARP) arp_input(f + ETH_HDR, length - ETH_HDR);
    else if (type == ETH_IPV4) ip_input(f + ETH_HDR, length - ETH_HDR);
}

/* ------------------------------------------------------------------ control */
static void reset_sessions(void) {
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) {
        TCP_SOCK *s = &tcps[i];
        if (s->state != TS_FREE && s->state != TS_CLOSED && s->state != TS_SYN_SENT && net.ip)
            (void)tcp_segment(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
        if (s->state != TS_FREE) { if (s->app_closed) tcp_free(s); else { s->state = TS_CLOSED; s->error = MINT_TCP_STATE_RESET; } }
    }
    for (uint32_t i = 0; i < DNS_SLOTS; ++i) dns_cache[i].state = DNS_FREE;
    for (uint32_t i = 0; i < ARP_SLOTS; ++i) arp_cache[i].valid = 0;
    arp_pending.used = 0;
}

int64_t mint_net_up(uint32_t mode) {
    if (mode == MINT_NET_MODE_DOWN) {
        reset_sessions();
        net.wanted = 0; net.ip = 0; net.mask = 0; net.gw = 0; net.dns = 0; net.server = 0;
        net.dhcp_phase = DHCP_IDLE;
        net.state = MINT_NET_STATE_DOWN;
        set_status(net.nic_started ? "Disconnected" : "Not started");
        mint_serial_write("[net] network disconnected by request\n");
        return 0;
    }
    if (mode != MINT_NET_MODE_DHCP) return -MINT_EINVAL;
    if (!mint_e1000_active() && !mint_e1000_init()) {
        MINT_E1000_INFO nic;
        (void)mint_e1000_get_info(&nic);
        net.state = nic.state == MINT_E1000_NOT_FOUND ? MINT_NET_STATE_DOWN : MINT_NET_STATE_FAILED;
        set_status(nic.state == MINT_E1000_NOT_FOUND ? "No supported network adapter" : "Adapter failed to start");
        return nic.state == MINT_E1000_NOT_FOUND ? -MINT_ENODEV : -MINT_EIO;
    }
    if (!net.nic_started) {
        (void)mint_e1000_mac(net.mac);
        net.nic_started = 1;
        net.next_port = (uint16_t)(49152u + (random32() & 0x3FFFu));
    }
    net.wanted = 1;
    if (net.state == MINT_NET_STATE_UP || net.state == MINT_NET_STATE_DHCP) return 0;
    net.link = (uint8_t)mint_e1000_link_up();
    if (net.link) dhcp_start();
    else { net.state = MINT_NET_STATE_NO_LINK; set_status("Cable unplugged or no link"); }
    mint_serial_write("[net] network start requested; ");
    mint_serial_write(net.link ? "link up, starting DHCP\n" : "waiting for link\n");
    return 0;
}

int mint_net_get_info(MINT_NET_INFO *out) {
    if (!out) return 0;
    MINT_E1000_INFO nic;
    (void)mint_e1000_get_info(&nic);
    *out = stats;
    out->version = MINT_NET_INFO_VERSION;
    out->state = net.state;
    out->link_up = net.nic_started ? net.link : 0u;
    out->driver_active = nic.state == MINT_E1000_ACTIVE;
    for (uint32_t i = 0; i < 6u; ++i) out->mac[i] = nic.mac[i];
    out->nic_device_id = nic.device_id;
    out->adapter_flags = nic.experimental ? MINT_NET_ADAPTER_EXPERIMENTAL : 0u;
    out->ip = net.ip; out->netmask = net.mask; out->gateway = net.gw; out->dns = net.dns; out->dhcp_server = net.server;
    out->lease_seconds = net.state == MINT_NET_STATE_UP ? net.lease : 0u;
    out->lease_elapsed_seconds = net.state == MINT_NET_STATE_UP ? (uint32_t)((ticks() - net.lease_tick) / HZ) : 0u;
    uint32_t open = 0;
    for (uint32_t i = 0; i < TCP_SLOTS; ++i) if (tcps[i].state != TS_FREE) ++open;
    out->tcp_open_sockets = open;
    for (uint32_t i = 0; i < sizeof(out->adapter); ++i) out->adapter[i] = i < sizeof(nic.name) ? nic.name[i] : 0;
    out->adapter[sizeof(out->adapter) - 1u] = '\0';
    if (nic.state == MINT_E1000_NOT_FOUND && !nic.name[0]) {
        static const char none[] = "No supported adapter";
        for (uint32_t i = 0; i < sizeof(none); ++i) out->adapter[i] = none[i];
    }
    for (uint32_t i = 0; i < sizeof(out->status); ++i) out->status[i] = net.status[i];
    if (!out->status[0]) {
        const char *text = nic.state == MINT_E1000_FOUND ? "Not connected" :
                           nic.state == MINT_E1000_NOT_FOUND ? "No supported adapter" : "Not connected";
        for (uint32_t i = 0; text[i] && i + 1u < sizeof(out->status); ++i) out->status[i] = text[i];
    }
    return 1;
}

/* ------------------------------------------------------------------ task */
void mint_net_task(void *argument) {
    (void)argument;
    cal_tick0 = ticks(); cal_tsc0 = rdtsc();
    uint64_t last_timer = 0;
    for (;;) {
        uint64_t now = ticks();
        if (!tsc_per_us && now - cal_tick0 >= 50u) {
            uint64_t us = (now - cal_tick0) * 10000u, delta = rdtsc() - cal_tsc0;
            tsc_per_us = delta / us;
            if (!tsc_per_us) tsc_per_us = 0;   /* keep the tick fallback on a stalled TSC */
        }
        if (net.nic_started && mint_e1000_active()) {
            for (uint32_t i = 0; i < 64u; ++i) {
                uint32_t length = 0;
                int rc = mint_e1000_poll(rx_frame, sizeof(rx_frame), &length);
                if (rc == 0) break;
                if (rc > 0) { if (net.wanted) eth_input(rx_frame, length); }
                else ++stats.rx_dropped;
            }
            if (now != last_timer) {
                last_timer = now;
                uint8_t link = (uint8_t)mint_e1000_link_up();
                if (link != net.link) {
                    net.link = link;
                    mint_serial_write(link ? "[net] link up\n" : "[net] link down\n");
                    if (!link) net.link_lost_tick = now;
                    else if (net.wanted && net.state == MINT_NET_STATE_NO_LINK) dhcp_start();
                }
                if (net.wanted && !net.link && net.state != MINT_NET_STATE_NO_LINK && now - net.link_lost_tick > 3u * HZ) {
                    reset_sessions();
                    net.ip = 0; net.dhcp_phase = DHCP_IDLE; net.state = MINT_NET_STATE_NO_LINK;
                    set_status("Cable unplugged or no link");
                }
                if (net.wanted) {
                    dhcp_timer();
                    dns_timer();
                    tcp_timer();
                    if (arp_pending.used && now - arp_pending.tick > 3u * HZ) arp_pending.used = 0;
                    else if (arp_pending.used && now - arp_last_request_tick >= HZ) arp_request(arp_pending.next_hop);
                }
            }
        }
        mint_task_sleep_ticks(1u);
    }
}
