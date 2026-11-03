package kernel;
import mem;
import types;

typedef types.uint8_t  u8;
typedef types.uint16_t u16;
typedef types.uint32_t u32;
typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);
extern void virtio_net_init(void);
extern u32 virtio_net_present(void);
extern void virtio_net_mac(u8 *out);
extern u32 virtio_net_tx(const void *frame, u32 len);
extern u32 virtio_net_rx(void *frame, u32 maxlen);
extern void sched_yield(void);
extern void sched_sleep(u64 chan);
extern void sched_wakeup(u64 chan);
extern u64 sched_tick_count(void);
extern u64 sched_current_as_h(void);
extern u32 sched_fd_alloc(void);
extern void sched_fd_set(u32 idx, u32 fh);
extern u32 sched_fd_get(u32 idx);

/* ---------------------------------------------------------------------------
 * Minimal L2/L3 + BSD-ish UDP/TCP sockets.
 *
 * Addressing: DHCP DISCOVER/REQUEST against QEMU user-net (fallback to
 *   static 10.0.2.15/24, gw 10.0.2.2, dns 10.0.2.2 if lease fails).
 *   Lease option 51 arms T1/T2 renew/rebind timers on the 100Hz tick.
 * ICMP echo (sys_ping); UDP socket/bind/connect/sendto/recvfrom;
 * TCP 3-way handshake, listen/accept, and byte-stream send/recv
 * (loopback fully exercised; SYN/data retransmit via 100Hz timer).
 * ------------------------------------------------------------------------- */

enum {
    ETH_TYPE_IP   = 0x0800,
    ETH_TYPE_ARP  = 0x0806,
    ARP_OP_REQ    = 1,
    ARP_OP_REP    = 2,
    IP_PROTO_ICMP = 1,
    IP_PROTO_TCP  = 6,
    IP_PROTO_UDP  = 17,
    ICMP_ECHO_REP = 0,
    ICMP_ECHO_REQ = 8,
    ETH_HDR       = 14,
    ARP_LEN       = 28,
    IP_HDR        = 20,
    ICMP_HDR      = 8,
    UDP_HDR       = 8,
    FRAME_MAX     = 1514,
    ARP_CACHE     = 8,
    PING_DATA_LEN = 32,

    AF_INET       = 2,
    SOCK_STREAM   = 1,
    SOCK_DGRAM    = 2,
    IPPROTO_IP    = 0,
    IPPROTO_TCP   = 6,
    IPPROTO_UDP   = 17,

    FH_SOCK_BASE  = 0x20000000u,
    FD_BAD        = 0xFFFFFFFF,
    MAX_SOCKS     = 16,
    SOCK_RXQ      = 8,
    UDP_MAX_PAYLOAD = 1400,
    TCP_HDR       = 20,
    TCP_MAX_PAYLOAD = 1360,
    LOOPBACK_IP   = 0x7F000001u, /* 127.0.0.1 */
    TCP_RX_CAP    = 2048,
    TCP_BACKLOG   = 4,
    SOCKADDR_IN_LEN = 16,
    EPHEMERAL_BASE = 49152,
    EPHEMERAL_MAX  = 65535,

    /* TCP states (subset) */
    TCPS_CLOSED      = 0,
    TCPS_LISTEN      = 1,
    TCPS_SYN_SENT    = 2,
    TCPS_SYN_RCVD    = 3,
    TCPS_ESTABLISHED = 4,
    TCPS_CLOSE_WAIT  = 5,
    TCPS_LAST_ACK    = 6,
    TCPS_FIN_WAIT    = 7,

    TCPF_FIN = 0x01,
    TCPF_SYN = 0x02,
    TCPF_RST = 0x04,
    TCPF_PSH = 0x08,
    TCPF_ACK = 0x10,

    SHUT_RD = 0,
    SHUT_WR = 1,
    SHUT_RDWR = 2
};

struct arp_entry {
    u32 ip;
    u8  mac[6];
    u32 used;
};

struct udp_pkt {
    u32 src_ip;
    u16 src_port;
    u16 len;
    u8  data[UDP_MAX_PAYLOAD];
};

struct sock {
    u32 used;
    u32 refs;
    u32 type;          /* SOCK_DGRAM / SOCK_STREAM */
    u32 bound;
    u32 connected;
    u16 local_port;
    u32 local_ip;      /* host order; 0 = INADDR_ANY */
    u32 peer_ip;       /* host order */
    u16 peer_port;
    u32 rx_head;
    u32 rx_tail;
    u32 rx_count;
    struct udp_pkt rxq[SOCK_RXQ];
    /* TCP */
    u32 tcp_state;
    u32 snd_una;       /* first unacked seq (= next seq for our minimal stack) */
    u32 snd_nxt;
    u32 rcv_nxt;
    u32 iss;
    u32 irs;
    u32 listen_parent; /* slot of LISTEN parent; 0 = none */
    u32 accept_head;
    u32 accept_tail;
    u32 accept_count;
    u32 accept_q[TCP_BACKLOG];
    u32 s_head;
    u32 s_tail;
    u32 s_count;
    u8  sbuf[TCP_RX_CAP];
    u32 peer_fin;      /* saw FIN; recv returns 0 after drain */
    u32 shut_rd;       /* shutdown(SHUT_RD): discard further RX */
    u32 shut_wr;       /* shutdown(SHUT_WR): no more TX / send FIN */
    u32 reuseaddr;     /* SO_REUSEADDR hint (bind helper) */
    /* Retransmit of last unacked segment (SYN or data). */
    u32 rexmit_due;    /* sched_ticks deadline; 0 = idle */
    u32 rexmit_seq;
    u8  rexmit_flags;
    u16 rexmit_len;
    u8  rexmit_buf[TCP_MAX_PAYLOAD];
};

static u8  local_mac[6];
static u32 local_ip;                 /* host order */
static u32 netmask;
static u32 gateway_ip;
static u32 dns_ip;                   /* host order; from DHCP or default */
static u32 dhcp_pending;             /* accept DHCP replies (init/renew) */
static u32 dhcp_xid;
static u32 dhcp_offer_ip;
static u32 dhcp_server_ip;
static u32 dhcp_got_offer;
static u32 dhcp_got_ack;
static u32 dhcp_offer_mask;
static u32 dhcp_offer_gw;
static u32 dhcp_offer_dns;
static u32 dhcp_offer_lease;         /* option 51 seconds; 0 = default */
static u32 dhcp_state;               /* 0 idle/static, 1 bound, 2 renew, 3 rebind */
static u32 dhcp_t1_tick;             /* sched_ticks deadline for T1 */
static u32 dhcp_t2_tick;
static u32 dhcp_exp_tick;            /* lease expiry */
static u32 dhcp_next_xmit_tick;      /* renew/rebind retransmit pacing */
static u32 net_up;
static struct arp_entry arp_tab[ARP_CACHE];
static u8  rx_frame[FRAME_MAX];
static u8  tx_frame[FRAME_MAX];
static u16 ping_id;
static u16 ping_seq;
static u32 ping_waiting;
static u32 ping_got_reply;
static u32 ping_target;
static struct sock socks[MAX_SOCKS];
static u16 next_eph = EPHEMERAL_BASE;

/* True if dst is our QEMU lease or IPv4 loopback. */
static u32 is_local_dst(u32 ip) {
    return ip == local_ip || ip == LOOPBACK_IP;
}

static void handle_tcp(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 len);
static u32 resolve_mac(u32 ip, u8 *mac_out);
static u32 tcp_tx_seg(u16 sport, u32 dst_ip, u16 dport,
                      u32 seq, u32 ack, u8 flags,
                      const u8 *payload, u16 plen);
static u32 tcp_tx_seg_ex(u16 sport, u32 dst_ip, u16 dport,
                         u32 seq, u32 ack, u8 flags,
                         const u8 *payload, u16 plen, u32 allow_arp);


static u16 checksum(const u8 *data, u32 len) {
    u32 sum = 0;
    u32 i = 0;
    while (i + 1 < len) {
        sum += ((u32)data[i] << 8) | (u32)data[i + 1];
        i += 2;
    }
    if (i < len) {
        sum += ((u32)data[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)(~sum);
}

static void mac_copy(u8 *dst, const u8 *src) {
    u32 i;
    for (i = 0; i < 6; i++) {
        dst[i] = src[i];
    }
}

static void mac_set_bcast(u8 *dst) {
    u32 i;
    for (i = 0; i < 6; i++) {
        dst[i] = 0xFF;
    }
}

static void put16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}
static void put32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)(v & 0xFF);
}
static u16 get16(const u8 *p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}
static u32 get32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void mem_zero(void *dst, u32 n) {
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void arp_learn(u32 ip, const u8 *mac) {
    u32 i;
    for (i = 0; i < ARP_CACHE; i++) {
        if (arp_tab[i].used && arp_tab[i].ip == ip) {
            mac_copy(arp_tab[i].mac, mac);
            return;
        }
    }
    for (i = 0; i < ARP_CACHE; i++) {
        if (!arp_tab[i].used) {
            arp_tab[i].used = 1;
            arp_tab[i].ip = ip;
            mac_copy(arp_tab[i].mac, mac);
            return;
        }
    }
    arp_tab[0].used = 1;
    arp_tab[0].ip = ip;
    mac_copy(arp_tab[0].mac, mac);
}

static u32 arp_lookup(u32 ip, u8 *mac_out) {
    u32 i;
    for (i = 0; i < ARP_CACHE; i++) {
        if (arp_tab[i].used && arp_tab[i].ip == ip) {
            mac_copy(mac_out, arp_tab[i].mac);
            return 1;
        }
    }
    return 0;
}

static u32 eth_send(const u8 *dst_mac, u16 ethertype, const u8 *payload, u32 plen) {
    u32 i;
    if (ETH_HDR + plen > FRAME_MAX) {
        return 0;
    }
    mac_copy(tx_frame + 0, dst_mac);
    mac_copy(tx_frame + 6, local_mac);
    put16(tx_frame + 12, ethertype);
    for (i = 0; i < plen; i++) {
        tx_frame[ETH_HDR + i] = payload[i];
    }
    return virtio_net_tx(tx_frame, ETH_HDR + plen);
}

static u32 arp_send(u16 op, u32 tip, const u8 *tmac) {
    u8 pkt[ARP_LEN];
    u8 bcast[6];
    u32 i;
    for (i = 0; i < ARP_LEN; i++) {
        pkt[i] = 0;
    }
    put16(pkt + 0, 1);
    put16(pkt + 2, ETH_TYPE_IP);
    pkt[4] = 6;
    pkt[5] = 4;
    put16(pkt + 6, op);
    mac_copy(pkt + 8, local_mac);
    put32(pkt + 14, local_ip);
    if (tmac) {
        mac_copy(pkt + 18, tmac);
    }
    put32(pkt + 24, tip);
    if (op == ARP_OP_REQ) {
        mac_set_bcast(bcast);
        return eth_send(bcast, ETH_TYPE_ARP, pkt, ARP_LEN);
    }
    return eth_send(tmac, ETH_TYPE_ARP, pkt, ARP_LEN);
}

static void handle_arp(const u8 *pkt, u32 len) {
    u16 op;
    u32 sip;
    u32 tip;
    u8 smac[6];
    if (len < ARP_LEN) {
        return;
    }
    if (get16(pkt + 0) != 1 || get16(pkt + 2) != ETH_TYPE_IP) {
        return;
    }
    if (pkt[4] != 6 || pkt[5] != 4) {
        return;
    }
    op = get16(pkt + 6);
    mac_copy(smac, pkt + 8);
    sip = get32(pkt + 14);
    tip = get32(pkt + 24);
    arp_learn(sip, smac);
    if (op == ARP_OP_REQ && tip == local_ip) {
        arp_send(ARP_OP_REP, sip, smac);
    }
}

static u32 ip_send(u32 dst_ip, u8 proto, const u8 *payload, u32 plen, const u8 *dst_mac) {
    u8 ippkt[FRAME_MAX];
    u32 total = IP_HDR + plen;
    u32 i;
    u16 csum;
    if (total > FRAME_MAX - ETH_HDR) {
        return 0;
    }
    for (i = 0; i < total; i++) {
        ippkt[i] = 0;
    }
    ippkt[0] = 0x45;
    put16(ippkt + 2, (u16)total);
    put16(ippkt + 4, ping_id);
    ippkt[8] = 64;
    ippkt[9] = proto;
    put32(ippkt + 12, local_ip);
    put32(ippkt + 16, dst_ip);
    csum = checksum(ippkt, IP_HDR);
    put16(ippkt + 10, csum);
    for (i = 0; i < plen; i++) {
        ippkt[IP_HDR + i] = payload[i];
    }
    return eth_send(dst_mac, ETH_TYPE_IP, ippkt, total);
}

static void handle_icmp(u32 src_ip, const u8 *icmp, u32 len, const u8 *src_mac) {
    u8 reply[FRAME_MAX];
    u32 i;
    u16 csum;
    if (len < ICMP_HDR) {
        return;
    }
    if (icmp[0] == ICMP_ECHO_REQ) {
        for (i = 0; i < len; i++) {
            reply[i] = icmp[i];
        }
        reply[0] = ICMP_ECHO_REP;
        put16(reply + 2, 0);
        csum = checksum(reply, len);
        put16(reply + 2, csum);
        ip_send(src_ip, IP_PROTO_ICMP, reply, len, src_mac);
        return;
    }
    if (icmp[0] == ICMP_ECHO_REP && ping_waiting) {
        u16 id = get16(icmp + 4);
        u16 seq = get16(icmp + 6);
        if (id == ping_id && seq == ping_seq && src_ip == ping_target) {
            ping_got_reply = 1;
            ping_waiting = 0;
        }
    }
}

/* ---------------- sockets (UDP) ---------------- */

u32 sock_fh_is(u32 fh) {
    return (fh & FH_SOCK_BASE) != 0 && (fh & 0xC0000000u) == 0;
}

static u32 sock_slot(u32 fh) {
    return fh & ~FH_SOCK_BASE;
}

static struct sock *sock_from_fh(u32 fh) {
    u32 s;
    if (!sock_fh_is(fh)) {
        return 0;
    }
    s = sock_slot(fh);
    if (s >= MAX_SOCKS || !socks[s].used) {
        return 0;
    }
    return &socks[s];
}

void sock_retain(u32 fh) {
    struct sock *sk = sock_from_fh(fh);
    if (sk) {
        sk->refs++;
    }
}

u32 sock_close(u32 fh) {
    struct sock *sk = sock_from_fh(fh);
    if (!sk) {
        return 0;
    }
    if (sk->refs > 1) {
        sk->refs--;
        return 1;
    }
    if (sk->type == SOCK_STREAM) {
        if (sk->tcp_state == TCPS_ESTABLISHED) {
            tcp_tx_seg(sk->local_port, sk->peer_ip, sk->peer_port,
                       sk->snd_nxt, sk->rcv_nxt,
                       (u8)(TCPF_FIN | TCPF_ACK), 0, 0);
            sk->snd_nxt++;
            sk->tcp_state = TCPS_FIN_WAIT;
        } else if (sk->tcp_state == TCPS_CLOSE_WAIT) {
            tcp_tx_seg(sk->local_port, sk->peer_ip, sk->peer_port,
                       sk->snd_nxt, sk->rcv_nxt,
                       (u8)(TCPF_FIN | TCPF_ACK), 0, 0);
            sk->snd_nxt++;
            sk->tcp_state = TCPS_LAST_ACK;
        }
    }
    sk->used = 0;
    sk->refs = 0;
    sk->rx_count = 0;
    sk->rx_head = 0;
    sk->rx_tail = 0;
    sk->s_count = 0;
    sk->tcp_state = TCPS_CLOSED;
    sched_wakeup((u64)sk);
    return 1;
}

static u32 sock_alloc_slot(void) {
    u32 i;
    for (i = 1; i < MAX_SOCKS; i++) {
        if (!socks[i].used) {
            mem_zero(&socks[i], (u32)sizeof(struct sock));
            socks[i].used = 1;
            socks[i].rexmit_due = 0;
            socks[i].rexmit_len = 0;
            socks[i].refs = 1;
            return i;
        }
    }
    return 0;
}

static u16 alloc_ephemeral(void) {
    u32 tries;
    for (tries = 0; tries < 16384; tries++) {
        u16 p = next_eph;
        u32 i;
        u32 taken = 0;
        if (next_eph >= EPHEMERAL_MAX) {
            next_eph = EPHEMERAL_BASE;
        } else {
            next_eph = (u16)(next_eph + 1);
        }
        for (i = 1; i < MAX_SOCKS; i++) {
            if (socks[i].used && socks[i].bound && socks[i].local_port == p) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            return p;
        }
    }
    return 0;
}

static u32 port_in_use(u16 port, u32 except_slot) {
    u32 i;
    u32 self_reuse = 0;
    if (except_slot != 0 && except_slot < MAX_SOCKS) {
        self_reuse = socks[except_slot].reuseaddr;
    }
    for (i = 1; i < MAX_SOCKS; i++) {
        if (i == except_slot) {
            continue;
        }
        if (socks[i].used && socks[i].bound && socks[i].local_port == port) {
            /* Both ends with SO_REUSEADDR may share a port (UDP servers). */
            if (self_reuse && socks[i].reuseaddr) {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

static void sock_enqueue(struct sock *sk, u32 src_ip, u16 src_port,
                         const u8 *payload, u16 plen) {
    struct udp_pkt *pkt;
    u32 i;
    if (plen > UDP_MAX_PAYLOAD) {
        plen = UDP_MAX_PAYLOAD;
    }
    if (sk->rx_count >= SOCK_RXQ) {
        /* Drop oldest. */
        sk->rx_head++;
        if (sk->rx_head >= SOCK_RXQ) {
            sk->rx_head = 0;
        }
        sk->rx_count--;
    }
    pkt = &sk->rxq[sk->rx_tail];
    pkt->src_ip = src_ip;
    pkt->src_port = src_port;
    pkt->len = plen;
    for (i = 0; i < plen; i++) {
        pkt->data[i] = payload[i];
    }
    sk->rx_tail++;
    if (sk->rx_tail >= SOCK_RXQ) {
        sk->rx_tail = 0;
    }
    sk->rx_count++;
    sched_wakeup((u64)sk);
}

/* Parse DHCP options; fill offer_* globals. msgtype 2=OFFER, 5=ACK. */
static void dhcp_parse_options(const u8 *opt, u32 len, u8 *msgtype_out) {
    u32 i = 0;
    *msgtype_out = 0;
    while (i < len) {
        u8 tag = opt[i];
        u8 l;
        if (tag == 0xFF) break;
        if (tag == 0) { i++; continue; }
        if (i + 1 >= len) break;
        l = opt[i + 1];
        if (i + 2 + l > len) break;
        if (tag == 53 && l >= 1) {
            *msgtype_out = opt[i + 2];
        } else if (tag == 1 && l >= 4) {
            dhcp_offer_mask = get32(opt + i + 2);
        } else if (tag == 3 && l >= 4) {
            dhcp_offer_gw = get32(opt + i + 2);
        } else if (tag == 6 && l >= 4) {
            dhcp_offer_dns = get32(opt + i + 2);
        } else if (tag == 54 && l >= 4) {
            dhcp_server_ip = get32(opt + i + 2);
        } else if (tag == 51 && l >= 4) {
            dhcp_offer_lease = get32(opt + i + 2);
        }
        i += 2 + l;
    }
}

static void dhcp_apply_static_fallback(void);
static void dhcp_arm_lease_timers(u32 lease_secs) {
    u32 now = (u32)sched_tick_count();
    u32 secs = lease_secs;
    if (secs < 60) secs = 60;                  /* floor for timer math */
    if (secs > 86400u * 7u) secs = 86400u * 7u;
    dhcp_offer_lease = secs;
    /* 100 Hz ticks. */
    dhcp_t1_tick = now + (secs / 2u) * 100u;
    dhcp_t2_tick = now + ((secs * 7u) / 8u) * 100u;
    dhcp_exp_tick = now + secs * 100u;
    dhcp_next_xmit_tick = 0;
    dhcp_state = 1;                            /* BOUND */
    kprintf("[NET] DHCP timers T1=%us T2=%us expiry=%us\n",
            (u64)(secs / 2u), (u64)((secs * 7u) / 8u), (u64)secs);
}

static void dhcp_apply_ack(void) {
    local_ip = dhcp_offer_ip;
    netmask = dhcp_offer_mask ? dhcp_offer_mask : 0xFFFFFF00u;
    gateway_ip = dhcp_offer_gw ? dhcp_offer_gw : 0x0A000202u;
    dns_ip = dhcp_offer_dns ? dhcp_offer_dns : gateway_ip;
    dhcp_arm_lease_timers(dhcp_offer_lease ? dhcp_offer_lease : 3600u);
}

static void dhcp_handle_bootp(u32 src_ip, const u8 *bootp, u16 plen) {
    u32 xid;
    u32 yiaddr;
    u8 msgtype;
    (void)src_ip;
    if (!dhcp_pending || plen < 240) return;
    if (bootp[0] != 2) return;                 /* BOOTREPLY */
    xid = get32(bootp + 4);
    if (xid != dhcp_xid) return;
    /* magic cookie */
    if (bootp[236] != 0x63 || bootp[237] != 0x82 ||
        bootp[238] != 0x53 || bootp[239] != 0x63) return;
    yiaddr = get32(bootp + 16);
    dhcp_offer_mask = 0xFFFFFF00u;
    dhcp_offer_gw = 0;
    dhcp_offer_dns = 0;
    dhcp_offer_lease = 0;
    dhcp_parse_options(bootp + 240, (u32)(plen - 240), &msgtype);
    if (msgtype == 2) {                        /* DHCPOFFER */
        dhcp_offer_ip = yiaddr;
        if (dhcp_server_ip == 0) dhcp_server_ip = src_ip;
        dhcp_got_offer = 1;
    } else if (msgtype == 5) {                 /* ACK */
        if (yiaddr != 0) dhcp_offer_ip = yiaddr;
        dhcp_got_ack = 1;
        if (dhcp_state == 2 || dhcp_state == 3) {
            /* Renew/rebind success: re-arm without dropping traffic. */
            dhcp_pending = 0;
            dhcp_apply_ack();
            kprintf("[NET] DHCP lease renewed ip=%u.%u.%u.%u\n",
                    (u64)((local_ip >> 24) & 0xFF), (u64)((local_ip >> 16) & 0xFF),
                    (u64)((local_ip >> 8) & 0xFF), (u64)(local_ip & 0xFF));
        }
    } else if (msgtype == 6) {                 /* NAK */
        if (dhcp_state == 2 || dhcp_state == 3) {
            dhcp_state = 0;
            dhcp_pending = 0;
            dhcp_apply_static_fallback();
        }
    }
}

static void handle_udp(u32 src_ip, u32 dst_ip, const u8 *udp, u32 len) {
    u16 sport;
    u16 dport;
    u16 ulen;
    u32 i;
    const u8 *payload;
    u16 plen;
    (void)dst_ip;
    if (len < UDP_HDR) {
        return;
    }
    sport = get16(udp + 0);
    dport = get16(udp + 2);
    ulen = get16(udp + 4);
    if (ulen < UDP_HDR || ulen > len) {
        return;
    }
    payload = udp + UDP_HDR;
    plen = (u16)(ulen - UDP_HDR);
    if (dport == 68 && dhcp_pending) {
        dhcp_handle_bootp(src_ip, payload, plen);
        return;
    }
    for (i = 1; i < MAX_SOCKS; i++) {
        struct sock *sk = &socks[i];
        if (!sk->used || sk->type != SOCK_DGRAM || !sk->bound) {
            continue;
        }
        if (sk->local_port != dport) {
            continue;
        }
        if (sk->local_ip != 0 && sk->local_ip != dst_ip) {
            continue;
        }
        if (sk->connected) {
            if (sk->peer_ip != src_ip || sk->peer_port != sport) {
                continue;
            }
        }
        sock_enqueue(sk, src_ip, sport, payload, plen);
        return;
    }
}

static void handle_ip(const u8 *pkt, u32 len, const u8 *src_mac) {
    u8 ihl;
    u16 total;
    u8 proto;
    u32 src;
    u32 dst;
    if (len < IP_HDR) {
        return;
    }
    if ((pkt[0] >> 4) != 4) {
        return;
    }
    ihl = (u8)((pkt[0] & 0x0F) * 4);
    if (ihl < IP_HDR || len < ihl) {
        return;
    }
    total = get16(pkt + 2);
    if (total > len || total < ihl) {
        return;
    }
    proto = pkt[9];
    src = get32(pkt + 12);
    dst = get32(pkt + 16);
    arp_learn(src, src_mac);
    /* During DHCP we have no lease yet: accept broadcast and any unicast. */
    if (!dhcp_pending) {
        if (dst != local_ip && dst != 0xFFFFFFFFu && dst != LOOPBACK_IP) {
            return;
        }
    }
    if (proto == IP_PROTO_ICMP) {
        handle_icmp(src, pkt + ihl, (u32)(total - ihl), src_mac);
    } else if (proto == IP_PROTO_UDP) {
        handle_udp(src, dst, pkt + ihl, (u32)(total - ihl));
    } else if (proto == IP_PROTO_TCP) {
        handle_tcp(src, dst, pkt + ihl, (u32)(total - ihl));
    }
}


static void tcp_arm_rexmit(struct sock *sk, u32 seq, u8 flags,
                           const u8 *payload, u16 plen) {
    u32 i;
    sk->rexmit_seq = seq;
    sk->rexmit_flags = flags;
    sk->rexmit_len = plen;
    for (i = 0; i < plen && i < TCP_MAX_PAYLOAD; i++) {
        sk->rexmit_buf[i] = payload[i];
    }
    /* ~500ms at 100Hz */
    sk->rexmit_due = (u32)sched_tick_count() + 50;
}

static void tcp_clear_rexmit(struct sock *sk) {
    sk->rexmit_due = 0;
    sk->rexmit_len = 0;
}

/* Called from the APIC timer ISR path: retransmit unacked SYN/data. */
static void dhcp_timer_tick(void);
void net_tcp_timer(void) {
    dhcp_timer_tick();
    u32 i;
    u32 now;
    if (!net_up) {
        return;
    }
    now = (u32)sched_tick_count();
    for (i = 0; i < MAX_SOCKS; i++) {
        struct sock *sk = &socks[i];
        if (!sk->used || sk->type != SOCK_STREAM) {
            continue;
        }
        if (sk->rexmit_due == 0) {
            continue;
        }
        if ((int)(now - sk->rexmit_due) < 0) {
            continue;
        }
        if (sk->tcp_state == TCPS_SYN_SENT ||
            sk->tcp_state == TCPS_ESTABLISHED ||
            sk->tcp_state == TCPS_FIN_WAIT ||
            sk->tcp_state == TCPS_LAST_ACK) {
            /* No ARP wait from timer ISR. */
            tcp_tx_seg_ex(sk->local_port, sk->peer_ip, sk->peer_port,
                          sk->rexmit_seq, sk->rcv_nxt, sk->rexmit_flags,
                          sk->rexmit_buf, sk->rexmit_len, 0);
            sk->rexmit_due = now + 50;
        } else {
            tcp_clear_rexmit(sk);
        }
    }
}

void net_poll(void) {
    u32 n;
    u16 etype;
    u8 src_mac[6];
    if (!net_up) {
        return;
    }
    while (1) {
        n = virtio_net_rx(rx_frame, FRAME_MAX);
        if (n < ETH_HDR) {
            break;
        }
        mac_copy(src_mac, rx_frame + 6);
        etype = get16(rx_frame + 12);
        if (etype == ETH_TYPE_ARP) {
            handle_arp(rx_frame + ETH_HDR, n - ETH_HDR);
        } else if (etype == ETH_TYPE_IP) {
            handle_ip(rx_frame + ETH_HDR, n - ETH_HDR, src_mac);
        }
    }
}

static u32 resolve_mac(u32 ip, u8 *mac_out) {
    u32 spins;
    u32 i;
    if (ip == 0xFFFFFFFFu) {
        mac_set_bcast(mac_out);
        return 1;
    }
    /* No lease yet: only broadcast is resolvable without a gateway. */
    if (local_ip == 0) {
        return 0;
    }
    if ((ip & netmask) != (local_ip & netmask)) {
        ip = gateway_ip;
    }
    if (arp_lookup(ip, mac_out)) {
        return 1;
    }
    arp_send(ARP_OP_REQ, ip, 0);
    for (spins = 0; spins < 500000; spins++) {
        net_poll();
        if (arp_lookup(ip, mac_out)) {
            return 1;
        }
        for (i = 0; i < 20; i++) {
            (void)i;
        }
    }
    return 0;
}

/* ISR-safe: cache hit only (no ARP wait / net_poll). */
static u32 resolve_mac_cached(u32 ip, u8 *mac_out) {
    if ((ip & netmask) != (local_ip & netmask)) {
        ip = gateway_ip;
    }
    return arp_lookup(ip, mac_out);
}

static u32 udp_deliver_local(u16 sport, u32 dst_ip, u16 dport,
                             const u8 *payload, u16 plen) {
    u32 i;
    u32 hit = 0;
    for (i = 1; i < MAX_SOCKS; i++) {
        struct sock *sk = &socks[i];
        if (!sk->used || sk->type != SOCK_DGRAM || !sk->bound) {
            continue;
        }
        if (sk->local_port != dport) {
            continue;
        }
        if (sk->local_ip != 0 && sk->local_ip != dst_ip) {
            continue;
        }
        if (sk->connected && (sk->peer_ip != local_ip || sk->peer_port != sport)) {
            continue;
        }
        sock_enqueue(sk, local_ip, sport, payload, plen);
        hit = 1;
        break;
    }
    return hit;
}

static u32 udp_tx(u16 sport, u32 dst_ip, u16 dport,
                  const u8 *payload, u16 plen) {
    u8 udp[FRAME_MAX];
    u8 dst_mac[6];
    u32 i;
    if (UDP_HDR + plen > FRAME_MAX - ETH_HDR - IP_HDR) {
        return 0;
    }
    /* Loopback to our address / 127.0.0.1: never leave the host. */
    if (is_local_dst(dst_ip)) {
        return udp_deliver_local(sport, dst_ip, dport, payload, plen);
    }
    if (!resolve_mac(dst_ip, dst_mac)) {
        return 0;
    }
    put16(udp + 0, sport);
    put16(udp + 2, dport);
    put16(udp + 4, (u16)(UDP_HDR + plen));
    put16(udp + 6, 0);                 /* checksum optional for IPv4 */
    for (i = 0; i < plen; i++) {
        udp[UDP_HDR + i] = payload[i];
    }
    return ip_send(dst_ip, IP_PROTO_UDP, udp, UDP_HDR + plen, dst_mac);
}


/* ---------------- TCP ---------------- */

static u16 tcp_checksum(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 tcp_len) {
    u32 sum = 0;
    u32 i;
    /* Pseudo-header */
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += tcp_len;
    i = 0;
    while (i + 1 < tcp_len) {
        sum += ((u32)tcp[i] << 8) | (u32)tcp[i + 1];
        i += 2;
    }
    if (i < tcp_len) {
        sum += ((u32)tcp[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)(~sum);
}

static void stream_push(struct sock *sk, const u8 *data, u16 plen) {
    u32 i;
    for (i = 0; i < plen; i++) {
        if (sk->s_count >= TCP_RX_CAP) {
            break;
        }
        sk->sbuf[sk->s_tail] = data[i];
        sk->s_tail++;
        if (sk->s_tail >= TCP_RX_CAP) {
            sk->s_tail = 0;
        }
        sk->s_count++;
    }
    if (plen > 0) {
        sched_wakeup((u64)sk);
    }
}

static u32 stream_pop(struct sock *sk, u8 *dst, u32 want) {
    u32 n = 0;
    while (n < want && sk->s_count > 0) {
        dst[n] = sk->sbuf[sk->s_head];
        sk->s_head++;
        if (sk->s_head >= TCP_RX_CAP) {
            sk->s_head = 0;
        }
        sk->s_count--;
        n++;
    }
    return n;
}

static void accept_enqueue(struct sock *listener, u32 child_slot) {
    if (listener->accept_count >= TCP_BACKLOG) {
        return;
    }
    listener->accept_q[listener->accept_tail] = child_slot;
    listener->accept_tail++;
    if (listener->accept_tail >= TCP_BACKLOG) {
        listener->accept_tail = 0;
    }
    listener->accept_count++;
    sched_wakeup((u64)listener);
}

static u32 accept_dequeue(struct sock *listener) {
    u32 slot;
    if (listener->accept_count == 0) {
        return 0;
    }
    slot = listener->accept_q[listener->accept_head];
    listener->accept_head++;
    if (listener->accept_head >= TCP_BACKLOG) {
        listener->accept_head = 0;
    }
    listener->accept_count--;
    return slot;
}

static struct sock *tcp_find_sock(u32 src_ip, u16 sport, u32 dst_ip, u16 dport) {
    u32 i;
    struct sock *listen_sk = 0;
    for (i = 1; i < MAX_SOCKS; i++) {
        struct sock *sk = &socks[i];
        if (!sk->used || sk->type != SOCK_STREAM || !sk->bound) {
            continue;
        }
        if (sk->local_port != dport) {
            continue;
        }
        if (sk->local_ip != 0 && sk->local_ip != dst_ip) {
            continue;
        }
        if (sk->tcp_state == TCPS_LISTEN) {
            listen_sk = sk;
            continue;
        }
        if (sk->peer_ip == src_ip && sk->peer_port == sport) {
            return sk;
        }
    }
    return listen_sk;
}

static void tcp_input(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 len) {
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack;
    u8  doff;
    u8  flags;
    u16 plen;
    const u8 *payload;
    struct sock *sk;
    if (len < TCP_HDR) {
        return;
    }
    sport = get16(tcp + 0);
    dport = get16(tcp + 2);
    seq = get32(tcp + 4);
    ack = get32(tcp + 8);
    doff = (u8)((tcp[12] >> 4) * 4);
    if (doff < TCP_HDR || doff > len) {
        return;
    }
    flags = tcp[13];
    plen = (u16)(len - doff);
    payload = tcp + doff;

    sk = tcp_find_sock(src_ip, sport, dst_ip, dport);
    if (!sk) {
        if (flags & TCPF_SYN) {
            /* No listener: RST */
            tcp_tx_seg(dport, src_ip, sport, 0, seq + 1,
                       (u8)(TCPF_RST | TCPF_ACK), 0, 0);
        }
        return;
    }

    if (sk->tcp_state == TCPS_LISTEN) {
        struct sock *child;
        u32 cslot;
        if (!(flags & TCPF_SYN) || (flags & TCPF_ACK)) {
            return;
        }
        if (sk->accept_count >= TCP_BACKLOG) {
            return;
        }
        cslot = sock_alloc_slot();
        if (cslot == 0) {
            return;
        }
        child = &socks[cslot];
        child->type = SOCK_STREAM;
        child->bound = 1;
        child->local_ip = sk->local_ip;
        child->local_port = sk->local_port;
        child->peer_ip = src_ip;
        child->peer_port = sport;
        child->connected = 1;
        {
            u32 pi;
            child->listen_parent = 0;
            for (pi = 1; pi < MAX_SOCKS; pi++) {
                if (&socks[pi] == sk) {
                    child->listen_parent = pi;
                    break;
                }
            }
        }
        child->iss = 1000 + cslot * 97;
        child->snd_nxt = child->iss;
        child->snd_una = child->iss;
        child->irs = seq;
        child->rcv_nxt = seq + 1;
        child->tcp_state = TCPS_SYN_RCVD;
        /* SYN consumes iss; peer's ACK must see snd_nxt == iss+1 even when
         * the SYN-ACK is delivered synchronously on loopback. */
        child->snd_nxt = child->iss + 1;
        tcp_tx_seg(child->local_port, src_ip, sport,
                   child->iss, child->rcv_nxt,
                   (u8)(TCPF_SYN | TCPF_ACK), 0, 0);
        return;
    }

    if (sk->tcp_state == TCPS_SYN_SENT) {
        if ((flags & (TCPF_SYN | TCPF_ACK)) == (TCPF_SYN | TCPF_ACK)) {
            sk->irs = seq;
            sk->rcv_nxt = seq + 1;
            sk->snd_una = ack;
            sk->snd_nxt = ack;
            sk->tcp_state = TCPS_ESTABLISHED;
            sk->connected = 1;
            tcp_clear_rexmit(sk);
            tcp_tx_seg(sk->local_port, src_ip, sport,
                       sk->snd_nxt, sk->rcv_nxt, TCPF_ACK, 0, 0);
            sched_wakeup((u64)sk);
        } else if (flags & TCPF_RST) {
            sk->tcp_state = TCPS_CLOSED;
            tcp_clear_rexmit(sk);
            sched_wakeup((u64)sk);
        }
        return;
    }

    if (sk->tcp_state == TCPS_SYN_RCVD) {
        if (flags & TCPF_RST) {
            sk->used = 0;
            return;
        }
        if ((flags & TCPF_ACK) && ack == sk->snd_nxt) {
            sk->tcp_state = TCPS_ESTABLISHED;
            sk->snd_una = ack;
            if (sk->listen_parent > 0 && sk->listen_parent < MAX_SOCKS) {
                accept_enqueue(&socks[sk->listen_parent],
                               (u32)(sk - socks));
            }
            /* fall through to process any payload/FIN on same segment */
        } else {
            return;
        }
    }

    if (sk->tcp_state != TCPS_ESTABLISHED &&
        sk->tcp_state != TCPS_CLOSE_WAIT &&
        sk->tcp_state != TCPS_FIN_WAIT &&
        sk->tcp_state != TCPS_LAST_ACK) {
        return;
    }

    if (flags & TCPF_RST) {
        sk->tcp_state = TCPS_CLOSED;
        sk->peer_fin = 1;
        tcp_clear_rexmit(sk);
        sched_wakeup((u64)sk);
        return;
    }

    if (flags & TCPF_ACK) {
        /* Cumulative ACK: anything below ack is covered. */
        if (ack > sk->snd_una) {
            sk->snd_una = ack;
        }
        if (sk->snd_una >= sk->snd_nxt) {
            tcp_clear_rexmit(sk);
        }
    }

    if (plen > 0 && seq == sk->rcv_nxt) {
        stream_push(sk, payload, plen);
        sk->rcv_nxt += plen;
        tcp_tx_seg(sk->local_port, src_ip, sport,
                   sk->snd_nxt, sk->rcv_nxt, TCPF_ACK, 0, 0);
    }

    if (flags & TCPF_FIN) {
        u32 fin_seq = seq + plen;
        if (fin_seq == sk->rcv_nxt) {
            sk->rcv_nxt = fin_seq + 1;
            sk->peer_fin = 1;
            tcp_tx_seg(sk->local_port, src_ip, sport,
                       sk->snd_nxt, sk->rcv_nxt, TCPF_ACK, 0, 0);
            if (sk->tcp_state == TCPS_ESTABLISHED) {
                sk->tcp_state = TCPS_CLOSE_WAIT;
            } else if (sk->tcp_state == TCPS_FIN_WAIT) {
                sk->tcp_state = TCPS_CLOSED;
            }
            sched_wakeup((u64)sk);
        }
    }

    if ((flags & TCPF_ACK) && sk->tcp_state == TCPS_LAST_ACK) {
        if (ack == sk->snd_nxt) {
            sk->tcp_state = TCPS_CLOSED;
            sched_wakeup((u64)sk);
        }
    }
}

static u32 tcp_tx_seg_ex(u16 sport, u32 dst_ip, u16 dport,
                         u32 seq, u32 ack, u8 flags,
                         const u8 *payload, u16 plen, u32 allow_arp) {
    u8 seg[FRAME_MAX];
    u8 dst_mac[6];
    u32 i;
    u16 csum;
    u32 seglen = TCP_HDR + plen;

    if (seglen > FRAME_MAX - ETH_HDR - IP_HDR) {
        return 0;
    }
    for (i = 0; i < seglen; i++) {
        seg[i] = 0;
    }
    put16(seg + 0, sport);
    put16(seg + 2, dport);
    put32(seg + 4, seq);
    put32(seg + 8, ack);
    seg[12] = (u8)((TCP_HDR / 4) << 4);
    seg[13] = flags;
    put16(seg + 14, 8192);            /* window */
    put16(seg + 16, 0);               /* checksum filled below */
    for (i = 0; i < plen; i++) {
        seg[TCP_HDR + i] = payload[i];
    }

    /* Loopback: deliver without leaving the host. */
    if (is_local_dst(dst_ip)) {
        u32 src = (dst_ip == LOOPBACK_IP) ? LOOPBACK_IP : local_ip;
        csum = tcp_checksum(src, dst_ip, seg, seglen);
        put16(seg + 16, csum);
        tcp_input(src, dst_ip, seg, seglen);
        return 1;
    }
    if (allow_arp) {
        if (!resolve_mac(dst_ip, dst_mac)) {
            return 0;
        }
    } else if (!resolve_mac_cached(dst_ip, dst_mac)) {
        return 0;
    }
    csum = tcp_checksum(local_ip, dst_ip, seg, seglen);
    put16(seg + 16, csum);
    return ip_send(dst_ip, IP_PROTO_TCP, seg, seglen, dst_mac);
}

static u32 tcp_tx_seg(u16 sport, u32 dst_ip, u16 dport,
                      u32 seq, u32 ack, u8 flags,
                      const u8 *payload, u16 plen) {
    return tcp_tx_seg_ex(sport, dst_ip, dport, seq, ack, flags, payload, plen, 1);
}

static void handle_tcp(u32 src_ip, u32 dst_ip, const u8 *tcp, u32 len) {
    tcp_input(src_ip, dst_ip, tcp, len);
}


/* Blocking ICMP echo to `dst_ip` (host order). Returns 1 on reply. */
u32 net_icmp_ping(u32 dst_ip, u32 timeout_iters) {
    u8 icmp[ICMP_HDR + PING_DATA_LEN];
    u8 dst_mac[6];
    u32 i;
    u16 csum;
    u32 spins;

    if (!net_up) {
        return 0;
    }
    /* Ping self / 127.0.0.1 without leaving the host. */
    if (is_local_dst(dst_ip)) {
        ping_id = (u16)(ping_id + 1);
        if (ping_id == 0) {
            ping_id = 1;
        }
        ping_seq = (u16)(ping_seq + 1);
        ping_target = dst_ip;
        ping_got_reply = 1;
        ping_waiting = 0;
        (void)timeout_iters;
        (void)dst_mac;
        return 1;
    }
    if (!resolve_mac(dst_ip, dst_mac)) {
        kprintf("[NET] ARP failed for %u.%u.%u.%u\n",
                (u64)((dst_ip >> 24) & 0xFF), (u64)((dst_ip >> 16) & 0xFF),
                (u64)((dst_ip >> 8) & 0xFF), (u64)(dst_ip & 0xFF));
        return 0;
    }

    ping_id = (u16)(ping_id + 1);
    if (ping_id == 0) {
        ping_id = 1;
    }
    ping_seq = (u16)(ping_seq + 1);
    ping_target = dst_ip;
    ping_got_reply = 0;
    ping_waiting = 1;

    for (i = 0; i < ICMP_HDR + PING_DATA_LEN; i++) {
        icmp[i] = 0;
    }
    icmp[0] = ICMP_ECHO_REQ;
    put16(icmp + 4, ping_id);
    put16(icmp + 6, ping_seq);
    for (i = 0; i < PING_DATA_LEN; i++) {
        icmp[ICMP_HDR + i] = (u8)(0x40 + i);
    }
    csum = checksum(icmp, ICMP_HDR + PING_DATA_LEN);
    put16(icmp + 2, csum);

    if (!ip_send(dst_ip, IP_PROTO_ICMP, icmp, ICMP_HDR + PING_DATA_LEN, dst_mac)) {
        ping_waiting = 0;
        return 0;
    }

    for (spins = 0; spins < timeout_iters; spins++) {
        net_poll();
        if (ping_got_reply) {
            return 1;
        }
    }
    ping_waiting = 0;
    return 0;
}

u32 net_present(void) {
    return net_up;
}

u32 net_local_ip(void) {
    return local_ip;
}

u32 net_dns_ip(void) {
    return dns_ip;
}

/* Parse a user sockaddr_in (16 bytes) into host-order ip + port. */
static u32 parse_sockaddr_in(u64 uaddr, u64 ulen, u32 *ip_out, u16 *port_out) {
    u64 as = sched_current_as_h();
    volatile u8 *p;
    u16 family;
    if (as == 0 || ulen < SOCKADDR_IN_LEN) {
        return 0;
    }
    if (!mem.as_user_range_ok(as, uaddr, SOCKADDR_IN_LEN)) {
        return 0;
    }
    p = (volatile u8 *)uaddr;
    family = (u16)p[0] | ((u16)p[1] << 8);
    if (family != AF_INET) {
        return 0;
    }
    *port_out = (u16)(((u16)p[2] << 8) | (u16)p[3]);
    *ip_out = ((u32)p[4] << 24) | ((u32)p[5] << 16) | ((u32)p[6] << 8) | (u32)p[7];
    return 1;
}

static u32 write_sockaddr_in(u64 uaddr, u64 ualen_ptr, u32 ip, u16 port) {
    u64 as = sched_current_as_h();
    volatile u8 *p;
    volatile u32 *alenp;
    u32 alen;
    u32 i;
    if (uaddr == 0) {
        return 1;                      /* caller does not want address */
    }
    if (as == 0 || ualen_ptr == 0) {
        return 0;
    }
    if (!mem.as_user_range_ok(as, ualen_ptr, 4)) {
        return 0;
    }
    alenp = (volatile u32 *)ualen_ptr;
    alen = *alenp;
    if (alen < SOCKADDR_IN_LEN) {
        return 0;
    }
    if (!mem.as_user_range_ok(as, uaddr, SOCKADDR_IN_LEN)) {
        return 0;
    }
    p = (volatile u8 *)uaddr;
    p[0] = (u8)(AF_INET & 0xFF);
    p[1] = (u8)((AF_INET >> 8) & 0xFF);
    p[2] = (u8)(port >> 8);
    p[3] = (u8)(port & 0xFF);
    p[4] = (u8)(ip >> 24);
    p[5] = (u8)(ip >> 16);
    p[6] = (u8)(ip >> 8);
    p[7] = (u8)(ip & 0xFF);
    for (i = 8; i < SOCKADDR_IN_LEN; i++) {
        p[i] = 0;
    }
    *alenp = SOCKADDR_IN_LEN;
    return 1;
}

u64 sys_socket(u64 domain, u64 type, u64 protocol) {
    u32 slot;
    u32 fd;
    if (!net_up) {
        return (u64)(-1);
    }
    if (domain != AF_INET) {
        return (u64)(-1);
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return (u64)(-1);
    }
    if (type == SOCK_DGRAM) {
        if (protocol != IPPROTO_IP && protocol != IPPROTO_UDP) {
            return (u64)(-1);
        }
    } else {
        /* TCP scaffold: allow creation; data path still fails. */
        if (protocol != IPPROTO_IP && protocol != IPPROTO_TCP) {
            return (u64)(-1);
        }
    }
    slot = sock_alloc_slot();
    if (slot == 0) {
        return (u64)(-1);
    }
    socks[slot].type = (u32)type;
    fd = sched_fd_alloc();
    if (fd == FD_BAD) {
        socks[slot].used = 0;
        return (u64)(-1);
    }
    sched_fd_set(fd, FH_SOCK_BASE | slot);
    return (u64)fd;
}

u64 sys_bind(u64 fd, u64 uaddr, u64 ulen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk;
    u32 ip;
    u16 port;
    sk = sock_from_fh(fh);
    if (!sk) {
        return (u64)(-1);
    }
    if (sk->bound) {
        return (u64)(-1);
    }
    if (!parse_sockaddr_in(uaddr, ulen, &ip, &port)) {
        return (u64)(-1);
    }
    if (ip != 0 && ip != local_ip && ip != LOOPBACK_IP) {
        return (u64)(-1);
    }
    if (port == 0) {
        port = alloc_ephemeral();
        if (port == 0) {
            return (u64)(-1);
        }
    } else if (port_in_use(port, sock_slot(fh))) {
        return (u64)(-1);
    }
    sk->local_ip = ip;
    sk->local_port = port;
    sk->bound = 1;
    return 0;
}

u64 sys_connect(u64 fd, u64 uaddr, u64 ulen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk;
    u32 ip;
    u16 port;
    u32 spins;
    sk = sock_from_fh(fh);
    if (!sk) {
        return (u64)(-1);
    }
    if (!parse_sockaddr_in(uaddr, ulen, &ip, &port)) {
        return (u64)(-1);
    }
    if (port == 0) {
        return (u64)(-1);
    }
    if (!sk->bound) {
        u16 eph = alloc_ephemeral();
        if (eph == 0) {
            return (u64)(-1);
        }
        sk->local_ip = 0;
        sk->local_port = eph;
        sk->bound = 1;
    }
    sk->peer_ip = ip;
    sk->peer_port = port;
    if (sk->type == SOCK_DGRAM) {
        sk->connected = 1;
        return 0;
    }
    /* TCP active open */
    if (sk->tcp_state != TCPS_CLOSED && sk->tcp_state != 0) {
        return (u64)(-1);
    }
    sk->iss = 5000 + sk->local_port;
    sk->snd_nxt = sk->iss;
    sk->snd_una = sk->iss;
    sk->tcp_state = TCPS_SYN_SENT;
    if (!tcp_tx_seg(sk->local_port, ip, port, sk->iss, 0, TCPF_SYN, 0, 0)) {
        sk->tcp_state = TCPS_CLOSED;
        return (u64)(-1);
    }
    sk->snd_nxt = sk->iss + 1;
    tcp_arm_rexmit(sk, sk->iss, TCPF_SYN, 0, 0);
    for (spins = 0; spins < 2000000; spins++) {
        net_poll();
        if (sk->tcp_state == TCPS_ESTABLISHED) {
            sk->connected = 1;
            tcp_clear_rexmit(sk);
            return 0;
        }
        if (sk->tcp_state == TCPS_CLOSED) {
            tcp_clear_rexmit(sk);
            return (u64)(-1);
        }
        if ((spins & 255) == 0) {
            sched_yield();
        }
    }
    tcp_clear_rexmit(sk);
    sk->tcp_state = TCPS_CLOSED;
    return (u64)(-1);
}

u64 sys_sendto(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ulen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk;
    u64 as = sched_current_as_h();
    u32 dst_ip;
    u16 dst_port;
    u8 kbuf[UDP_MAX_PAYLOAD];
    u32 i;
    u16 plen;
    (void)flags;
    sk = sock_from_fh(fh);
    if (!sk || as == 0) {
        return (u64)(-1);
    }
    if (sk->type == SOCK_STREAM) {
        u8 tbuf[TCP_MAX_PAYLOAD];
        if (sk->shut_wr) {
            return (u64)(-1);
        }
        if (sk->tcp_state != TCPS_ESTABLISHED && sk->tcp_state != TCPS_CLOSE_WAIT) {
            return (u64)(-1);
        }
        if (len > TCP_MAX_PAYLOAD) {
            len = TCP_MAX_PAYLOAD;
        }
        plen = (u16)len;
        if (plen > 0) {
            if (!mem.as_user_range_ok(as, ubuf, plen)) {
                return (u64)(-1);
            }
            {
                volatile u8 *u = (volatile u8 *)ubuf;
                for (i = 0; i < plen; i++) {
                    tbuf[i] = u[i];
                }
            }
        }
        if (!tcp_tx_seg(sk->local_port, sk->peer_ip, sk->peer_port,
                        sk->snd_nxt, sk->rcv_nxt,
                        (u8)(TCPF_PSH | TCPF_ACK), tbuf, plen)) {
            return (u64)(-1);
        }
        tcp_arm_rexmit(sk, sk->snd_nxt, (u8)(TCPF_PSH | TCPF_ACK), tbuf, plen);
        sk->snd_nxt += plen;
        return (u64)plen;
    }
    if (sk->type != SOCK_DGRAM) {
        return (u64)(-1);
    }
    if (len > UDP_MAX_PAYLOAD) {
        len = UDP_MAX_PAYLOAD;
    }
    plen = (u16)len;
    if (uaddr != 0) {
        if (!parse_sockaddr_in(uaddr, ulen, &dst_ip, &dst_port)) {
            return (u64)(-1);
        }
    } else if (sk->connected) {
        dst_ip = sk->peer_ip;
        dst_port = sk->peer_port;
    } else {
        return (u64)(-1);
    }
    if (dst_port == 0) {
        return (u64)(-1);
    }
    if (!sk->bound) {
        u16 eph = alloc_ephemeral();
        if (eph == 0) {
            return (u64)(-1);
        }
        sk->local_ip = 0;
        sk->local_port = eph;
        sk->bound = 1;
    }
    if (plen > 0) {
        if (!mem.as_user_range_ok(as, ubuf, plen)) {
            return (u64)(-1);
        }
        {
            volatile u8 *u = (volatile u8 *)ubuf;
            for (i = 0; i < plen; i++) {
                kbuf[i] = u[i];
            }
        }
    }
    if (!udp_tx(sk->local_port, dst_ip, dst_port, kbuf, plen)) {
        return (u64)(-1);
    }
    return (u64)plen;
}

u64 sys_recvfrom(u64 fd, u64 ubuf, u64 len, u64 flags, u64 uaddr, u64 ualen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk;
    u64 as = sched_current_as_h();
    struct udp_pkt *pkt;
    u32 i;
    u16 n;
    u32 spins;
    (void)flags;
    sk = sock_from_fh(fh);
    if (!sk || as == 0) {
        return (u64)(-1);
    }
    if (sk->type == SOCK_STREAM) {
        u8 tbuf[TCP_MAX_PAYLOAD];
        u32 got;
        if (!sk->bound) {
            return (u64)(-1);
        }
        spins = 0;
        while (sk->s_count == 0) {
            if (sk->peer_fin || sk->tcp_state == TCPS_CLOSED) {
                return 0;             /* EOF */
            }
            net_poll();
            if (sk->s_count > 0) {
                break;
            }
            if ((spins & 63) == 0) {
                sched_sleep((u64)sk);
            } else {
                sched_yield();
            }
            spins++;
            if (spins > 4000000) {
                return (u64)(-1);
            }
            if (!sk->used) {
                return (u64)(-1);
            }
        }
        if (sk->s_count == 0 && sk->peer_fin) {
            return 0;
        }
        if (len > TCP_MAX_PAYLOAD) {
            len = TCP_MAX_PAYLOAD;
        }
        got = stream_pop(sk, tbuf, (u32)len);
        if (got > 0) {
            if (!mem.as_user_range_ok(as, ubuf, got)) {
                return (u64)(-1);
            }
            {
                volatile u8 *u = (volatile u8 *)ubuf;
                for (i = 0; i < got; i++) {
                    u[i] = tbuf[i];
                }
            }
        }
        if (uaddr != 0) {
            if (!write_sockaddr_in(uaddr, ualen, sk->peer_ip, sk->peer_port)) {
                return (u64)(-1);
            }
        }
        return (u64)got;
    }
    if (sk->type != SOCK_DGRAM || !sk->bound) {
        return (u64)(-1);
    }
    spins = 0;
    while (sk->rx_count == 0) {
        net_poll();
        if (sk->rx_count > 0) {
            break;
        }
        /* Yield so idle can run; also sleep on the sock channel. */
        if ((spins & 63) == 0) {
            sched_sleep((u64)sk);
        } else {
            sched_yield();
        }
        spins++;
        if (spins > 2000000) {
            return (u64)(-1);         /* soft timeout */
        }
        if (!sk->used) {
            return (u64)(-1);
        }
    }
    pkt = &sk->rxq[sk->rx_head];
    n = pkt->len;
    if (len < n) {
        n = (u16)len;
    }
    if (n > 0) {
        if (!mem.as_user_range_ok(as, ubuf, n)) {
            return (u64)(-1);
        }
        {
            volatile u8 *u = (volatile u8 *)ubuf;
            for (i = 0; i < n; i++) {
                u[i] = pkt->data[i];
            }
        }
    }
    if (uaddr != 0) {
        if (!write_sockaddr_in(uaddr, ualen, pkt->src_ip, pkt->src_port)) {
            return (u64)(-1);
        }
    }
    sk->rx_head++;
    if (sk->rx_head >= SOCK_RXQ) {
        sk->rx_head = 0;
    }
    sk->rx_count--;
    return (u64)n;
}


u64 sys_listen(u64 fd, u64 backlog) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    (void)backlog;
    if (!sk || sk->type != SOCK_STREAM || !sk->bound) {
        return (u64)(-1);
    }
    sk->tcp_state = TCPS_LISTEN;
    sk->accept_head = 0;
    sk->accept_tail = 0;
    sk->accept_count = 0;
    return 0;
}

u64 sys_accept(u64 fd, u64 uaddr, u64 ualen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    u32 child_slot;
    u32 nfd;
    u32 spins;
    if (!sk || sk->type != SOCK_STREAM || sk->tcp_state != TCPS_LISTEN) {
        return (u64)(-1);
    }
    spins = 0;
    while (sk->accept_count == 0) {
        net_poll();
        if (sk->accept_count > 0) {
            break;
        }
        if ((spins & 63) == 0) {
            sched_sleep((u64)sk);
        } else {
            sched_yield();
        }
        spins++;
        if (spins > 4000000) {
            return (u64)(-1);
        }
        if (!sk->used) {
            return (u64)(-1);
        }
    }
    child_slot = accept_dequeue(sk);
    if (child_slot == 0 || !socks[child_slot].used) {
        return (u64)(-1);
    }
    nfd = sched_fd_alloc();
    if (nfd == FD_BAD) {
        socks[child_slot].used = 0;
        return (u64)(-1);
    }
    sched_fd_set(nfd, FH_SOCK_BASE | child_slot);
    if (uaddr != 0) {
        if (!write_sockaddr_in(uaddr, ualen,
                               socks[child_slot].peer_ip,
                               socks[child_slot].peer_port)) {
            /* still return the fd; address copy is best-effort */
        }
    }
    return (u64)nfd;
}

/* shutdown(fd, how): half-close TCP or mute UDP RX/TX. */
u64 sys_shutdown(u64 fd, u64 how) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    if (!sk) {
        return (u64)(-1);
    }
    if (how > SHUT_RDWR) {
        return (u64)(-1);
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        sk->shut_rd = 1;
        sk->rx_count = 0;
        sk->rx_head = 0;
        sk->rx_tail = 0;
        sk->s_count = 0;
        sk->s_head = 0;
        sk->s_tail = 0;
        sk->peer_fin = 1;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        sk->shut_wr = 1;
        if (sk->type == SOCK_STREAM) {
            if (sk->tcp_state == TCPS_ESTABLISHED) {
                tcp_tx_seg(sk->local_port, sk->peer_ip, sk->peer_port,
                           sk->snd_nxt, sk->rcv_nxt,
                           (u8)(TCPF_FIN | TCPF_ACK), 0, 0);
                sk->snd_nxt++;
                sk->tcp_state = TCPS_FIN_WAIT;
            } else if (sk->tcp_state == TCPS_CLOSE_WAIT) {
                tcp_tx_seg(sk->local_port, sk->peer_ip, sk->peer_port,
                           sk->snd_nxt, sk->rcv_nxt,
                           (u8)(TCPF_FIN | TCPF_ACK), 0, 0);
                sk->snd_nxt++;
                sk->tcp_state = TCPS_LAST_ACK;
            }
        }
    }
    return 0;
}

u64 sys_getsockname(u64 fd, u64 uaddr, u64 ualen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    u32 ip;
    if (!sk || !sk->bound) {
        return (u64)(-1);
    }
    ip = sk->local_ip;
    if (ip == 0) {
        ip = local_ip;
    }
    if (!write_sockaddr_in(uaddr, ualen, ip, sk->local_port)) {
        return (u64)(-1);
    }
    return 0;
}

u64 sys_getpeername(u64 fd, u64 uaddr, u64 ualen) {
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    if (!sk || !sk->connected) {
        return (u64)(-1);
    }
    if (!write_sockaddr_in(uaddr, ualen, sk->peer_ip, sk->peer_port)) {
        return (u64)(-1);
    }
    return 0;
}

enum {
    SOL_SOCKET_N   = 1,
    SO_REUSEADDR_N = 2,
    SO_TYPE_N      = 3,
    SO_ERROR_N     = 4,
    SO_RCVBUF_N    = 8,
    SO_SNDBUF_N    = 7
};

u32 sock_can_read(u32 fh) {
    struct sock *sk = sock_from_fh(fh);
    if (!sk) {
        return 0;
    }
    if (sk->shut_rd) {
        return 1; /* EOF readable */
    }
    if (sk->type == SOCK_STREAM) {
        if (sk->s_count > 0 || sk->peer_fin) {
            return 1;
        }
        return 0;
    }
    return sk->rx_count > 0;
}

u32 sock_can_write(u32 fh) {
    struct sock *sk = sock_from_fh(fh);
    if (!sk) {
        return 0;
    }
    if (sk->shut_wr) {
        return 0;
    }
    if (sk->type == SOCK_STREAM) {
        return sk->tcp_state == TCPS_ESTABLISHED
            || sk->tcp_state == TCPS_CLOSE_WAIT
            || sk->connected;
    }
    return sk->bound || sk->connected;
}

/* setsockopt(2): SOL_SOCKET / SO_REUSEADDR (+ a few query-friendly no-ops). */
u64 sys_setsockopt(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen) {
    u64 as = sched_current_as_h();
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    u32 val = 0;
    if (!sk || as == 0) {
        return (u64)(-1);
    }
    if (level != SOL_SOCKET_N) {
        return (u64)(-1);
    }
    if (optname == SO_REUSEADDR_N) {
        if (optlen < 4 || optval == 0) {
            return (u64)(-1);
        }
        if (!mem.as_user_range_ok(as, optval, 4)) {
            return (u64)(-1);
        }
        val = *(volatile u32 *)optval;
        sk->reuseaddr = val ? 1u : 0u;
        return 0;
    }
    /* Accept-and-ignore common libc probes. */
    if (optname == SO_RCVBUF_N || optname == SO_SNDBUF_N) {
        return 0;
    }
    return (u64)(-1);
}

u64 sys_getsockopt(u64 fd, u64 level, u64 optname, u64 optval, u64 optlen_u) {
    u64 as = sched_current_as_h();
    u32 fh = sched_fd_get((u32)fd);
    struct sock *sk = sock_from_fh(fh);
    u32 val = 0;
    u32 len = 4;
    if (!sk || as == 0 || optval == 0 || optlen_u == 0) {
        return (u64)(-1);
    }
    if (level != SOL_SOCKET_N) {
        return (u64)(-1);
    }
    if (!mem.as_user_range_ok(as, optlen_u, 4)) {
        return (u64)(-1);
    }
    len = *(volatile u32 *)optlen_u;
    if (len < 4) {
        return (u64)(-1);
    }
    if (!mem.as_user_range_ok(as, optval, 4)) {
        return (u64)(-1);
    }
    if (optname == SO_REUSEADDR_N) {
        val = sk->reuseaddr;
    } else if (optname == SO_TYPE_N) {
        val = sk->type;
    } else if (optname == SO_ERROR_N) {
        val = 0;
    } else if (optname == SO_RCVBUF_N) {
        val = sk->type == SOCK_STREAM ? TCP_RX_CAP : UDP_MAX_PAYLOAD;
    } else if (optname == SO_SNDBUF_N) {
        val = 4096;
    } else {
        return (u64)(-1);
    }
    *(volatile u32 *)optval = val;
    *(volatile u32 *)optlen_u = 4;
    return 0;
}

void net_udp_selftest(void) {
    u32 a;
    u32 b;
    u8 payload[8];
    u32 i;
    if (!net_up) {
        return;
    }
    a = sock_alloc_slot();
    b = sock_alloc_slot();
    if (a == 0 || b == 0) {
        kprintf("      [FAIL] UDP selftest: no socket slots\n");
        if (a) {
            socks[a].used = 0;
        }
        if (b) {
            socks[b].used = 0;
        }
        return;
    }
    socks[a].type = SOCK_DGRAM;
    socks[a].local_ip = 0;
    socks[a].local_port = 7;
    socks[a].bound = 1;
    socks[b].type = SOCK_DGRAM;
    socks[b].local_ip = 0;
    socks[b].local_port = alloc_ephemeral();
    socks[b].bound = 1;
    for (i = 0; i < 8; i++) {
        payload[i] = (u8)('A' + i);
    }
    if (!udp_tx(socks[b].local_port, local_ip, 7, payload, 8)) {
        kprintf("      [FAIL] UDP selftest: loopback send\n");
    } else if (socks[a].rx_count == 0) {
        kprintf("      [FAIL] UDP selftest: no loopback delivery\n");
    } else {
        struct udp_pkt *pkt = &socks[a].rxq[socks[a].rx_head];
        u32 ok = (pkt->len == 8);
        for (i = 0; ok && i < 8; i++) {
            if (pkt->data[i] != payload[i]) {
                ok = 0;
            }
        }
        if (ok && pkt->src_port == socks[b].local_port) {
            kprintf("      [PASS] UDP loopback (bind/sendto/recv path)\n");
        } else {
            kprintf("      [FAIL] UDP selftest: payload mismatch\n");
        }
    }
    socks[a].used = 0;
    socks[b].used = 0;
}

void net_tcp_selftest(void) {
    u32 listen_s;
    u32 client_s;
    u32 child_s;
    u8 payload[4];
    u8 got[4];
    u32 i;
    u32 ok;

    if (!net_up) {
        return;
    }
    listen_s = sock_alloc_slot();
    client_s = sock_alloc_slot();
    if (listen_s == 0 || client_s == 0) {
        kprintf("      [FAIL] TCP selftest: no socket slots\n");
        if (listen_s) socks[listen_s].used = 0;
        if (client_s) socks[client_s].used = 0;
        return;
    }
    socks[listen_s].type = SOCK_STREAM;
    socks[listen_s].local_ip = 0;
    socks[listen_s].local_port = 9;
    socks[listen_s].bound = 1;
    socks[listen_s].tcp_state = TCPS_LISTEN;

    socks[client_s].type = SOCK_STREAM;
    socks[client_s].local_ip = 0;
    socks[client_s].local_port = alloc_ephemeral();
    socks[client_s].bound = 1;
    socks[client_s].peer_ip = local_ip;
    socks[client_s].peer_port = 9;
    socks[client_s].iss = 9000;
    socks[client_s].snd_nxt = 9000;
    socks[client_s].snd_una = 9000;
    socks[client_s].tcp_state = TCPS_SYN_SENT;
    if (!tcp_tx_seg(socks[client_s].local_port, local_ip, 9,
                    socks[client_s].iss, 0, TCPF_SYN, 0, 0)) {
        kprintf("      [FAIL] TCP selftest: SYN send\n");
        socks[listen_s].used = 0;
        socks[client_s].used = 0;
        return;
    }
    socks[client_s].snd_nxt = socks[client_s].iss + 1;

    if (socks[client_s].tcp_state != TCPS_ESTABLISHED) {
        kprintf("      [FAIL] TCP selftest: client not established (state=%u)\n",
                (u64)socks[client_s].tcp_state);
        socks[listen_s].used = 0;
        socks[client_s].used = 0;
        return;
    }
    if (socks[listen_s].accept_count == 0) {
        kprintf("      [FAIL] TCP selftest: no accepted child\n");
        socks[listen_s].used = 0;
        socks[client_s].used = 0;
        return;
    }
    child_s = accept_dequeue(&socks[listen_s]);
    if (child_s == 0 || socks[child_s].tcp_state != TCPS_ESTABLISHED) {
        kprintf("      [FAIL] TCP selftest: child not established\n");
        socks[listen_s].used = 0;
        socks[client_s].used = 0;
        if (child_s) socks[child_s].used = 0;
        return;
    }
    payload[0] = 'T'; payload[1] = 'C'; payload[2] = 'P'; payload[3] = '!';
    if (!tcp_tx_seg(socks[client_s].local_port, local_ip,
                    socks[child_s].local_port,
                    socks[client_s].snd_nxt, socks[client_s].rcv_nxt,
                    (u8)(TCPF_PSH | TCPF_ACK), payload, 4)) {
        kprintf("      [FAIL] TCP selftest: data send\n");
    } else {
        socks[client_s].snd_nxt += 4;
    }
    ok = (socks[child_s].s_count >= 4);
    if (ok) {
        stream_pop(&socks[child_s], got, 4);
        for (i = 0; i < 4; i++) {
            if (got[i] != payload[i]) ok = 0;
        }
    }
    if (ok) {
        kprintf("      [PASS] TCP handshake + stream loopback\n");
    } else {
        kprintf("      [FAIL] TCP selftest: payload mismatch (count=%u)\n",
                (u64)socks[child_s].s_count);
    }
    socks[listen_s].used = 0;
    socks[client_s].used = 0;
    socks[child_s].used = 0;
}

/* Minimal DHCP client for QEMU user-net. Falls back to static lease. */
static u32 dhcp_build_discover(u8 *bootp, u32 xid) {
    u32 i;
    for (i = 0; i < 300; i++) bootp[i] = 0;
    bootp[0] = 1;                              /* BOOTREQUEST */
    bootp[1] = 1;                              /* Ethernet */
    bootp[2] = 6;                              /* hlen */
    put32(bootp + 4, xid);
    put16(bootp + 10, 0x8000);                 /* broadcast flag */
    mac_copy(bootp + 28, local_mac);
    bootp[236] = 0x63; bootp[237] = 0x82;
    bootp[238] = 0x53; bootp[239] = 0x63;
    bootp[240] = 53; bootp[241] = 1; bootp[242] = 1; /* DISCOVER */
    bootp[243] = 55; bootp[244] = 5;                 /* param list */
    bootp[245] = 1; bootp[246] = 3; bootp[247] = 6; bootp[248] = 15;
    bootp[249] = 51;                                 /* lease time */
    bootp[250] = 0xFF;
    return 251;
}

static u32 dhcp_build_request(u8 *bootp, u32 xid, u32 req_ip, u32 srv_ip) {
    u32 i;
    for (i = 0; i < 300; i++) bootp[i] = 0;
    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    put32(bootp + 4, xid);
    put16(bootp + 10, 0x8000);
    mac_copy(bootp + 28, local_mac);
    bootp[236] = 0x63; bootp[237] = 0x82;
    bootp[238] = 0x53; bootp[239] = 0x63;
    bootp[240] = 53; bootp[241] = 1; bootp[242] = 3; /* REQUEST */
    bootp[243] = 50; bootp[244] = 4; put32(bootp + 245, req_ip);
    bootp[249] = 54; bootp[250] = 4; put32(bootp + 251, srv_ip);
    bootp[255] = 55; bootp[256] = 5;
    bootp[257] = 1; bootp[258] = 3; bootp[259] = 6; bootp[260] = 15;
    bootp[261] = 51;
    bootp[262] = 0xFF;
    return 263;
}

/* Renew/rebind REQUEST: ciaddr set, broadcast flag clear for renew. */
static u32 dhcp_build_renew(u8 *bootp, u32 xid, u32 ciaddr, u32 srv_ip, u32 rebind) {
    u32 i;
    for (i = 0; i < 300; i++) bootp[i] = 0;
    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    put32(bootp + 4, xid);
    if (rebind) put16(bootp + 10, 0x8000);
    put32(bootp + 12, ciaddr);                     /* ciaddr */
    mac_copy(bootp + 28, local_mac);
    bootp[236] = 0x63; bootp[237] = 0x82;
    bootp[238] = 0x53; bootp[239] = 0x63;
    bootp[240] = 53; bootp[241] = 1; bootp[242] = 3; /* REQUEST */
    i = 243;
    if (srv_ip != 0 && !rebind) {
        bootp[i++] = 54; bootp[i++] = 4; put32(bootp + i, srv_ip); i += 4;
    }
    bootp[i++] = 55; bootp[i++] = 5;
    bootp[i++] = 1; bootp[i++] = 3; bootp[i++] = 6; bootp[i++] = 15;
    bootp[i++] = 51;
    bootp[i++] = 0xFF;
    return i;
}

/* Send UDP from src_ip (may be 0) to dst, bypassing socket table. */
static u32 dhcp_udp_xmit(u32 src_ip, u16 sport, u32 dst_ip, u16 dport,
                         const u8 *payload, u16 plen) {
    u8 udp[FRAME_MAX];
    u8 dst_mac[6];
    u32 i;
    u32 saved = local_ip;
    if (UDP_HDR + plen > FRAME_MAX - ETH_HDR - IP_HDR) return 0;
    mac_set_bcast(dst_mac);
    put16(udp + 0, sport);
    put16(udp + 2, dport);
    put16(udp + 4, (u16)(UDP_HDR + plen));
    put16(udp + 6, 0);
    for (i = 0; i < plen; i++) udp[UDP_HDR + i] = payload[i];
    local_ip = src_ip;                         /* ip_send source */
    i = ip_send(dst_ip, IP_PROTO_UDP, udp, UDP_HDR + plen, dst_mac);
    local_ip = saved;
    return i;
}

static void dhcp_timer_tick(void) {
    u8 bootp[300];
    u32 blen;
    u32 now;
    if (!net_up || dhcp_state == 0) return;
    now = (u32)sched_tick_count();
    if (dhcp_state == 1 && now >= dhcp_t1_tick) {
        dhcp_state = 2;
        dhcp_pending = 1;
        dhcp_got_ack = 0;
        dhcp_next_xmit_tick = now;
        kprintf("[NET] DHCP T1 reached; renewing lease\n");
    }
    if (dhcp_state == 2 && now >= dhcp_t2_tick) {
        dhcp_state = 3;
        dhcp_got_ack = 0;
        dhcp_next_xmit_tick = now;
        kprintf("[NET] DHCP T2 reached; rebinding\n");
    }
    if ((dhcp_state == 2 || dhcp_state == 3) && now >= dhcp_exp_tick) {
        dhcp_state = 0;
        dhcp_pending = 0;
        kprintf("[NET] DHCP lease expired\n");
        dhcp_apply_static_fallback();
        return;
    }
    if (dhcp_state != 2 && dhcp_state != 3) return;
    if (dhcp_next_xmit_tick != 0 && now < dhcp_next_xmit_tick) return;
    blen = dhcp_build_renew(bootp, dhcp_xid, local_ip, dhcp_server_ip,
                            dhcp_state == 3 ? 1u : 0u);
    if (dhcp_state == 2 && dhcp_server_ip != 0) {
        dhcp_udp_xmit(local_ip, 68, dhcp_server_ip, 67, bootp, (u16)blen);
    } else {
        dhcp_udp_xmit(local_ip, 68, 0xFFFFFFFFu, 67, bootp, (u16)blen);
    }
    dhcp_next_xmit_tick = now + 500;
}

static void dhcp_apply_static_fallback(void) {
    local_ip = 0x0A00020Fu;
    netmask = 0xFFFFFF00u;
    gateway_ip = 0x0A000202u;
    dns_ip = 0x0A000202u;
    dhcp_state = 0;
    dhcp_pending = 0;
    kprintf("[NET] DHCP timeout; static fallback 10.0.2.15/24 gw/dns 10.0.2.2\n");
}

static void dhcp_run(void) {
    u8 bootp[300];
    u32 blen;
    u32 spins;
    u32 tries;
    dhcp_pending = 1;
    dhcp_state = 0;
    dhcp_xid = 0xFACE0001u ^ (u32)sched_tick_count();
    if (dhcp_xid == 0) dhcp_xid = 0xFACE0001u;
    dhcp_got_offer = 0;
    dhcp_got_ack = 0;
    dhcp_offer_ip = 0;
    dhcp_server_ip = 0;
    dhcp_offer_lease = 0;
    local_ip = 0;
    netmask = 0xFFFFFF00u;
    gateway_ip = 0;
    dns_ip = 0x0A000202u;

    for (tries = 0; tries < 3 && !dhcp_got_offer; tries++) {
        blen = dhcp_build_discover(bootp, dhcp_xid);
        dhcp_udp_xmit(0, 68, 0xFFFFFFFFu, 67, bootp, (u16)blen);
        for (spins = 0; spins < 800000 && !dhcp_got_offer; spins++) {
            net_poll();
        }
    }
    if (!dhcp_got_offer || dhcp_offer_ip == 0) {
        dhcp_pending = 0;
        dhcp_apply_static_fallback();
        return;
    }
    if (dhcp_server_ip == 0) dhcp_server_ip = gateway_ip ? gateway_ip : 0x0A000202u;

    for (tries = 0; tries < 3 && !dhcp_got_ack; tries++) {
        blen = dhcp_build_request(bootp, dhcp_xid, dhcp_offer_ip, dhcp_server_ip);
        dhcp_udp_xmit(0, 68, 0xFFFFFFFFu, 67, bootp, (u16)blen);
        for (spins = 0; spins < 800000 && !dhcp_got_ack; spins++) {
            net_poll();
        }
    }
    dhcp_pending = 0;
    if (!dhcp_got_ack) {
        dhcp_state = 0;
        dhcp_apply_static_fallback();
        return;
    }
    dhcp_apply_ack();
    kprintf("[NET] DHCP lease ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
            (u64)((local_ip >> 24) & 0xFF), (u64)((local_ip >> 16) & 0xFF),
            (u64)((local_ip >> 8) & 0xFF), (u64)(local_ip & 0xFF),
            (u64)((netmask >> 24) & 0xFF), (u64)((netmask >> 16) & 0xFF),
            (u64)((netmask >> 8) & 0xFF), (u64)(netmask & 0xFF),
            (u64)((gateway_ip >> 24) & 0xFF), (u64)((gateway_ip >> 16) & 0xFF),
            (u64)((gateway_ip >> 8) & 0xFF), (u64)(gateway_ip & 0xFF),
            (u64)((dns_ip >> 24) & 0xFF), (u64)((dns_ip >> 16) & 0xFF),
            (u64)((dns_ip >> 8) & 0xFF), (u64)(dns_ip & 0xFF));
}

void net_selftest(void) {
    u32 ok;
    if (!net_up) {
        return;
    }
    kprintf("[NET] ICMP echo -> 10.0.2.2 (QEMU user gateway)...\n");
    ok = net_icmp_ping(gateway_ip, 2000000);
    if (ok) {
        kprintf("      [PASS] ICMP echo reply from gateway\n");
    } else {
        kprintf("      [FAIL] no ICMP echo reply (is virtio-net + user netdev attached?)\n");
    }
    net_udp_selftest();
    net_tcp_selftest();
}

void net_init(void) {
    u32 i;
    net_up = 0;
    local_ip = 0;
    netmask = 0xFFFFFF00u;
    gateway_ip = 0x0A000202u;
    dns_ip = 0x0A000202u;
    dhcp_pending = 0;
    dhcp_state = 0;
    dhcp_offer_lease = 0;
    dhcp_t1_tick = 0;
    dhcp_t2_tick = 0;
    dhcp_exp_tick = 0;
    dhcp_next_xmit_tick = 0;
    ping_id = 1;
    ping_seq = 0;
    ping_waiting = 0;
    ping_got_reply = 0;
    next_eph = EPHEMERAL_BASE;
    for (i = 0; i < ARP_CACHE; i++) {
        arp_tab[i].used = 0;
    }
    for (i = 0; i < MAX_SOCKS; i++) {
        socks[i].used = 0;
    }

    virtio_net_init();
    if (!virtio_net_present()) {
        return;
    }
    virtio_net_mac(local_mac);
    net_up = 1;                        /* enable RX before DHCP polls */
    dhcp_run();
    kprintf("[NET] up: ip=%u.%u.%u.%u/%u gw=%u.%u.%u.%u mac=%x:%x:%x:%x:%x:%x\n",
            (u64)((local_ip >> 24) & 0xFF), (u64)((local_ip >> 16) & 0xFF),
            (u64)((local_ip >> 8) & 0xFF), (u64)(local_ip & 0xFF),
            (u64)24,
            (u64)((gateway_ip >> 24) & 0xFF), (u64)((gateway_ip >> 16) & 0xFF),
            (u64)((gateway_ip >> 8) & 0xFF), (u64)(gateway_ip & 0xFF),
            (u64)local_mac[0], (u64)local_mac[1], (u64)local_mac[2],
            (u64)local_mac[3], (u64)local_mac[4], (u64)local_mac[5]);
}

/* Syscall wrapper: host-order IPv4, 0 on reply, -1 on failure. */
u64 sys_ping(u64 ip) {
    if (!net_up) {
        return (u64)(-1);
    }
    if (net_icmp_ping((u32)ip, 2000000)) {
        return 0;
    }
    return (u64)(-1);
}
