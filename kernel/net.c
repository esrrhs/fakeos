package kernel;
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

/* ---------------------------------------------------------------------------
 * Minimal L2/L3: Ethernet + ARP + IPv4 + ICMP echo.
 *
 * Static addressing matches QEMU user-mode networking defaults:
 *   guest 10.0.2.15/24, gateway/DNS 10.0.2.2.
 * Host can be reached by ICMP echo (guest ping 10.0.2.2). Incoming echo
 * requests are answered so a bridged setup can ping the guest too.
 * ------------------------------------------------------------------------- */

enum {
    ETH_TYPE_IP   = 0x0800,
    ETH_TYPE_ARP  = 0x0806,
    ARP_OP_REQ    = 1,
    ARP_OP_REP    = 2,
    IP_PROTO_ICMP = 1,
    ICMP_ECHO_REP = 0,
    ICMP_ECHO_REQ = 8,
    ETH_HDR       = 14,
    ARP_LEN       = 28,
    IP_HDR        = 20,
    ICMP_HDR      = 8,
    FRAME_MAX     = 1514,
    ARP_CACHE     = 8,
    PING_DATA_LEN = 32
};

struct arp_entry {
    u32 ip;
    u8  mac[6];
    u32 used;
};

static u8  local_mac[6];
static u32 local_ip;                 /* host order */
static u32 netmask;
static u32 gateway_ip;
static u32 net_up;
static struct arp_entry arp_tab[ARP_CACHE];
static u8  rx_frame[FRAME_MAX];
static u8  tx_frame[FRAME_MAX];
static u16 ping_id;
static u16 ping_seq;
static u32 ping_waiting;
static u32 ping_got_reply;
static u32 ping_target;

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
    /* Overwrite slot 0 when full. */
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
    put16(pkt + 0, 1);                /* HTYPE Ethernet */
    put16(pkt + 2, ETH_TYPE_IP);      /* PTYPE IPv4 */
    pkt[4] = 6;                       /* HLEN */
    pkt[5] = 4;                       /* PLEN */
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
    ippkt[0] = 0x45;                  /* v4, IHL=5 */
    put16(ippkt + 2, (u16)total);
    put16(ippkt + 4, ping_id);        /* identification */
    ippkt[8] = 64;                    /* TTL */
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
    if (dst != local_ip && dst != 0xFFFFFFFFu) {
        return;
    }
    if (proto == IP_PROTO_ICMP) {
        handle_icmp(src, pkt + ihl, (u32)(total - ihl), src_mac);
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
    if (arp_lookup(ip, mac_out)) {
        return 1;
    }
    arp_send(ARP_OP_REQ, ip, 0);
    for (spins = 0; spins < 500000; spins++) {
        net_poll();
        if (arp_lookup(ip, mac_out)) {
            return 1;
        }
        /* Mild delay via a tiny busy touch. */
        for (i = 0; i < 20; i++) {
            (void)i;
        }
    }
    return 0;
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
}

void net_init(void) {
    u32 i;
    net_up = 0;
    local_ip = 0x0A00020Fu;           /* 10.0.2.15 */
    netmask = 0xFFFFFF00u;            /* /24 */
    gateway_ip = 0x0A000202u;         /* 10.0.2.2 */
    ping_id = 1;
    ping_seq = 0;
    ping_waiting = 0;
    ping_got_reply = 0;
    for (i = 0; i < ARP_CACHE; i++) {
        arp_tab[i].used = 0;
    }

    virtio_net_init();
    if (!virtio_net_present()) {
        return;
    }
    virtio_net_mac(local_mac);
    net_up = 1;
    kprintf("[NET] up: ip=10.0.2.15/24 gw=10.0.2.2 mac=%x:%x:%x:%x:%x:%x\n",
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
