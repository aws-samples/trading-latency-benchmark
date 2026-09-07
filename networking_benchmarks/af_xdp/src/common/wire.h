/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */

/*
 * wire.h — THE single definition of the on-wire formats shared by the XDP
 * programs, the replicator datapath, and the measurement tools.
 *
 * Deliberately plain C: no C++ features outside the __cplusplus guards, so it is
 * includable from `clang -target bpf` as well as g++.
 *   BPF C:  #include "../common/wire.h"
 *   C++:    #include "common/wire.h"     (built with -I./src)
 *
 * ── m2u frame (multicast-over-unicast tunnel) ──────────────────────────────
 *   Eth(14) | IPv4(20) | UDP(8) | m2u(8) | app payload(>=32)
 *                                 ^ 42     ^ 50
 *   m2u  = magic(4, big-endian) + multicast group(4, network order)
 *   app  = seq(8) | ts_ns(8) | replicator_ns(8) | replicator_tx_ns(8)  [big-endian]
 *          ^0      ^8         ^16                ^24
 *     seq, ts_ns          written by mcast_send
 *     replicator_ns       written by the replicator at RX entry (hop1 boundary)
 *     replicator_tx_ns    written by the replicator just before TX submit (hop2 split)
 *
 * ── rtt --xdp-rx probe ─────────────────────────────────────────────────────
 *   UDP payload: magic(4) | xdp_rx_ns(8) | ... application bytes ...
 *   magic written by the rtt client; xdp_rx_ns stamped by ucast.o at the XDP
 *   ingress hook (bpf_ktime_get_ns, CLOCK_MONOTONIC) and read back by rtt.
 */

#ifndef AFXDP_WIRE_H
#define AFXDP_WIRE_H

/* ── Fixed L2/L3/L4 header sizes (IPv4, no options — see WIRE_IP_IHL_WORDS) ── */
#define WIRE_ETH_LEN            14
#define WIRE_IP_LEN             20
#define WIRE_UDP_LEN            8
/* Fast paths recompute the IPv4 checksum over exactly this many 16-bit words.
 * Frames with IP options (ihl > 5) must therefore be rejected, not patched. */
#define WIRE_IP_IHL_WORDS       10
#define WIRE_IP_IHL_NO_OPTIONS  5

/* ── m2u tunnel header ─────────────────────────────────────────────────────── */
#define WIRE_M2U_MAGIC          0x4D324355u   /* "M2CU", compared in host order */
#define WIRE_M2U_HDR_LEN        8             /* magic(4) + group(4) */
#define WIRE_M2U_GROUP_OFF      4             /* within the m2u header */

/* ── m2u application header (all fields big-endian uint64) ─────────────────── */
#define WIRE_APP_HDR_LEN        32
#define WIRE_APP_SEQ_OFF        0
#define WIRE_APP_TS_NS_OFF      8
#define WIRE_APP_REPL_NS_OFF    16
#define WIRE_APP_REPL_TX_NS_OFF 24

/* ── Absolute offsets from the start of an m2u frame ───────────────────────── */
#define WIRE_M2U_OFF            (WIRE_ETH_LEN + WIRE_IP_LEN + WIRE_UDP_LEN)        /* 42 */
#define WIRE_PAYLOAD_OFF        (WIRE_M2U_OFF + WIRE_M2U_HDR_LEN)                  /* 50 */
#define WIRE_REPL_NS_FRAME_OFF  (WIRE_PAYLOAD_OFF + WIRE_APP_REPL_NS_OFF)          /* 66 */
#define WIRE_REPL_TX_FRAME_OFF  (WIRE_PAYLOAD_OFF + WIRE_APP_REPL_TX_NS_OFF)       /* 74 */
/* Smallest valid m2u frame */
#define WIRE_M2U_MIN_FRAME      (WIRE_PAYLOAD_OFF + WIRE_APP_HDR_LEN)              /* 82 */

/* ── rtt --xdp-rx probe header (inside the UDP payload) ────────────────────── */
#define WIRE_RTT_MAGIC          0x58545452u   /* "RTTX" little-endian */
#define WIRE_RTT_XDP_RX_NS_OFF  4
#define WIRE_RTT_HDR_LEN        12            /* magic(4) + xdp_rx_ns(8) */

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
namespace wire {

/* m2u application header, exactly as it appears on the wire (big-endian). */
struct AppHeader {
    uint64_t seq;
    uint64_t ts_ns;
    uint64_t replicator_ns;      /* 0 until the replicator stamps it at RX entry */
    uint64_t replicator_tx_ns;   /* 0 until the replicator stamps it before TX  */
};

/* The offsets above are load-bearing in BPF C (where this struct is unavailable),
 * so assert the two representations can never drift apart. */
static_assert(sizeof(AppHeader) == WIRE_APP_HDR_LEN,               "app header size drift");
static_assert(offsetof(AppHeader, seq)              == WIRE_APP_SEQ_OFF,        "seq off drift");
static_assert(offsetof(AppHeader, ts_ns)            == WIRE_APP_TS_NS_OFF,      "ts_ns off drift");
static_assert(offsetof(AppHeader, replicator_ns)    == WIRE_APP_REPL_NS_OFF,    "repl_ns off drift");
static_assert(offsetof(AppHeader, replicator_tx_ns) == WIRE_APP_REPL_TX_NS_OFF, "repl_tx_ns off drift");
static_assert(WIRE_PAYLOAD_OFF == 50, "m2u payload offset drift");
static_assert(WIRE_REPL_TX_FRAME_OFF == 74, "replicator_tx_ns frame offset drift");

} // namespace wire
#endif /* __cplusplus */

#endif /* AFXDP_WIRE_H */
