/* Feature: onload-feed-relay-receiver, Property 3: Power-of-2 buffer sizing */
/*
 * Property 3: Power-of-2 Buffer Sizing
 *
 * For any positive uint32_t value N, next_power_of_2(N) should return a value P
 * such that: (a) P is a power of 2, (b) P >= N, and (c) P is the smallest
 * power of 2 satisfying (b).
 *
 * Special cases:
 *   - next_power_of_2(0) returns 1
 *   - For values > 2^31, overflow returns 0
 *
 * Validates: Requirements 7.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>

#define NUM_ITERATIONS 200

/*
 * Copy of next_power_of_2() from timestamp_common.h for standalone compilation.
 */
static inline uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    if ((n & (n - 1)) == 0) return n;
    uint32_t power = 1;
    while (power < n) {
        power <<= 1;
        if (power == 0) return 0;
    }
    return power;
}

/* Check if a value is a power of 2 (and non-zero) */
static int is_power_of_2(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* Generate a random uint32_t using rand() calls */
static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 3: Power-of-2 Buffer Sizing\n");
    printf("Validates: Requirements 7.1\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t n = rand_uint32();
        uint32_t p = next_power_of_2(n);

        /*
         * For very large values (n > 2^31), the function may overflow and
         * return 0. We only check the three properties when the result is
         * non-zero (i.e., representable in uint32_t).
         */
        if (n == 0) {
            /* Special case: next_power_of_2(0) == 1 */
            if (p != 1) {
                fprintf(stderr, "FAIL iteration %d: next_power_of_2(0) = %u, expected 1\n", i, p);
                failed++;
            } else {
                passed++;
            }
        } else if (n > (uint32_t)0x80000000U) {
            /* Overflow range: result should be 0 (cannot represent in uint32_t) */
            if (p != 0) {
                fprintf(stderr, "FAIL iteration %d: next_power_of_2(%u) = %u, expected 0 (overflow)\n", i, n, p);
                failed++;
            } else {
                passed++;
            }
        } else {
            /* Normal range: check all three properties */
            int ok = 1;

            /* (a) P is a power of 2 */
            if (!is_power_of_2(p)) {
                fprintf(stderr, "FAIL iteration %d: next_power_of_2(%u) = %u is not a power of 2\n", i, n, p);
                ok = 0;
            }

            /* (b) P >= N */
            if (p < n) {
                fprintf(stderr, "FAIL iteration %d: next_power_of_2(%u) = %u < input\n", i, n, p);
                ok = 0;
            }

            /* (c) P is the smallest power of 2 >= N */
            if (is_power_of_2(p) && p >= n && p > 1) {
                /* The previous power of 2 must be < N */
                uint32_t prev = p >> 1;
                if (prev >= n) {
                    fprintf(stderr, "FAIL iteration %d: next_power_of_2(%u) = %u but %u is also a power of 2 >= input\n",
                            i, n, p, prev);
                    ok = 0;
                }
            }

            if (ok) {
                passed++;
            } else {
                failed++;
            }
        }
    }

    /* Also test specific edge cases */
    struct {
        uint32_t input;
        uint32_t expected;
        const char *desc;
    } edge_cases[] = {
        { 0, 1, "zero" },
        { 1, 1, "one (already power of 2)" },
        { 2, 2, "two (already power of 2)" },
        { 3, 4, "three -> 4" },
        { 4, 4, "four (already power of 2)" },
        { 5, 8, "five -> 8" },
        { 7, 8, "seven -> 8" },
        { 8, 8, "eight (already power of 2)" },
        { 9, 16, "nine -> 16" },
        { 16, 16, "sixteen (already power of 2)" },
        { 17, 32, "seventeen -> 32" },
        { 255, 256, "255 -> 256" },
        { 256, 256, "256 (already power of 2)" },
        { 1023, 1024, "1023 -> 1024" },
        { 1024, 1024, "1024 (already power of 2)" },
        { 65535, 65536, "65535 -> 65536" },
        { 65536, 65536, "65536 (already power of 2)" },
        { 0x40000000U, 0x40000000U, "2^30 (already power of 2)" },
        { 0x40000001U, 0x80000000U, "2^30 + 1 -> 2^31" },
        { 0x80000000U, 0x80000000U, "2^31 (already power of 2)" },
        { 0x80000001U, 0, "2^31 + 1 -> overflow (0)" },
        { 0xFFFFFFFFU, 0, "UINT32_MAX -> overflow (0)" },
    };
    int num_edge = (int)(sizeof(edge_cases) / sizeof(edge_cases[0]));

    for (int i = 0; i < num_edge; i++) {
        uint32_t result = next_power_of_2(edge_cases[i].input);
        if (result != edge_cases[i].expected) {
            fprintf(stderr, "FAIL edge case '%s': next_power_of_2(%u) = %u, expected %u\n",
                    edge_cases[i].desc, edge_cases[i].input, result, edge_cases[i].expected);
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
