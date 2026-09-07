// nexthop.h - resolve the Ethernet next-hop MAC for a destination IP.
//
// AF_XDP TX builds raw frames, so it needs the MAC of the NEXT HOP, not of the
// destination. A destination outside the local subnet - another VPC over
// peering, another region, anything off-link - never appears in the ARP cache,
// so looking the destination up directly can only fail there. The kernel picks
// the next hop from the routing table: an on-link route resolves the
// destination itself, a gateway route resolves the gateway. This mirrors that.
//
// Shared by tools/rtt.cpp and tools/mcast_send.cpp, matching the logic in
// Replicator::getDestinationMac (src/Replicator/Net.cpp).

#ifndef AFXDP_COMMON_NEXTHOP_H
#define AFXDP_COMMON_NEXTHOP_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace afxdp {

// Pick the next hop for dest_ip the way the kernel does: longest-prefix match
// over every up route. When the winning route carries RTF_GATEWAY the next hop
// is that gateway; otherwise the destination is on-link and is its own next
// hop. Considering only gateway routes would send same-subnet traffic to the
// router, since the default route matches everything.
static inline bool lookup_next_hop(const char* dest_ip, char* nh_out, size_t nh_len,
                                   bool* via_gateway) {
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return false;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }  // header

    const uint32_t dest_addr = inet_addr(dest_ip);
    uint32_t best_gw = 0;
    int best_len = -1;          // prefix length of the winning route
    bool best_is_gw = false;
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned destination, gateway, flags, mask;
        int refcnt, use, metric;
        if (sscanf(line, "%63s %x %x %x %d %d %d %x",
                   iface, &destination, &gateway, &flags,
                   &refcnt, &use, &metric, &mask) != 8) continue;
        if (!(flags & 0x1)) continue;                    // RTF_UP
        if ((dest_addr & mask) != destination) continue; // does not cover dest
        int len = __builtin_popcount(mask);
        if (len > best_len) {
            best_len = len;
            best_is_gw = (flags & 0x2) != 0;             // RTF_GATEWAY
            best_gw = gateway;
            found = true;
        }
    }
    fclose(f);
    if (!found) return false;

    if (best_is_gw) {
        struct in_addr gw {};
        gw.s_addr = best_gw;
        if (!inet_ntop(AF_INET, &gw, nh_out, (socklen_t)nh_len)) return false;
    } else {
        snprintf(nh_out, nh_len, "%s", dest_ip);
    }
    if (via_gateway) *via_gateway = best_is_gw;
    return true;
}

// One pass over the ARP cache. An all-zero MAC means the entry is incomplete.
static inline bool arp_lookup(const char* ip, uint8_t mac[6]) {
    FILE* f = fopen("/proc/net/arp", "r");
    if (!f) return false;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }  // header

    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char a[64], hw[64], t[64], fl[64], m[64], dev[64];
        if (sscanf(line, "%63s %63s %63s %63s %63s %63s", a, t, fl, hw, m, dev) != 6) continue;
        if (strcmp(a, ip) != 0) continue;
        unsigned b[6];
        if (sscanf(hw, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
        if ((b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) == 0) break;   // incomplete
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
        found = true;
        break;
    }
    fclose(f);
    return found;
}

/**
 * Resolve the next-hop MAC for dst_ip, out of iface.
 *
 * Picks the next hop from the routing table first, so an off-subnet
 * destination resolves its gateway rather than waiting out a lookup for an ARP
 * entry that cannot exist. Sends a zero-length datagram toward the destination
 * to prompt resolution, then polls the cache; the cache is read before the
 * first sleep because in a benchmark the neighbour is usually already known.
 *
 * next_hop_out, when non-null, receives the IP actually resolved - useful for
 * reporting which hop the frame is addressed to.
 */
static inline bool resolve_next_hop_mac(const char* dst_ip, const char* iface,
                                        uint8_t mac[6],
                                        char* next_hop_out = nullptr,
                                        size_t next_hop_len = 0) {
    char hop[INET_ADDRSTRLEN] = {0};
    bool via_gw = false;
    // No usable route means the destination is treated as its own next hop.
    if (!lookup_next_hop(dst_ip, hop, sizeof(hop), &via_gw)) {
        snprintf(hop, sizeof(hop), "%s", dst_ip);
    }
    const char* next_hop = hop;
    if (next_hop_out && next_hop_len) {
        snprintf(next_hop_out, next_hop_len, "%s", next_hop);
    }

    // Prompt neighbour resolution. Addressing the destination is correct even
    // when the next hop is a gateway - the kernel resolves the hop it needs.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface, (socklen_t)(strlen(iface) + 1));
        struct sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(9);          // discard
        inet_pton(AF_INET, dst_ip, &a.sin_addr);
        if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) ::send(s, nullptr, 0, 0);
        ::close(s);
    }

    for (int attempt = 0; attempt < 21; ++attempt) {
        if (attempt) usleep(50000);
        if (arp_lookup(next_hop, mac)) return true;
    }
    return false;
}

}  // namespace afxdp

#endif  // AFXDP_COMMON_NEXTHOP_H
