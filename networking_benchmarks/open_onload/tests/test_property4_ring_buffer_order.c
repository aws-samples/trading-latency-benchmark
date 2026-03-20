/* Feature: onload-feed-relay-receiver, Property 4: Ring buffer enqueue/dequeue preserves order */
/*
 * Property 4: Ring Buffer Enqueue/Dequeue Preserves Order
 *
 * For any sequence of stats entries enqueued into a lock-free ring buffer,
 * the entries dequeued should appear in the same order they were enqueued.
 * When the buffer overflows (more entries than capacity), the oldest entries
 * should be dropped and the dropped_entries counter should equal the number
 * of overwritten entries.
 *
 * The ring buffer uses power-of-2 sizing with size_mask for efficient modulo.
 *
 * Validates: Requirements 7.2, 7.3
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define NUM_ITERATIONS 200

/*
 * Minimal ring buffer replication matching stats_collector_t from timestamp_common.h.
 * We replicate only the fields and logic needed for enqueue/dequeue order testing.
 */

/* Minimal stats entry - only seq_num matters for order verification */
typedef struct {
    uint32_t seq_num;
    uint64_t timestamp_ns[12];
    uint16_t src_port;
    uint16_t timestamp_mask;
    uint8_t entry_type;
    uint8_t padding[3];
} __attribute__((aligned(64))) test_stats_entry_t;

/* Minimal ring buffer control structure */
typedef struct {
    test_stats_entry_t *buffer;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t size_mask;
    uint32_t capacity;
    uint64_t total_entries;
    uint64_t dropped_entries;
} test_ring_buffer_t;

/* next_power_of_2 from timestamp_common.h */
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

/*
 * stats_enqueue replication from timestamp_common.h.
 * Matches the exact logic: when buffer is full, advance tail (overwrite oldest)
 * and increment dropped_entries.
 */
static inline int ring_enqueue(test_ring_buffer_t *rb, const test_stats_entry_t *entry) {
    if (!rb || !entry || !rb->buffer) return -1;

    uint32_t current_head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t next_head = (current_head + 1) & rb->size_mask;

    /* Buffer full - overwrite oldest (circular buffer behavior) */
    if (next_head == __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE)) {
        rb->tail = (rb->tail + 1) & rb->size_mask;
        __atomic_fetch_add(&rb->dropped_entries, 1, __ATOMIC_RELAXED);
    }

    rb->buffer[current_head] = *entry;
    __atomic_store_n(&rb->head, next_head, __ATOMIC_RELEASE);
    __atomic_fetch_add(&rb->total_entries, 1, __ATOMIC_RELAXED);
    return 0;
}

/* Get number of entries currently in the buffer */
static inline uint32_t ring_count(test_ring_buffer_t *rb) {
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    return (head - tail) & rb->size_mask;
}

/* Dequeue one entry from the buffer (consumer side) */
static inline int ring_dequeue(test_ring_buffer_t *rb, test_stats_entry_t *out) {
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return -1; /* empty */

    *out = rb->buffer[tail];
    __atomic_store_n(&rb->tail, (tail + 1) & rb->size_mask, __ATOMIC_RELEASE);
    return 0;
}

/* Create a test ring buffer with given capacity (will be rounded to power of 2) */
static test_ring_buffer_t* create_test_ring_buffer(uint32_t requested_size) {
    test_ring_buffer_t *rb = calloc(1, sizeof(test_ring_buffer_t));
    if (!rb) return NULL;

    uint32_t actual_size = next_power_of_2(requested_size);
    if (actual_size == 0) { free(rb); return NULL; }

    rb->buffer = calloc(actual_size, sizeof(test_stats_entry_t));
    if (!rb->buffer) { free(rb); return NULL; }

    rb->head = 0;
    rb->tail = 0;
    rb->size_mask = actual_size - 1;
    rb->capacity = actual_size;
    rb->total_entries = 0;
    rb->dropped_entries = 0;
    return rb;
}

static void destroy_test_ring_buffer(test_ring_buffer_t *rb) {
    if (rb) {
        free(rb->buffer);
        free(rb);
    }
}

/* Generate a random uint32_t */
static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

/*
 * Test A: Enqueue N entries (N <= usable capacity), dequeue all, verify FIFO order.
 *
 * Note: The ring buffer reserves one slot to distinguish full from empty,
 * so usable capacity = actual_size - 1.
 */
static int test_fifo_order(int iteration, uint32_t buf_capacity) {
    test_ring_buffer_t *rb = create_test_ring_buffer(buf_capacity);
    if (!rb) {
        fprintf(stderr, "FAIL iteration %d: could not create ring buffer (cap=%u)\n",
                iteration, buf_capacity);
        return 0;
    }

    /* Usable capacity is size_mask (actual_size - 1) because one slot is sentinel */
    uint32_t usable = rb->size_mask;
    /* Pick a random N in [1, usable] */
    uint32_t n = (rand_uint32() % usable) + 1;

    /* Enqueue N entries with sequential seq_num values */
    uint32_t base_seq = rand_uint32();
    for (uint32_t i = 0; i < n; i++) {
        test_stats_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.seq_num = base_seq + i;
        ring_enqueue(rb, &entry);
    }

    /* Verify count */
    uint32_t count = ring_count(rb);
    if (count != n) {
        fprintf(stderr, "FAIL iteration %d: expected count=%u, got %u (cap=%u, n=%u)\n",
                iteration, n, count, buf_capacity, n);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    /* No drops should have occurred */
    if (rb->dropped_entries != 0) {
        fprintf(stderr, "FAIL iteration %d: expected 0 drops, got %lu (cap=%u, n=%u)\n",
                iteration, (unsigned long)rb->dropped_entries, buf_capacity, n);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    /* Dequeue all and verify FIFO order */
    for (uint32_t i = 0; i < n; i++) {
        test_stats_entry_t out;
        if (ring_dequeue(rb, &out) != 0) {
            fprintf(stderr, "FAIL iteration %d: dequeue failed at index %u (cap=%u, n=%u)\n",
                    iteration, i, buf_capacity, n);
            destroy_test_ring_buffer(rb);
            return 0;
        }
        uint32_t expected_seq = base_seq + i;
        if (out.seq_num != expected_seq) {
            fprintf(stderr, "FAIL iteration %d: dequeue[%u] seq_num=%u, expected=%u (cap=%u, n=%u)\n",
                    iteration, i, out.seq_num, expected_seq, buf_capacity, n);
            destroy_test_ring_buffer(rb);
            return 0;
        }
    }

    /* Buffer should now be empty */
    if (ring_count(rb) != 0) {
        fprintf(stderr, "FAIL iteration %d: buffer not empty after dequeue (cap=%u, n=%u)\n",
                iteration, buf_capacity, n);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    destroy_test_ring_buffer(rb);
    return 1;
}

/*
 * Test B: Enqueue more than capacity entries, verify overflow behavior.
 *
 * When we enqueue M entries where M > usable capacity, the oldest (M - usable)
 * entries are dropped. The remaining entries in the buffer should be the last
 * `usable` entries in FIFO order, and dropped_entries == M - usable.
 */
static int test_overflow_drops(int iteration, uint32_t buf_capacity) {
    test_ring_buffer_t *rb = create_test_ring_buffer(buf_capacity);
    if (!rb) {
        fprintf(stderr, "FAIL iteration %d: could not create ring buffer (cap=%u)\n",
                iteration, buf_capacity);
        return 0;
    }

    uint32_t usable = rb->size_mask; /* actual_size - 1 */
    /* Pick overflow amount: enqueue between usable+1 and usable*3 entries */
    uint32_t overflow_extra = (rand_uint32() % (usable * 2)) + 1;
    uint32_t total_enqueue = usable + overflow_extra;

    uint32_t base_seq = rand_uint32();
    for (uint32_t i = 0; i < total_enqueue; i++) {
        test_stats_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.seq_num = base_seq + i;
        ring_enqueue(rb, &entry);
    }

    /* Verify dropped_entries == total_enqueue - usable */
    uint64_t expected_drops = (uint64_t)(total_enqueue - usable);
    if (rb->dropped_entries != expected_drops) {
        fprintf(stderr, "FAIL iteration %d: expected drops=%lu, got %lu (cap=%u, enqueued=%u, usable=%u)\n",
                iteration, (unsigned long)expected_drops, (unsigned long)rb->dropped_entries,
                buf_capacity, total_enqueue, usable);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    /* Verify count == usable (buffer is full) */
    uint32_t count = ring_count(rb);
    if (count != usable) {
        fprintf(stderr, "FAIL iteration %d: expected count=%u, got %u after overflow (cap=%u, enqueued=%u)\n",
                iteration, usable, count, buf_capacity, total_enqueue);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    /* Dequeue all and verify the surviving entries are the last `usable` in FIFO order */
    uint32_t first_surviving_seq = base_seq + (total_enqueue - usable);
    for (uint32_t i = 0; i < usable; i++) {
        test_stats_entry_t out;
        if (ring_dequeue(rb, &out) != 0) {
            fprintf(stderr, "FAIL iteration %d: dequeue failed at index %u after overflow\n",
                    iteration, i);
            destroy_test_ring_buffer(rb);
            return 0;
        }
        uint32_t expected_seq = first_surviving_seq + i;
        if (out.seq_num != expected_seq) {
            fprintf(stderr, "FAIL iteration %d: overflow dequeue[%u] seq_num=%u, expected=%u\n",
                    iteration, i, out.seq_num, expected_seq);
            destroy_test_ring_buffer(rb);
            return 0;
        }
    }

    /* Buffer should be empty */
    if (ring_count(rb) != 0) {
        fprintf(stderr, "FAIL iteration %d: buffer not empty after overflow dequeue\n", iteration);
        destroy_test_ring_buffer(rb);
        return 0;
    }

    destroy_test_ring_buffer(rb);
    return 1;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 4: Ring Buffer Enqueue/Dequeue Preserves Order\n");
    printf("Validates: Requirements 7.2, 7.3\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    /* Random buffer capacities between 4 and 256 for manageable test sizes */
    uint32_t capacity_choices[] = { 4, 8, 16, 32, 64, 128, 256 };
    int num_choices = (int)(sizeof(capacity_choices) / sizeof(capacity_choices[0]));

    /* Test A: FIFO order without overflow */
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t cap = capacity_choices[rand() % num_choices];
        if (test_fifo_order(i, cap)) {
            passed++;
        } else {
            failed++;
        }
    }

    /* Test B: Overflow and dropped_entries count */
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t cap = capacity_choices[rand() % num_choices];
        if (test_overflow_drops(NUM_ITERATIONS + i, cap)) {
            passed++;
        } else {
            failed++;
        }
    }

    /* Edge cases */
    struct {
        uint32_t cap;
        uint32_t enqueue_count;
        const char *desc;
    } edge_cases[] = {
        { 4, 1, "min enqueue into small buffer" },
        { 4, 3, "fill small buffer exactly (usable=3)" },
        { 4, 4, "one overflow in small buffer" },
        { 4, 10, "heavy overflow in small buffer" },
        { 8, 7, "fill size-8 buffer exactly (usable=7)" },
        { 8, 8, "one overflow in size-8 buffer" },
        { 8, 100, "massive overflow in size-8 buffer" },
        { 16, 15, "fill size-16 buffer exactly (usable=15)" },
        { 16, 16, "one overflow in size-16 buffer" },
    };
    int num_edge = (int)(sizeof(edge_cases) / sizeof(edge_cases[0]));

    for (int i = 0; i < num_edge; i++) {
        test_ring_buffer_t *rb = create_test_ring_buffer(edge_cases[i].cap);
        if (!rb) {
            fprintf(stderr, "FAIL edge case '%s': could not create buffer\n", edge_cases[i].desc);
            failed++;
            continue;
        }

        uint32_t usable = rb->size_mask;
        uint32_t n = edge_cases[i].enqueue_count;
        uint32_t base_seq = 1000 + (uint32_t)i * 1000;

        for (uint32_t j = 0; j < n; j++) {
            test_stats_entry_t entry;
            memset(&entry, 0, sizeof(entry));
            entry.seq_num = base_seq + j;
            ring_enqueue(rb, &entry);
        }

        /* Check dropped count */
        uint64_t expected_drops = (n > usable) ? (uint64_t)(n - usable) : 0;
        uint32_t expected_count = (n > usable) ? usable : n;

        if (rb->dropped_entries != expected_drops) {
            fprintf(stderr, "FAIL edge case '%s': expected drops=%lu, got %lu\n",
                    edge_cases[i].desc, (unsigned long)expected_drops,
                    (unsigned long)rb->dropped_entries);
            failed++;
            destroy_test_ring_buffer(rb);
            continue;
        }

        if (ring_count(rb) != expected_count) {
            fprintf(stderr, "FAIL edge case '%s': expected count=%u, got %u\n",
                    edge_cases[i].desc, expected_count, ring_count(rb));
            failed++;
            destroy_test_ring_buffer(rb);
            continue;
        }

        /* Verify FIFO order of surviving entries */
        uint32_t first_seq = (n > usable) ? base_seq + (n - usable) : base_seq;
        int ok = 1;
        for (uint32_t j = 0; j < expected_count; j++) {
            test_stats_entry_t out;
            if (ring_dequeue(rb, &out) != 0) {
                fprintf(stderr, "FAIL edge case '%s': dequeue failed at %u\n",
                        edge_cases[i].desc, j);
                ok = 0;
                break;
            }
            if (out.seq_num != first_seq + j) {
                fprintf(stderr, "FAIL edge case '%s': dequeue[%u] seq=%u, expected=%u\n",
                        edge_cases[i].desc, j, out.seq_num, first_seq + j);
                ok = 0;
                break;
            }
        }

        if (ok) {
            passed++;
        } else {
            failed++;
        }

        destroy_test_ring_buffer(rb);
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
