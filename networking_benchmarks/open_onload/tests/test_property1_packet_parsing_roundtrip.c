/* Feature: onload-feed-relay-receiver, Property 1: Feed relay packet parsing round trip */
/*
 * Property 1: Feed Relay Packet Parsing Round Trip
 *
 * For any valid sequence number (uint32_t) and TX_App_Timestamp (uint64_t),
 * encoding them into a 12-byte buffer in network byte order (htonl for seq_num,
 * htobe64 for tx_app_ts) and then parsing them back (ntohl, be64toh) should
 * produce the original values.
 *
 * Validates: Requirements 6.1, 6.2
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <arpa/inet.h>
#include <endian.h>

#define NUM_ITERATIONS 200
#define PACKET_MIN_SIZE 12

/* Generate a random uint32_t using rand() calls */
static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

/* Generate a random uint64_t using rand() calls */
static uint64_t rand_uint64(void) {
    uint64_t val = 0;
    val |= (uint64_t)rand_uint32() << 32;
    val |= (uint64_t)rand_uint32();
    return val;
}

/*
 * Encode a (seq_num, tx_app_ts) pair into a 12-byte packet buffer
 * in network byte order, matching the feed relay packet format:
 *   Offset 0: 4 bytes - seq_num in network byte order (htonl)
 *   Offset 4: 8 bytes - tx_app_ts in network byte order (htobe64)
 */
static void encode_feed_relay_packet(uint8_t *buf, uint32_t seq_num, uint64_t tx_app_ts) {
    uint32_t net_seq = htonl(seq_num);
    uint64_t net_ts = htobe64(tx_app_ts);
    memcpy(buf, &net_seq, 4);
    memcpy(buf + 4, &net_ts, 8);
}

/*
 * Decode a 12-byte packet buffer back to (seq_num, tx_app_ts),
 * matching the receiver's parsing logic:
 *   seq_num = ntohl(*(uint32_t *)buf)
 *   tx_app_ts = be64toh(*(uint64_t *)(buf + 4))
 */
static void decode_feed_relay_packet(const uint8_t *buf, uint32_t *seq_num, uint64_t *tx_app_ts) {
    uint32_t net_seq;
    uint64_t net_ts;
    memcpy(&net_seq, buf, 4);
    memcpy(&net_ts, buf + 4, 8);
    *seq_num = ntohl(net_seq);
    *tx_app_ts = be64toh(net_ts);
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 1: Feed Relay Packet Parsing Round Trip\n");
    printf("Validates: Requirements 6.1, 6.2\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        /* Generate random input pair */
        uint32_t orig_seq = rand_uint32();
        uint64_t orig_ts = rand_uint64();

        /* Encode to network byte order into a packet buffer */
        uint8_t packet[PACKET_MIN_SIZE];
        memset(packet, 0, sizeof(packet));
        encode_feed_relay_packet(packet, orig_seq, orig_ts);

        /* Decode back from network byte order */
        uint32_t decoded_seq = 0;
        uint64_t decoded_ts = 0;
        decode_feed_relay_packet(packet, &decoded_seq, &decoded_ts);

        /* Assert round-trip equality */
        if (decoded_seq != orig_seq || decoded_ts != orig_ts) {
            fprintf(stderr, "FAIL iteration %d: "
                    "seq_num: orig=0x%08X decoded=0x%08X, "
                    "tx_app_ts: orig=0x%016lX decoded=0x%016lX\n",
                    i, orig_seq, decoded_seq,
                    (unsigned long)orig_ts, (unsigned long)decoded_ts);
            failed++;
        } else {
            passed++;
        }
    }

    /* Also test specific edge cases */
    struct {
        uint32_t seq;
        uint64_t ts;
        const char *desc;
    } edge_cases[] = {
        { 0, 0, "all zeros" },
        { UINT32_MAX, UINT64_MAX, "max values" },
        { 1, 1, "min non-zero" },
        { 0x80000000, 0x8000000000000000ULL, "high bit set" },
        { 0x01020304, 0x0102030405060708ULL, "sequential bytes" },
        { 0xFF000000, 0xFF00000000000000ULL, "high byte only" },
        { 0x000000FF, 0x00000000000000FFULL, "low byte only" },
    };
    int num_edge = (int)(sizeof(edge_cases) / sizeof(edge_cases[0]));

    for (int i = 0; i < num_edge; i++) {
        uint8_t packet[PACKET_MIN_SIZE];
        encode_feed_relay_packet(packet, edge_cases[i].seq, edge_cases[i].ts);

        uint32_t decoded_seq = 0;
        uint64_t decoded_ts = 0;
        decode_feed_relay_packet(packet, &decoded_seq, &decoded_ts);

        if (decoded_seq != edge_cases[i].seq || decoded_ts != edge_cases[i].ts) {
            fprintf(stderr, "FAIL edge case '%s': "
                    "seq_num: orig=0x%08X decoded=0x%08X, "
                    "tx_app_ts: orig=0x%016lX decoded=0x%016lX\n",
                    edge_cases[i].desc,
                    edge_cases[i].seq, decoded_seq,
                    (unsigned long)edge_cases[i].ts, (unsigned long)decoded_ts);
            failed++;
        } else {
            passed++;
        }
    }

    printf("Results: %d passed, %d failed (out of %d total)\n",
           passed, failed, passed + failed);

    if (failed > 0) {
        printf("PROPERTY TEST FAILED\n");
        return 1;
    }

    printf("PROPERTY TEST PASSED\n");
    return 0;
}
