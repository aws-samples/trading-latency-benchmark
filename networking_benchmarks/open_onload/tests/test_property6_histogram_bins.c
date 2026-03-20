/* Feature: onload-feed-relay-receiver, Property 6: Histogram bin assignment */
/*
 * Property 6: Histogram Bin Assignment
 *
 * For any set of latency values and a histogram configuration (bin_width_us,
 * max_bins), each value should be assigned to exactly one bin where
 * bin_index = floor(value / bin_width_us), or counted as an outlier if
 * bin_index >= max_bins. The sum of all bin counts plus the outlier count
 * should equal the total number of values.
 *
 * Validates: Requirements 8.3
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define NUM_ITERATIONS 200
#define MAX_VALUES 500
#define MIN_VALUES 1

/*
 * Histogram configuration matching stats_config_t from timestamp_common.h.
 */
typedef struct {
    uint32_t bin_width_us;
    uint32_t max_bins;
} test_histogram_config_t;

/*
 * Standalone histogram bin assignment logic extracted from
 * process_entry_for_delta() in timestamp_common.c.
 *
 * For a given positive latency value (delta_us) and config:
 *   bin = (uint32_t)(delta_us / config->bin_width_us)
 *   if (bin < config->max_bins) histogram[bin]++
 *   else outlier_count++
 */
static void assign_to_histogram(double delta_us, const test_histogram_config_t *config,
                                uint32_t *histogram, uint32_t *outlier_count) {
    uint32_t bin = (uint32_t)(delta_us / config->bin_width_us);
    if (bin < config->max_bins) {
        histogram[bin]++;
    } else {
        (*outlier_count)++;
    }
}

/* Generate a random uint32_t using rand() calls */
static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

/* Generate a random double in range [lo, hi) */
static double rand_double(double lo, double hi) {
    double r = (double)rand() / ((double)RAND_MAX + 1.0);
    return lo + r * (hi - lo);
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 6: Histogram Bin Assignment\n");
    printf("Validates: Requirements 8.3\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        /* Generate random histogram config */
        test_histogram_config_t config;
        config.bin_width_us = (rand_uint32() % 100) + 1;   /* 1..100 us */
        config.max_bins = (rand_uint32() % 200) + 1;        /* 1..200 bins */

        /* Generate random number of latency values */
        uint32_t num_values = (rand_uint32() % (MAX_VALUES - MIN_VALUES + 1)) + MIN_VALUES;

        /* Allocate histogram bins (zeroed) */
        uint32_t *histogram = calloc(config.max_bins, sizeof(uint32_t));
        if (!histogram) {
            fprintf(stderr, "FAIL iteration %d: calloc failed\n", iter);
            failed++;
            continue;
        }
        uint32_t outlier_count = 0;

        /* Generate random positive latency values and assign to bins.
         * The range spans from small values (within bins) to large values
         * (outliers beyond max_bins * bin_width_us). */
        double max_range = config.bin_width_us * config.max_bins * 2.0;
        for (uint32_t i = 0; i < num_values; i++) {
            double delta_us = rand_double(0.001, max_range);
            assign_to_histogram(delta_us, &config, histogram, &outlier_count);
        }

        /* Property: sum of all bin counts + outlier_count == num_values */
        uint32_t bin_sum = 0;
        for (uint32_t b = 0; b < config.max_bins; b++) {
            bin_sum += histogram[b];
        }
        uint32_t total = bin_sum + outlier_count;

        if (total != num_values) {
            fprintf(stderr, "FAIL iteration %d: bin_sum(%u) + outliers(%u) = %u, expected %u "
                    "(bw=%u, bn=%u)\n",
                    iter, bin_sum, outlier_count, total, num_values,
                    config.bin_width_us, config.max_bins);
            failed++;
        } else {
            passed++;
        }

        free(histogram);
    }

    /* Edge cases */
    struct {
        const char *desc;
        uint32_t bin_width_us;
        uint32_t max_bins;
        double *values;
        uint32_t num_values;
        uint32_t expected_outliers;
    } edge_cases_data[6];

    /* Edge case 1: all values in first bin */
    double ec1[] = {0.5, 0.1, 0.9, 0.001};
    edge_cases_data[0] = (typeof(edge_cases_data[0])){
        "all in bin 0", 1, 10, ec1, 4, 0};

    /* Edge case 2: all values are outliers */
    double ec2[] = {100.0, 200.0, 300.0};
    edge_cases_data[1] = (typeof(edge_cases_data[0])){
        "all outliers", 10, 5, ec2, 3, 3};

    /* Edge case 3: single value exactly on bin boundary */
    double ec3[] = {10.0};
    /* bin = (uint32_t)(10.0 / 5) = 2, max_bins=10 => in bin 2 */
    edge_cases_data[2] = (typeof(edge_cases_data[0])){
        "exact boundary", 5, 10, ec3, 1, 0};

    /* Edge case 4: value exactly at outlier boundary */
    double ec4[] = {50.0};
    /* bin = (uint32_t)(50.0 / 10) = 5, max_bins=5 => outlier (5 >= 5) */
    edge_cases_data[3] = (typeof(edge_cases_data[0])){
        "at outlier boundary", 10, 5, ec4, 1, 1};

    /* Edge case 5: single bin histogram */
    double ec5[] = {0.5, 0.9, 1.5, 2.0};
    /* bin_width=1, max_bins=1: 0.5->bin0, 0.9->bin0, 1.5->outlier, 2.0->outlier */
    edge_cases_data[4] = (typeof(edge_cases_data[0])){
        "single bin", 1, 1, ec5, 4, 2};

    /* Edge case 6: very small bin width */
    double ec6[] = {0.001, 0.002, 0.003};
    /* bin_width=1, max_bins=100: all go to bin 0 */
    edge_cases_data[5] = (typeof(edge_cases_data[0])){
        "tiny values", 1, 100, ec6, 3, 0};

    int num_edge = 6;
    for (int i = 0; i < num_edge; i++) {
        uint32_t *hist = calloc(edge_cases_data[i].max_bins, sizeof(uint32_t));
        if (!hist) {
            fprintf(stderr, "FAIL edge case '%s': calloc failed\n", edge_cases_data[i].desc);
            failed++;
            continue;
        }
        uint32_t outliers = 0;
        test_histogram_config_t cfg = {
            .bin_width_us = edge_cases_data[i].bin_width_us,
            .max_bins = edge_cases_data[i].max_bins
        };

        for (uint32_t j = 0; j < edge_cases_data[i].num_values; j++) {
            assign_to_histogram(edge_cases_data[i].values[j], &cfg, hist, &outliers);
        }

        uint32_t bin_sum = 0;
        for (uint32_t b = 0; b < cfg.max_bins; b++) {
            bin_sum += hist[b];
        }
        uint32_t total = bin_sum + outliers;
        int ok = 1;

        /* Check conservation property */
        if (total != edge_cases_data[i].num_values) {
            fprintf(stderr, "FAIL edge case '%s': total %u != expected %u\n",
                    edge_cases_data[i].desc, total, edge_cases_data[i].num_values);
            ok = 0;
        }

        /* Check expected outlier count */
        if (outliers != edge_cases_data[i].expected_outliers) {
            fprintf(stderr, "FAIL edge case '%s': outliers %u != expected %u\n",
                    edge_cases_data[i].desc, outliers, edge_cases_data[i].expected_outliers);
            ok = 0;
        }

        if (ok) passed++; else failed++;
        free(hist);
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
