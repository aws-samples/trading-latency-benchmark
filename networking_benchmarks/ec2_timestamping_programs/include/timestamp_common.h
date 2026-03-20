/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_common.h
 * 
 * Summary: Header definitions for high-performance UDP timestamp measurement infrastructure.
 * 
 * Description: Definitions providing types, structures, constants and function declarations
 * for hardware/software timestamping, TSC-based precision timing, lock-free statistics
 * collection, percentile calculations, CSV logging infrastructure, socket optimization,
 * TX timestamp correlation and performance monitoring. Includes inline implementations
 * for hot-path operations, system compatibility definitions and complete statistical
 * analysis framework for sub-microsecond timestamp latency measurements between client/server.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * FILE HEADER & DEFINITIONS
 */

#ifndef HW_TIMESTAMP_COMMON_H
#define HW_TIMESTAMP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <linux/socket.h>
#include <sys/syscall.h>
#include <linux/net_tstamp.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <linux/if_ether.h>
#include <linux/errqueue.h>
#include "timestamp_logging.h"

/*
 * SYSTEM CONSTANTS & COMPATIBILITY DEFINITIONS
 */

/* Socket options for timestamping and zero-copy operations */
#ifndef SO_TXTIME
#define SO_TXTIME 61
#endif

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef SO_TIMESTAMPING_NEW
#define SO_TIMESTAMPING_NEW 65
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

#ifndef SO_INCOMING_CPU  
#define SO_INCOMING_CPU 49
#endif

#ifndef SO_ATTACH_REUSEPORT_CBPF
#define SO_ATTACH_REUSEPORT_CBPF 51
#endif

/* TX packet correlation arrays size - used for ring buffers (will wrap-around) */
#define MAX_SEQUENCE_NUMBERS 50000

/* Maximum client packet size */
#define MAX_PACKET_SIZE 1500

/*
 * CORE TYPE DEFINITIONS & ENUMS
 */

/* Timestamp entry type definitions */
typedef enum {
    TIMESTAMP_MODE_CLIENT_ONEWAY = 1,
    TIMESTAMP_MODE_CLIENT_ROUNDTRIP = 2,
    TIMESTAMP_MODE_SERVER_ONEWAY = 3,
    TIMESTAMP_MODE_SERVER_ROUNDTRIP = 4
} timestamp_mode_t;

/* CSV type definitions */
typedef enum {
    CSV_CLIENT_MAIN_ONEWAY,
    CSV_CLIENT_MAIN_ROUNDTRIP,
    CSV_CLIENT_TX,
    CSV_SERVER_MAIN_ONEWAY,
    CSV_SERVER_MAIN_ROUNDTRIP,
    CSV_SERVER_TX
} csv_type_t;

/* Statistics Mode Type Definitions */
typedef enum {
    STATS_CLIENT_ONEWAY = 1,
    STATS_CLIENT_ROUNDTRIP = 2,
    STATS_SERVER_ONEWAY = 3,
    STATS_SERVER_ROUNDTRIP = 4
} stats_mode_type_t;

/* Statistics configuration structure */
typedef struct {
    int enabled;
    uint32_t buffer_size;
    uint32_t bin_width_us;
    uint32_t max_bins;
} stats_config_t;

/* TX correlation circular array indexing */
static inline uint32_t get_circular_index(uint32_t seq_num) {
    return seq_num % MAX_SEQUENCE_NUMBERS;
}

/*
 * NETWORK PACKET INFRASTRUCTURE
 */

/* Packet format definitions */
#define ORIGINAL_PACKET_SIZE 8
#define RETURN_PACKET_SIZE 4
#define MAX_PACKET_SIZE 1500

/* RoundTripData structure */
typedef struct {
    uint32_t seq_num;
    
    /* Client timestamps */
    struct __kernel_timespec clt_app_tx_ts;      /* Application Software TX */
    struct __kernel_timespec clt_app_tx_tsc_ts;  /* Application Software TX (TSC) */
    struct __kernel_timespec clt_ker_tx_ts;      /* Kernel Software TX */
    struct __kernel_timespec clt_hw_rx_ts;       /* Hardware RX */
    struct __kernel_timespec clt_ker_rx_ts;      /* Kernel Software RX */
    struct __kernel_timespec clt_app_rx_ts;      /* Application Software RX */
    struct __kernel_timespec clt_app_rx_tsc_ts;  /* Application Software RX (TSC) */
    
    /* Server timestamps */
    struct __kernel_timespec svr_hw_rx_ts;       /* Hardware RX */
    struct __kernel_timespec svr_ker_rx_ts;      /* Kernel Software RX */
    struct __kernel_timespec svr_app_rx_ts;      /* Application Software RX */
    struct __kernel_timespec svr_app_tx_ts;      /* Application Software TX */
    struct __kernel_timespec svr_ker_tx_ts;      /* Kernel Software TX */
    
    /* Client connection info */
    char clt_src_ip[INET_ADDRSTRLEN];
    int clt_src_port;
} RoundTripData;

/*
 * PERFORMANCE & TIMING INFRASTRUCTURE
 */

/* Performance optimizations */
#define BATCH_SIZE 128
#define MAX_SOCKET_BUFFER 16777216
#define PREFETCH_DISTANCE 8
#define HUGE_PAGE_SIZE 2097152
#define CMSG_BUFFER_SIZE 1024

/* TX Timestamp processing */
#define TX_TIMESTAMP_BUFFER_SIZE 65536  
#define TX_TIMESTAMP_BATCH_SIZE 256
#define TX_TIMESTAMP_TIMEOUT_CYCLES (cpu_freq_ghz * 1e9 * 0.05)

/* CPU cycle timing support - Linux x86_64 optimized */
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a" (low), "=d" (high));
    return ((uint64_t)high << 32) | low;
}

/* CPU serializing instruction for precise timing */
static inline uint64_t rdtscp(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtscp" : "=a" (low), "=d" (high) :: "ecx");
    return ((uint64_t)high << 32) | low;
}

/* TSC invariant detection function */
static inline int check_tsc_invariant(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000007));
    return (edx & (1 << 8)) != 0;  /* TSC invariant flag */
}

/* Memory prefetch for better cache performance */
static inline void prefetch(const void *addr) {
    __asm__ volatile ("prefetcht0 %0" : : "m" (*(const char*)addr));
}

/* High-resolution timestamp function */
static inline long long monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* System clock timestamp function */
static inline struct timespec get_system_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

/* CPU cycle based precise delay */
static inline void precise_delay_cycles(uint64_t cycles) {
    uint64_t start = rdtsc();
    while ((rdtsc() - start) < cycles) {
        /* Busy wait with CPU specific pause instruction for efficiency */
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile ("pause");
#elif defined(__aarch64__)
        __asm__ volatile ("yield");
#else
        /* Fallback for other architectures */
        usleep(1);
#endif
    }
}

/* Helper function to calculate timespec difference in nanoseconds */
static inline long long timespec_diff_ns(const struct __kernel_timespec *later, const struct __kernel_timespec *earlier) {
    long long later_ns = later->tv_sec * 1000000000LL + later->tv_nsec;
    long long earlier_ns = earlier->tv_sec * 1000000000LL + earlier->tv_nsec;
    return later_ns - earlier_ns;
}

/*
 * STATISTICS COLLECTION SYSTEM
 */

/* Statistics buffer entry */
typedef struct {
    uint32_t seq_num;                    /* Packet sequence number */
    uint64_t timestamp_ns[12];           /* All possible timestamps */
    uint16_t src_port;                   /* Client source port */
    uint8_t entry_type;                  /* Mode identifier */
    uint8_t timestamp_mask;              /* Which timestamps are valid */
    uint8_t padding[4];                  /* Cache alignment */
} __attribute__((aligned(64))) stats_entry_t;

/* Main statistics control structure */
typedef struct {
    /* Rolling buffer */
    stats_entry_t *buffer;               /* N entries */
    volatile uint32_t head;              /* Producer index (atomic) */
    volatile uint32_t tail;              /* Consumer index (atomic) */
    uint32_t size_mask;                  /* N - 1 (power of 2, for efficient modulo)  */
    uint32_t capacity;                   /* User-specified N */
    
    /* Configuration and metadata */
    uint8_t program_mode;                 /* CLIENT_ONEWAY, SERVER_ROUNDTRIP... */
    
    /* Counters */
    uint64_t total_entries;               /* Total packets processed */
    uint64_t dropped_entries;             /* Entries dropped due to buffer full */
    
    /* Integration with TX correlation system */
    void *tx_correlation_context;        /* Pointer to existing correlation data */
} stats_collector_t;

/*
 * Universal timestamp array mapping:
 * timestamp_ns[0]:  clt_app_tx_tsc_ts
 * timestamp_ns[1]:  clt_app_tx_ts
 * timestamp_ns[2]:  clt_ker_tx_ts
 * timestamp_ns[3]:  clt_hw_rx_ts
 * timestamp_ns[4]:  clt_ker_rx_ts
 * timestamp_ns[5]:  clt_app_rx_tsc_ts
 * timestamp_ns[6]:  clt_app_rx_ts
 * timestamp_ns[7]:  svr_hw_rx_ts
 * timestamp_ns[8]:  svr_ker_rx_ts
 * timestamp_ns[9]:  svr_app_rx_ts
 * timestamp_ns[10]: svr_app_tx_ts
 * timestamp_ns[11]: svr_ker_tx_ts
 * 
 * Mode specific timestamp usage:
 * CLIENT_ONEWAY:     Uses indices 0,1,2 (timestamp_mask = 0x007)
 * CLIENT_ROUNDTRIP:  Uses indices 0,1,2,3,4,5,6 (timestamp_mask = 0x07F)
 * SERVER_ONEWAY:     Uses indices 7,8,9 (timestamp_mask = 0x380)
 * SERVER_ROUNDTRIP:  Uses indices 7,8,9,10,11 (timestamp_mask = 0xF80)
*/

/* Create and destroy statistics collector */
stats_collector_t* create_stats_collector(uint32_t buffer_size, stats_mode_type_t mode);
void destroy_stats_collector(stats_collector_t *stats);

/* Lock-free buffer operations */
static inline int stats_enqueue(stats_collector_t *stats, const stats_entry_t *entry);
static inline uint32_t get_buffer_count(stats_collector_t *stats);
static inline int buffer_has_data(stats_collector_t *stats);
static inline double get_buffer_utilization_percent(stats_collector_t *stats);
static inline int is_buffer_near_full(stats_collector_t *stats);

/* TX timestamp correlation integration functions */
void update_stats_buffer_with_tx_timestamp(uint32_t seq_num, uint64_t ker_tx_ts_ns, stats_mode_type_t mode);
void update_stats_buffer_with_app_tx_timestamp(uint32_t seq_num, uint64_t app_tx_ts_ns, 
                                              uint64_t tsc_tx_ts_ns, stats_mode_type_t mode);
void update_stats_buffer_with_rx_timestamps(uint32_t seq_num, uint64_t hw_rx_ts_ns, uint64_t ker_rx_ts_ns, 
                                           uint64_t app_rx_ts_ns, uint64_t tsc_rx_ts_ns, stats_mode_type_t mode);
void create_minimal_stats_entry(uint32_t seq_num, uint16_t src_port, const char* src_ip,
                               timestamp_mode_t entry_type, stats_mode_type_t mode);

/* Power-of-2 calculation for buffer sizing */
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

/* Inline implementations for hot path functions */
static inline int stats_enqueue(stats_collector_t *stats, const stats_entry_t *entry) {
    if (!stats || !entry || !stats->buffer) {
        return -1;
    }
    
    uint32_t current_head = __atomic_load_n(&stats->head, __ATOMIC_RELAXED);
    uint32_t next_head = (current_head + 1) & stats->size_mask;
    
    /* Check if buffer full (lock-free) */
    if (next_head == __atomic_load_n(&stats->tail, __ATOMIC_ACQUIRE)) {
        /* Buffer full - overwrite oldest entry (circular buffer) */
        stats->tail = (stats->tail + 1) & stats->size_mask;
        __atomic_fetch_add(&stats->dropped_entries, 1, __ATOMIC_RELAXED);
    }
    
    /* Copy entry (cache-line aligned operation) */
    stats->buffer[current_head] = *entry;
    
    /* Publish entry (release semantics) */
    __atomic_store_n(&stats->head, next_head, __ATOMIC_RELEASE);
    __atomic_fetch_add(&stats->total_entries, 1, __ATOMIC_RELAXED);
    return 0;
}

static inline uint32_t get_buffer_count(stats_collector_t *stats) {
    if (!stats) return 0;
    
    uint32_t head = __atomic_load_n(&stats->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&stats->tail, __ATOMIC_ACQUIRE);
    return (head - tail) & stats->size_mask;
}

static inline int buffer_has_data(stats_collector_t *stats) {
    return get_buffer_count(stats) > 0;
}

static inline double get_buffer_utilization_percent(stats_collector_t *stats) {
    if (!stats || stats->capacity == 0) return 0.0;
    
    uint32_t count = get_buffer_count(stats);
    return ((double)count / stats->capacity) * 100.0;
}

static inline int is_buffer_near_full(stats_collector_t *stats) {
    return get_buffer_utilization_percent(stats) > 90.0;
}

/* Delta definition structure for mode specific delta calculations */
typedef struct {
    const char *abbreviation;           /* Short form (e.g., "CAT->CKT") */
    const char *description;            /* Full description */
    uint8_t timestamp_a_index;          /* First timestamp index */
    uint8_t timestamp_b_index;          /* Second timestamp index */
    uint8_t mode_mask;                  /* Which modes use this delta */
} delta_definition_t;


/* Statistical analysis per delta */
typedef struct {
    uint32_t packet_count;              /* Valid packets for this delta */
    double *delta_values;               /* Array of calculated delta values - changed to double for fractional microseconds */
    uint32_t delta_capacity;            /* Allocated array size */
    double exact_percentiles[5];        /* Cached percentiles: P25, P50, P75, P90, P95 */
    uint8_t percentiles_calculated;     /* Flag indicating percentiles are computed */
    uint32_t *histogram;                /* Histogram bin counts */
    uint32_t outlier_count;             /* Packets beyond histogram range */
    uint32_t used_bins;                 /* Number of non-empty bins */
} delta_analysis_t;

/* Complete statistical analysis result */
typedef struct {
    delta_analysis_t deltas[12];        /* Analysis for each possible delta */
    uint8_t active_deltas[12];          /* Which deltas are active for this mode */
    uint8_t delta_count;                /* Number of active deltas */
    stats_mode_type_t mode;             /* Program mode */
    stats_config_t config;              /* Analysis configuration */
} stats_analysis_result_t;

/* All possible deltas across all modes */
extern const delta_definition_t ALL_DELTAS[12];

/* Mode-specific delta index arrays */
extern const uint8_t CLIENT_ONEWAY_DELTAS[1];
extern const uint8_t CLIENT_ROUNDTRIP_DELTAS[6];
extern const uint8_t SERVER_ONEWAY_DELTAS[3];
extern const uint8_t SERVER_ROUNDTRIP_DELTAS[6];

/* Statistical analysis function declarations */
void configure_analysis_for_mode(stats_analysis_result_t *result, stats_mode_type_t mode, stats_config_t *config);

/* Delta processing engine functions */
void process_buffer_for_analysis(stats_collector_t *stats, stats_analysis_result_t *result);
void process_entry_for_delta(stats_entry_t *entry, delta_analysis_t *analysis, uint8_t delta_idx, stats_config_t *config);

/* Analysis display functions */
void display_analysis_results(stats_analysis_result_t *result, uint64_t packets_sent, uint64_t packets_received);
void display_delta_block(delta_analysis_t *analysis, uint8_t delta_idx, stats_config_t *config);

/* Analysis cleanup functions */
void cleanup_analysis_result(stats_analysis_result_t *result);

/* Analysis memory management functions */
int allocate_analysis_histograms(stats_analysis_result_t *result);
int validate_analysis_result(stats_analysis_result_t *result, const char *context);

/* Analysis initialization and control functions */
int initialize_analysis_result(stats_analysis_result_t *result, stats_mode_type_t mode, stats_config_t *config);
void reset_analysis_counters(stats_analysis_result_t *result);

/* Percentile calculation function declarations */
int exact_percentiles_init(delta_analysis_t *analysis, uint32_t capacity);
void exact_percentiles_add_value(delta_analysis_t *analysis, double delta_us);
void exact_percentiles_calculate(delta_analysis_t *analysis);
double exact_percentiles_get(delta_analysis_t *analysis, uint8_t percentile);
void exact_percentiles_cleanup(delta_analysis_t *analysis);

/*
 * CSV LOGGING INFRASTRUCTURE
 */

/* CSV configuration structure */
typedef struct {
    int csv_enabled;
    char csv_filename[256];
    char tx_csv_filename[256];
    int log_cpu;
} csv_config_t;

/* Helper function to get timestamp mask for different modes */
static inline uint8_t get_timestamp_mask_for_mode(timestamp_mode_t entry_type) {
    switch (entry_type) {
        case TIMESTAMP_MODE_CLIENT_ONEWAY:
            return 0x07;
        case TIMESTAMP_MODE_CLIENT_ROUNDTRIP:
            return 0x7F;
        case TIMESTAMP_MODE_SERVER_ONEWAY:
            return 0x80;
        case TIMESTAMP_MODE_SERVER_ROUNDTRIP:
            return 0xF8;
        default:
            return 0x00;
    }
}

/* High-performance CSV logging infrastructure */
typedef struct {
    uint32_t seq_num;
    uint64_t timestamp_ns[12];   
    char src_ip[16];             
    uint16_t src_port;
    uint8_t csv_type;            /* Enum for different CSV formats */
    uint8_t padding[1];          /* Align to proper boundary */
} __attribute__((packed, aligned(64))) csv_entry_t;

/*
 * DIRECT CSV CREATION HELPERS
 */

/* Direct CSV entry creation for Client one-way TX */
static inline void create_csv_client_oneway_tx(csv_entry_t *csv, uint32_t seq_num, 
                                              const char *src_ip, uint16_t src_port, 
                                              uint64_t kernel_tx_ts_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_CLIENT_TX;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[2] = kernel_tx_ts_ns;
}

/* Direct CSV entry creation for Client one-way main */
static inline void create_csv_client_oneway_main(csv_entry_t *csv, uint32_t seq_num,
                                                const char *src_ip, uint16_t src_port,
                                                uint64_t app_tx_ts_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_CLIENT_MAIN_ONEWAY;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[1] = app_tx_ts_ns;
}

/* Direct CSV entry creation for Client round-trip RX */
static inline void create_csv_client_roundtrip_rx(csv_entry_t *csv, uint32_t seq_num,
                                                  const char *src_ip, uint16_t src_port,
                                                  uint64_t tx_tsc_ns, uint64_t app_tx_ns,
                                                  uint64_t hw_rx_ns, uint64_t ker_rx_ns,
                                                  uint64_t rx_tsc_ns, uint64_t app_rx_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_CLIENT_MAIN_ROUNDTRIP;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[0] = tx_tsc_ns;
    csv->timestamp_ns[1] = app_tx_ns;
    csv->timestamp_ns[3] = hw_rx_ns;
    csv->timestamp_ns[4] = ker_rx_ns;
    csv->timestamp_ns[5] = rx_tsc_ns;
    csv->timestamp_ns[6] = app_rx_ns;
}

/* Direct CSV entry creation for Server one-way main */
static inline void create_csv_server_oneway_main(csv_entry_t *csv, uint32_t seq_num,
                                                const char *src_ip, uint16_t src_port,
                                                uint64_t hw_rx_ns, uint64_t ker_rx_ns,
                                                uint64_t app_rx_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_SERVER_MAIN_ONEWAY;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[7] = hw_rx_ns;
    csv->timestamp_ns[8] = ker_rx_ns;
    csv->timestamp_ns[9] = app_rx_ns;
}

/* Direct CSV entry creation for Server round-trip main */
static inline void create_csv_server_roundtrip_main(csv_entry_t *csv, uint32_t seq_num,
                                                   const char *src_ip, uint16_t src_port,
                                                   uint64_t hw_rx_ns, uint64_t ker_rx_ns,
                                                   uint64_t app_rx_ns, uint64_t app_tx_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_SERVER_MAIN_ROUNDTRIP;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[7] = hw_rx_ns;
    csv->timestamp_ns[8] = ker_rx_ns;
    csv->timestamp_ns[9] = app_rx_ns;
    csv->timestamp_ns[10] = app_tx_ns;
}

/* Direct CSV entry creation for Server TX */
static inline void create_csv_server_tx(csv_entry_t *csv, uint32_t seq_num,
                                       const char *src_ip, uint16_t src_port,
                                       uint64_t ker_tx_ts_ns) {
    csv->seq_num = seq_num;
    csv->src_port = src_port;
    strncpy(csv->src_ip, src_ip, 15);
    csv->src_ip[15] = '\0';
    csv->csv_type = CSV_SERVER_TX;
    
    memset(csv->timestamp_ns, 0, sizeof(csv->timestamp_ns));
    csv->timestamp_ns[11] = ker_tx_ts_ns;
}

typedef struct {
    csv_entry_t *entries;
    volatile uint32_t head;      /* Producer index (atomic) */
    volatile uint32_t tail;      /* Consumer index (atomic) */
    uint32_t size_mask;          /* Power of 2 size - 1 */
    uint32_t batch_size;         /* Entries per batch write */
    int fd;                      /* File descriptor */
    pthread_t writer_thread;     /* Dedicated I/O thread */
    volatile int running;        /* Thread control flag */
    char *write_buffer;          /* Pre-allocated format buffer */
    size_t write_buffer_size;
    csv_type_t csv_type;         /* CSV format type */
    int log_cpu;                 
} csv_ring_buffer_t;

/* High-performance CSV logging functions */
csv_ring_buffer_t* csv_buffer_create(uint32_t size, const char *filename, csv_type_t csv_type, uint32_t batch_size, int log_cpu);
void csv_buffer_destroy(csv_ring_buffer_t *buffer);
int csv_dequeue_batch(csv_ring_buffer_t *buffer, csv_entry_t *batch, int max_entries);
void* csv_writer_thread(void* arg);
void csv_format_batch(const csv_entry_t *batch, int count, char *buffer, size_t buffer_size, csv_type_t csv_type);

/* Lock-free CSV enqueue function (inline for performance) */
static inline int csv_enqueue_fast(csv_ring_buffer_t *buffer, const csv_entry_t *entry) {
    uint32_t current_head = __atomic_load_n(&buffer->head, __ATOMIC_RELAXED);
    uint32_t next_head = (current_head + 1) & buffer->size_mask;
    
    /* Check if buffer full (lock-free) */
    if (next_head == __atomic_load_n(&buffer->tail, __ATOMIC_ACQUIRE)) {
        return -1; /* Buffer full - drop packet (hot path continues) */
    }
    
    /* Copy entry (single cache line operation) */
    buffer->entries[current_head] = *entry;
    
    /* Publish entry (release semantics) */
    __atomic_store_n(&buffer->head, next_head, __ATOMIC_RELEASE);
    return 0;
}

/* Mode-aware CSV function */
const char* get_csv_header(csv_type_t type);

/*
 * SOCKET & NETWORK OPTIMIZATION
 */

/* Function declarations */
int optimize_socket_performance(int sockfd, int cpu_id, int is_tx_socket);
int setup_timestamping(int sockfd);
void calibrate_cpu_freq(void);
int optimize_process_scheduling(int cpu_id);
int setup_tx_timestamping(int sockfd);
int process_tx_timestamps_in_main_thread(int sockfd);
int monitor_error_queue_health(int sockfd);

/* RX timestamps extraction */
static inline int extract_rx_timestamps(struct msghdr *msg, 
                                       struct __kernel_timespec *hw_rx_ts, 
                                       struct __kernel_timespec *ker_rx_ts) {
    struct cmsghdr *cmsg;
    int found_timestamps = 0;
    
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING_NEW) {
            struct scm_timestamping64 *tss = (struct scm_timestamping64 *)CMSG_DATA(cmsg);
            
            *hw_rx_ts = tss->ts[2];   /* Hardware RX timestamp */
            *ker_rx_ts = tss->ts[0];  /* Kernel RX timestamp */
            
            found_timestamps = 1;
            break;
        }
    }
    
    return found_timestamps;
}

/* TX timestamp processing thread */
typedef struct {
    int sockfd;
    volatile int running;
    int cpu_core;
    int polling_interval_us;
} tx_timestamp_thread_data_t;

void* tx_timestamp_processing_thread(void* arg);
int start_tx_timestamp_processing_thread(int sockfd, int cpu_core, pthread_t *thread, tx_timestamp_thread_data_t **thread_data);
void stop_tx_timestamp_processing_thread(pthread_t thread, tx_timestamp_thread_data_t *thread_data);

/* TSC to timespec conversion with zero-value handling */
struct __kernel_timespec tsc_to_timespec(uint64_t tsc_cycles);

/* High-performance TSC capture macros */
#define CAPTURE_TSC_TIMESTAMP() rdtsc()
#define TSC_TO_TIMESPEC(tsc) tsc_to_timespec(tsc)

/* High-performance timestamp capture */
static inline struct __kernel_timespec get_app_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (struct __kernel_timespec){ts.tv_sec, ts.tv_nsec};
}

/* Thread creation with realtime priority functions */
int create_realtime_thread(pthread_t *thread, void *(*thread_func)(void *), 
                          void *thread_data, int cpu_core, int priority, 
                          const char *thread_name);
int setup_realtime_thread_attributes(pthread_attr_t *attr, int priority, 
                                   const char *context);

/*
 * GLOBAL STATE & EXTERNAL VARIABLES
 */

/* Lock-free statistics state structure for hot path optimization */
typedef struct {
    volatile uint64_t last_sent;
    volatile uint64_t last_received;
    volatile time_t last_time;
    volatile uint64_t current_sent_pps;
    volatile uint64_t current_received_pps;
    volatile int stats_ready;  /* Flag for main thread consumption */
} stats_state_t;

/* Global variable for CPU frequency */
extern double cpu_freq_ghz;

/* Global TSC reliability flag */
extern volatile int g_tsc_reliable;

/* Global atomic statistics counters (lock-free) */
extern volatile uint64_t g_packets_sent;
extern volatile uint64_t g_packets_received;
extern volatile uint64_t g_packets_tx_timestamp_processed;

/* External declaration for global stats state */
extern stats_state_t g_stats_state;
extern volatile time_t program_start_time;

/* Hot path optimized statistics functions */
void setup_stats_reporting_hotpath(void);
void stats_signal_handler_hotpath(int sig);
void cleanup_stats_reporting_hotpath(void);
void display_stats_if_ready(void);

#endif /* HW_TIMESTAMP_COMMON_H */
