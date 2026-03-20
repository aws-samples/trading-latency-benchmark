/* Feature: onload-feed-relay-receiver, Property 7: CSV timestamp formatting round trip */
/*
 * Property 7: CSV Timestamp Formatting Round Trip
 *
 * For any uint64_t nanosecond timestamp value, formatting it as
 * seconds.nanoseconds (e.g., %llu.%09llu with value / 1000000000 and
 * value % 1000000000) and parsing it back should produce the original
 * nanosecond value.
 *
 * Validates: Requirements 10.3, 10.4
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

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
 * Format a nanosecond timestamp as "seconds.nanoseconds" using the same
 * format string as csv_format_batch in timestamp_common.c:
 *   %llu.%09llu  with  value / 1000000000  and  value % 1000000000
 */
static int format_timestamp(char *buf, size_t buf_size, uint64_t ns_value) {
    return snprintf(buf, buf_size, "%llu.%09llu",
                    (unsigned long long)(ns_value / 1000000000ULL),
                    (unsigned long long)(ns_value % 1000000000ULL));
}

/*
 * Parse a "seconds.nanoseconds" formatted string back to a uint64_t
 * nanosecond value: seconds * 1000000000 + nanoseconds
 */
static uint64_t parse_timestamp(const char *buf) {
    char *dot = strchr(buf, '.');
    if (!dot) {
        /* No dot found — treat entire string as seconds */
        return strtoull(buf, NULL, 10) * 1000000000ULL;
    }

    /* Parse seconds part */
    uint64_t seconds = strtoull(buf, NULL, 10);

    /* Parse nanoseconds part (the 9 digits after the dot) */
    uint64_t nanoseconds = strtoull(dot + 1, NULL, 10);

    return seconds * 1000000000ULL + nanoseconds;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 7: CSV Timestamp Formatting Round Trip\n");
    printf("Validates: Requirements 10.3, 10.4\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint64_t orig_ns = rand_uint64();

        /* Format to string */
        char formatted[64];
        format_timestamp(formatted, sizeof(formatted), orig_ns);

        /* Parse back */
        uint64_t parsed_ns = parse_timestamp(formatted);

        /* Assert round-trip equality */
        if (parsed_ns != orig_ns) {
            fprintf(stderr, "FAIL iteration %d: "
                    "orig=%" PRIu64 " formatted='%s' parsed=%" PRIu64 "\n",
                    i, orig_ns, formatted, parsed_ns);
            failed++;
        } else {
            passed++;
        }
    }

    /* Edge cases */
    struct {
        uint64_t ns;
        const char *desc;
    } edge_cases[] = {
        { 0, "zero" },
        { 1, "one nanosecond" },
        { 999999999, "max nanoseconds part (0.999999999)" },
        { 1000000000ULL, "exactly one second" },
        { 1000000001ULL, "one second + one nanosecond" },
        { 1234567891123456789ULL, "realistic timestamp (1234567891.123456789)" },
        { UINT64_MAX, "max uint64" },
        { UINT64_MAX - 1, "max uint64 - 1" },
        { 999999999999999999ULL, "large value below 1e18" },
        { 100000000ULL, "100 milliseconds" },
        { 1000000ULL, "1 millisecond" },
        { 1000ULL, "1 microsecond" },
    };
    int num_edge = (int)(sizeof(edge_cases) / sizeof(edge_cases[0]));

    for (int i = 0; i < num_edge; i++) {
        uint64_t orig_ns = edge_cases[i].ns;

        char formatted[64];
        format_timestamp(formatted, sizeof(formatted), orig_ns);

        uint64_t parsed_ns = parse_timestamp(formatted);

        if (parsed_ns != orig_ns) {
            fprintf(stderr, "FAIL edge case '%s': "
                    "orig=%" PRIu64 " formatted='%s' parsed=%" PRIu64 "\n",
                    edge_cases[i].desc, orig_ns, formatted, parsed_ns);
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
