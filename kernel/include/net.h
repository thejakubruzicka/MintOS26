#ifndef MINTOS26_NET_H
#define MINTOS26_NET_H
#include <stdint.h>
#include <stddef.h>

/* MintOS26 IPv4 network stack: ARP, IPv4 (no fragmentation), ICMP echo, UDP,
 * a DHCP client, a DNS (A record) resolver and a client-side TCP. It runs in
 * the kernel "net" task, which polls the NIC once per timer tick. IPv4
 * addresses are carried in host order: 10.0.2.15 == 0x0A00020F. */

#define MINT_NET_INFO_VERSION 1u

enum mint_net_state {
    MINT_NET_STATE_DOWN = 0,      /* no adapter started */
    MINT_NET_STATE_NO_LINK = 1,   /* adapter running, no carrier */
    MINT_NET_STATE_DHCP = 2,      /* asking a DHCP server for an address */
    MINT_NET_STATE_UP = 3,        /* configured */
    MINT_NET_STATE_FAILED = 4     /* adapter failed or DHCP gave up */
};

typedef struct mint_net_info {
    uint32_t version;
    uint32_t state;
    uint32_t link_up;
    uint32_t driver_active;
    uint8_t mac[6];
    uint16_t nic_device_id;          /* PCI device id (vendor is Intel 0x8086) */
    uint32_t ip, netmask, gateway, dns, dhcp_server;
    uint32_t lease_seconds;
    uint32_t lease_elapsed_seconds;
    uint64_t rx_frames, tx_frames, rx_bytes, tx_bytes, rx_dropped;
    uint64_t arp_replies, icmp_echo_answered, udp_received, tcp_segments_in, tcp_segments_out;
    uint64_t tcp_retransmits, dns_queries;
    uint32_t tcp_open_sockets;
    uint32_t adapter_flags;          /* MINT_NET_ADAPTER_* */
    char adapter[48];
    char status[64];
} MINT_NET_INFO;

enum { MINT_NET_ADAPTER_EXPERIMENTAL = 1u };

/* MINT_SYS_NET_UP modes */
enum { MINT_NET_MODE_DOWN = 0, MINT_NET_MODE_DHCP = 1 };

/* MINT_SYS_NET_PING ops: SEND(ip, seq) -> 0; POLL(seq) -> (ttl << 32) | rtt_us,
 * or -EAGAIN while no reply has arrived. */
enum { MINT_NET_PING_SEND = 0, MINT_NET_PING_POLL = 1 };

/* MINT_SYS_TCP ops (arg0 = op):
 *   CONNECT(ip, port)          -> handle >= 0
 *   SEND(handle, buf, len)     -> bytes queued (0 when the send buffer is full)
 *   RECV(handle, buf, cap)     -> bytes; 0 = peer closed; -EAGAIN = nothing yet
 *   STATE(handle)              -> MINT_TCP_STATE_*
 *   CLOSE(handle)              -> 0 (graceful FIN; the handle is invalid afterwards) */
enum { MINT_TCP_OP_CONNECT = 0, MINT_TCP_OP_SEND = 1, MINT_TCP_OP_RECV = 2,
       MINT_TCP_OP_STATE = 3, MINT_TCP_OP_CLOSE = 4 };
enum { MINT_TCP_STATE_CONNECTING = 1, MINT_TCP_STATE_ESTABLISHED = 2, MINT_TCP_STATE_CLOSING = 3,
       MINT_TCP_STATE_CLOSED = 4, MINT_TCP_STATE_REFUSED = 5, MINT_TCP_STATE_RESET = 6,
       MINT_TCP_STATE_TIMEOUT = 7, MINT_TCP_STATE_UNREACHABLE = 8 };

/* ------------------------------------------------------------------ pure helpers
 * Packet builders and parsers with no kernel dependencies (host-tested). */
uint32_t mint_net_checksum_add(uint32_t sum, const void *data, size_t length);
uint16_t mint_net_checksum_fold(uint32_t sum);
uint16_t mint_net_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                     const uint8_t *segment, size_t length);
int mint_ipv4_parse(const char *text, uint32_t *ip_out);
size_t mint_ipv4_format(char *out, size_t capacity, uint32_t ip);
static inline uint16_t mint_net_get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t mint_net_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void mint_net_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void mint_net_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline int mint_tcp_seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int mint_tcp_seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

enum { MINT_DHCP_DISCOVER = 1, MINT_DHCP_OFFER = 2, MINT_DHCP_REQUEST = 3, MINT_DHCP_ACK = 5, MINT_DHCP_NAK = 6 };
typedef struct mint_dhcp_reply {
    uint8_t type;
    uint32_t your_ip, server_id, netmask, router, dns, lease_seconds;
} MINT_DHCP_REPLY;
/* Builds a BOOTP/DHCP client message (UDP payload, >= 300 bytes). */
size_t mint_dhcp_build(uint8_t *out, size_t capacity, uint8_t type, uint32_t xid, const uint8_t mac[6],
                       uint32_t client_ip, uint32_t requested_ip, uint32_t server_id, const char *hostname);
/* 1 = valid reply for this xid/MAC (fields filled), 0 = not ours or malformed. */
int mint_dhcp_parse(const uint8_t *packet, size_t length, uint32_t xid, const uint8_t mac[6], MINT_DHCP_REPLY *out);

/* DNS: recursive A query. parse returns 1 with the first A record, 0 if the
 * answer has no A record, -1 for NXDOMAIN/server failure, -2 if malformed. */
size_t mint_dns_build_query(uint8_t *out, size_t capacity, uint16_t id, const char *name);
int mint_dns_parse_a(const uint8_t *packet, size_t length, uint16_t id, uint32_t *ip_out, uint32_t *ttl_out);

/* TCP options: returns the MSS option value or 0 if absent/malformed. */
uint16_t mint_tcp_parse_mss(const uint8_t *options, size_t length);

#ifndef MINT_NET_USERSPACE
/* ------------------------------------------------------------------ kernel API */
void mint_net_task(void *argument);
int mint_net_get_info(MINT_NET_INFO *out);
int64_t mint_net_up(uint32_t mode);
int64_t mint_net_resolve(const char *name, uint32_t *ip_out);
int64_t mint_net_ping(uint32_t op, uint32_t ip, uint32_t seq);
int64_t mint_net_tcp_connect(uint32_t ip, uint16_t port);
int64_t mint_net_tcp_send(int64_t handle, const uint8_t *data, uint32_t length);
int64_t mint_net_tcp_recv(int64_t handle, uint8_t *out, uint32_t capacity);
int64_t mint_net_tcp_state(int64_t handle);
int64_t mint_net_tcp_close(int64_t handle);
#endif
#endif
