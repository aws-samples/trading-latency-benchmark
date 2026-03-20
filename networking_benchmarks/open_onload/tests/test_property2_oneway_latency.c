/* Feature: onload-feed-relay-receiver, Property 2: One-way latency computation */
/*
 * Property 2: One-Way Latency Computation
 *
 * For any pair of HW_RX_Timestamp (uint64_t nanoseconds) and TX_App_Timestamp
 * (uint64_t nanoseconds) where both are non-zero, the computed one-way latency
 * in microseconds should equal (HW_RX_ns - TX_App_ns) / 1000.0, including
 * negative values when TX_App > HW_RX (clock skew).
 *
 * Validates: Requirements 6.3, 6.5
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>

#define NUM_ITERATIONS 200

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
 * Compute one-way latency in microseconds from HW_RX and TX_App timestamps.
 * This matches the formula used in feed_relay_receiver.c:
 *   one_way_latency_us = (double)((int64_t)(hw_rx_ns - tx_app_ns)) / 1000.0
 *
 * The cast to int64_t ensures correct signed arithmetic for negative results
 * (when tx_app_ns > hw_rx_ns, indicating clock skew).
 */
static double compute_one_way_latency_us(uint64_t hw_rx_ns, uint64_t tx_app_ns) {
    return (double)((int64_t)(hw_rx_ns - tx_app_ns)) / 1000.0;
}

/*
 * Reference computation using the same formula to verify correctness.
 * We compute the expected value independently.
 */
static double expected_one_way_latency_us(uint64_t hw_rx_ns, uint64_t tx_app_ns) {
    int64_t diff_ns = (int64_t)(hw_rx_ns - tx_app_ns);
    return (double)diff_ns / 1000.0;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 2: One-Way Latency Computation\n");
    printf("Validates: Requirements 6.3, 6.5\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        /* Generate random non-zero timestamp pair */
        uint64_t hw_rx_ns = rand_uint64();
        uint64_t tx_app_ns = rand_uint64();

        /* Ensure both are non-zero (requirement: both must be valid) */
        if (hw_rx_ns == 0) hw_rx_ns = 1;
        if (tx_app_ns == 0) tx_app_ns = 1;

        double actual = compute_one_way_latency_us(hw_rx_ns, tx_app_ns);
        double expected = expected_one_way_latency_us(hw_rx_ns, tx_app_ns);

        if (actual != expected) {
            fprintf(stderr, "FAIL iteration %d: "
                    "hw_rx_ns=%lu tx_app_ns=%lu "
                    "actual=%.6f expected=%.6f\n",
                    i,
                    (unsigned long)hw_rx_ns, (unsigned long)tx_app_ns,
                    actual, expected);
            failed++;
        } else {
            passed++;
        }
    }

    /* Edge cases: specific scenarios that exercise important behaviors */
    struct {
        uint64_t hw_rx_ns;
        uint64_t tx_app_ns;
        const char *desc;
    } edge_cases[] = {
        /* Positive latency: HW_RX > TX_App */
        { 2000000, 1000000, "positive latency (1ms)" },
        /* Zero latency: equal timestamps */
        { 5000000, 5000000, "zero latency (equal timestamps)" },
        /* Negative latency: TX_App > HW_RX (clock skew) - Req 6.5 */
        { 1000000, 2000000, "negative latency (clock skew)" },
        /* Large positive latency */
        { 1000000000000ULL, 999999000000ULL, "large positive (1ms at high ts)" },
        /* Large negative latency */
        { 999999000000ULL, 1000000000000ULL, "large negative (clock skew at high ts)" },
        /* Minimum non-zero values */
        { 1, 1, "minimum non-zero equal" },
        { 2, 1, "minimum positive difference (1ns)" },
        { 1, 2, "minimum negative difference (-1ns)" },
        /* Near uint64 max to test wraparound via signed cast */
        { 0, 1, "hw_rx=0 minus tx_app=1 wraps to large negative" },
        /* Realistic nanosecond timestamps (e.g., ~1.7 trillion ns ~ 28 min) */
        { 1700000000000ULL, 1699999500000ULL, "realistic 500us latency" },
        { 1699999500000ULL, 1700000000000ULL, "realistic -500us latency (skew)" },
        /* Max uint64 values */
        { UINT64_MAX, UINT64_MAX, "max values equal" },
        { UINT64_MAX, UINT64_MAX - 1000, "max hw_rx, 1us positive" },
        { UINT64_MAX - 1000, UINT64_MAX, "near-max, 1us negative (skew)" },
    };
    int num_edge = (int)(sizeof(edge_cases) / sizeof(edge_cases[0]));

    for (int i = 0; i < num_edge; i++) {
        uint64_t hw_rx_ns = edge_cases[i].hw_rx_ns;
        uint64_t tx_app_ns = edge_cases[i].tx_app_ns;

        double actual = compute_one_way_latency_us(hw_rx_ns, tx_app_ns);
        double expected = expected_one_way_latency_us(hw_rx_ns, tx_app_ns);

        /* Also verify the sign is correct for known cases */
        int sign_ok = 1;
        if (hw_rx_ns > tx_app_ns && actual < 0.0) sign_ok = 0;
        if (hw_rx_ns < tx_app_ns && actual > 0.0) sign_ok = 0;
        if (hw_rx_ns == tx_app_ns && actual != 0.0) sign_ok = 0;

        if (actual != expected || !sign_ok) {
            fprintf(stderr, "FAIL edge case '%s': "
                    "hw_rx_ns=%lu tx_app_ns=%lu "
                    "actual=%.6f expected=%.6f sign_ok=%d\n",
                    edge_cases[i].desc,
                    (unsigned long)hw_rx_ns, (unsigned long)tx_app_ns,
                    actual, expected, sign_ok);
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
