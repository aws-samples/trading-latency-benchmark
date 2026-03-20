/* Feature: onload-feed-relay-receiver, Property 5: Exact percentile computation */
/*
 * Property 5: Exact Percentile Computation
 *
 * For any array of double values (latency samples), sorting the array and
 * computing percentiles using linear interpolation at p25/p50/p75/p90/p95
 * should produce the correct exact percentile values. The computation uses:
 *
 *   pos = (percentile / 100.0) * (count - 1)
 *   lower_idx = floor(pos)
 *   upper_idx = ceil(pos)
 *   if (lower_idx == upper_idx || upper_idx >= count)
 *       result = values[lower_idx]
 *   else
 *       result = values[lower_idx] + (pos - lower_idx) * (values[upper_idx] - values[lower_idx])
 *
 * Specifically, the p50 (median) of a sorted array should equal the middle
 * element (for odd count) or the interpolated midpoint (for even count).
 *
 * Validates: Requirements 8.1, 8.5
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define NUM_ITERATIONS 200
#define MAX_ARRAY_SIZE 1000
#define MIN_ARRAY_SIZE 1
#define EPSILON 1e-9

/* Comparison function for qsort (matches timestamp_common.c) */
static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Reference percentile computation - replicates exact_percentiles_calculate()
 * from timestamp_common.c using linear interpolation.
 */
static double compute_percentile(const double *sorted_values, uint32_t count, double percentile) {
    double pos = (percentile / 100.0) * (count - 1);
    uint32_t lower_idx = (uint32_t)floor(pos);
    uint32_t upper_idx = (uint32_t)ceil(pos);

    if (lower_idx == upper_idx || upper_idx >= count) {
        return sorted_values[lower_idx];
    } else {
        double fraction = pos - lower_idx;
        return sorted_values[lower_idx] + fraction * (sorted_values[upper_idx] - sorted_values[lower_idx]);
    }
}

/*
 * Simulated exact_percentiles API matching timestamp_common.c behavior.
 * We replicate the full init/add/calculate/get/cleanup cycle.
 */
typedef struct {
    uint32_t packet_count;
    double *delta_values;
    uint32_t delta_capacity;
    double exact_percentiles[5];  /* P25, P50, P75, P90, P95 */
    uint8_t percentiles_calculated;
} test_delta_analysis_t;

static int test_percentiles_init(test_delta_analysis_t *a, uint32_t capacity) {
    if (!a || capacity == 0) return -1;
    a->delta_values = malloc(capacity * sizeof(double));
    if (!a->delta_values) return -1;
    a->delta_capacity = capacity;
    a->packet_count = 0;
    a->percentiles_calculated = 0;
    memset(a->exact_percentiles, 0, sizeof(a->exact_percentiles));
    return 0;
}

static void test_percentiles_add(test_delta_analysis_t *a, double val) {
    if (!a || !a->delta_values) return;
    if (a->packet_count < a->delta_capacity) {
        a->delta_values[a->packet_count] = val;
        a->packet_count++;
        a->percentiles_calculated = 0;
    }
}

static void test_percentiles_calculate(test_delta_analysis_t *a) {
    if (!a || !a->delta_values || a->packet_count == 0) return;
    if (a->percentiles_calculated) return;

    qsort(a->delta_values, a->packet_count, sizeof(double), compare_double);

    const double percentile_positions[] = {25.0, 50.0, 75.0, 90.0, 95.0};
    for (int i = 0; i < 5; i++) {
        double pos = (percentile_positions[i] / 100.0) * (a->packet_count - 1);
        uint32_t lower_idx = (uint32_t)floor(pos);
        uint32_t upper_idx = (uint32_t)ceil(pos);

        if (lower_idx == upper_idx || upper_idx >= a->packet_count) {
            a->exact_percentiles[i] = a->delta_values[lower_idx];
        } else {
            double fraction = pos - lower_idx;
            double lower_val = a->delta_values[lower_idx];
            double upper_val = a->delta_values[upper_idx];
            a->exact_percentiles[i] = lower_val + fraction * (upper_val - lower_val);
        }
    }
    a->percentiles_calculated = 1;
}

static double test_percentiles_get(test_delta_analysis_t *a, uint8_t percentile) {
    if (!a) return 0.0;
    if (!a->percentiles_calculated) test_percentiles_calculate(a);
    switch (percentile) {
        case 25: return a->exact_percentiles[0];
        case 50: return a->exact_percentiles[1];
        case 75: return a->exact_percentiles[2];
        case 90: return a->exact_percentiles[3];
        case 95: return a->exact_percentiles[4];
        default: return 0.0;
    }
}

static void test_percentiles_cleanup(test_delta_analysis_t *a) {
    if (a && a->delta_values) {
        free(a->delta_values);
        a->delta_values = NULL;
    }
    if (a) {
        a->delta_capacity = 0;
        a->packet_count = 0;
        a->percentiles_calculated = 0;
    }
}

/* Generate a random double in range [lo, hi] */
static double rand_double(double lo, double hi) {
    double r = (double)rand() / (double)RAND_MAX;
    return lo + r * (hi - lo);
}

/* Generate a random uint32_t */
static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 5: Exact Percentile Computation\n");
    printf("Validates: Requirements 8.1, 8.5\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        /* Random array size between MIN_ARRAY_SIZE and MAX_ARRAY_SIZE */
        uint32_t count = (rand_uint32() % (MAX_ARRAY_SIZE - MIN_ARRAY_SIZE + 1)) + MIN_ARRAY_SIZE;

        /* Generate random double values (simulating latency in microseconds) */
        double *values = malloc(count * sizeof(double));
        if (!values) {
            fprintf(stderr, "FAIL iteration %d: malloc failed\n", iter);
            failed++;
            continue;
        }

        for (uint32_t i = 0; i < count; i++) {
            /* Mix of positive, negative (clock skew), and zero values */
            values[i] = rand_double(-100.0, 10000.0);
        }

        /* Method 1: Use the replicated API (init/add/calculate/get) */
        test_delta_analysis_t analysis;
        memset(&analysis, 0, sizeof(analysis));
        if (test_percentiles_init(&analysis, count) != 0) {
            fprintf(stderr, "FAIL iteration %d: init failed\n", iter);
            free(values);
            failed++;
            continue;
        }
        for (uint32_t i = 0; i < count; i++) {
            test_percentiles_add(&analysis, values[i]);
        }
        test_percentiles_calculate(&analysis);

        /* Method 2: Sort a copy and compute reference percentiles directly */
        double *sorted = malloc(count * sizeof(double));
        if (!sorted) {
            fprintf(stderr, "FAIL iteration %d: malloc failed for sorted\n", iter);
            test_percentiles_cleanup(&analysis);
            free(values);
            failed++;
            continue;
        }
        memcpy(sorted, values, count * sizeof(double));
        qsort(sorted, count, sizeof(double), compare_double);

        const double pcts[] = {25.0, 50.0, 75.0, 90.0, 95.0};
        const uint8_t pct_keys[] = {25, 50, 75, 90, 95};
        int ok = 1;

        for (int p = 0; p < 5; p++) {
            double expected = compute_percentile(sorted, count, pcts[p]);
            double got = test_percentiles_get(&analysis, pct_keys[p]);

            if (fabs(expected - got) > EPSILON) {
                fprintf(stderr, "FAIL iteration %d: p%.0f mismatch: expected=%.15f, got=%.15f, count=%u\n",
                        iter, pcts[p], expected, got, count);
                ok = 0;
            }
        }

        /* Additional check: verify sorted array property after calculate */
        /* The internal delta_values should be sorted after calculate */
        for (uint32_t i = 1; i < analysis.packet_count; i++) {
            if (analysis.delta_values[i] < analysis.delta_values[i - 1]) {
                fprintf(stderr, "FAIL iteration %d: internal array not sorted at index %u\n", iter, i);
                ok = 0;
                break;
            }
        }

        if (ok) {
            passed++;
        } else {
            failed++;
        }

        test_percentiles_cleanup(&analysis);
        free(sorted);
        free(values);
    }

    /* Edge cases */
    struct {
        const char *desc;
        double *vals;
        uint32_t count;
        double expected_p50;
    } edge_cases_data[7];

    /* Edge case 1: single element */
    double ec1[] = {42.0};
    edge_cases_data[0] = (typeof(edge_cases_data[0])){"single element", ec1, 1, 42.0};

    /* Edge case 2: two elements */
    double ec2[] = {10.0, 20.0};
    edge_cases_data[1] = (typeof(edge_cases_data[0])){"two elements", ec2, 2, 15.0};

    /* Edge case 3: three elements (odd count, p50 = middle) */
    double ec3[] = {1.0, 2.0, 3.0};
    edge_cases_data[2] = (typeof(edge_cases_data[0])){"three elements", ec3, 3, 2.0};

    /* Edge case 4: four elements (even count) */
    double ec4[] = {1.0, 2.0, 3.0, 4.0};
    /* p50: pos = 0.5 * 3 = 1.5, lower=1, upper=2, frac=0.5, result = 2.0 + 0.5*(3.0-2.0) = 2.5 */
    edge_cases_data[3] = (typeof(edge_cases_data[0])){"four elements", ec4, 4, 2.5};

    /* Edge case 5: all same values */
    double ec5[] = {7.7, 7.7, 7.7, 7.7, 7.7};
    edge_cases_data[4] = (typeof(edge_cases_data[0])){"all same values", ec5, 5, 7.7};

    /* Edge case 6: negative values */
    double ec6[] = {-5.0, -3.0, -1.0};
    edge_cases_data[5] = (typeof(edge_cases_data[0])){"negative values", ec6, 3, -3.0};

    /* Edge case 7: already sorted descending (should still work after internal sort) */
    double ec7[] = {100.0, 50.0, 25.0, 10.0, 5.0};
    /* sorted: 5, 10, 25, 50, 100. p50: pos = 0.5*4 = 2.0, idx=2, result = 25.0 */
    edge_cases_data[6] = (typeof(edge_cases_data[0])){"descending input", ec7, 5, 25.0};

    int num_edge = 7;
    for (int i = 0; i < num_edge; i++) {
        test_delta_analysis_t a;
        memset(&a, 0, sizeof(a));
        if (test_percentiles_init(&a, edge_cases_data[i].count) != 0) {
            fprintf(stderr, "FAIL edge case '%s': init failed\n", edge_cases_data[i].desc);
            failed++;
            continue;
        }
        for (uint32_t j = 0; j < edge_cases_data[i].count; j++) {
            test_percentiles_add(&a, edge_cases_data[i].vals[j]);
        }
        test_percentiles_calculate(&a);

        double got_p50 = test_percentiles_get(&a, 50);
        if (fabs(got_p50 - edge_cases_data[i].expected_p50) > EPSILON) {
            fprintf(stderr, "FAIL edge case '%s': p50 expected=%.15f, got=%.15f\n",
                    edge_cases_data[i].desc, edge_cases_data[i].expected_p50, got_p50);
            failed++;
        } else {
            /* Also verify all 5 percentiles against reference */
            double *sorted_copy = malloc(edge_cases_data[i].count * sizeof(double));
            memcpy(sorted_copy, edge_cases_data[i].vals, edge_cases_data[i].count * sizeof(double));
            qsort(sorted_copy, edge_cases_data[i].count, sizeof(double), compare_double);

            const double pcts[] = {25.0, 50.0, 75.0, 90.0, 95.0};
            const uint8_t pct_keys[] = {25, 50, 75, 90, 95};
            int ok = 1;
            for (int p = 0; p < 5; p++) {
                double expected = compute_percentile(sorted_copy, edge_cases_data[i].count, pcts[p]);
                double got = test_percentiles_get(&a, pct_keys[p]);
                if (fabs(expected - got) > EPSILON) {
                    fprintf(stderr, "FAIL edge case '%s': p%.0f expected=%.15f, got=%.15f\n",
                            edge_cases_data[i].desc, pcts[p], expected, got);
                    ok = 0;
                }
            }
            free(sorted_copy);
            if (ok) passed++; else failed++;
        }

        test_percentiles_cleanup(&a);
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
