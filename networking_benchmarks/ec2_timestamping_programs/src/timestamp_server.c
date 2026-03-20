/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_server.c
 * 
 * Summary: UDP server for EC2 timestamp latency measurements, to be used with timestamp_client.c.
 * 
 * Description: UDP server supporting one-way and round-trip timestamp measurement modes with
 * sub-microsecond precision. Features multi-threaded architecture with dedicated RX processing
 * and TX thread for return packets, hardware/software timestamp collection, lock-free SPSC queues
 * for inter-thread communication and batch packet transmission using sendmmsg(). Includes CPU
 * affinity optimization, real-time scheduling, memory-aligned allocations, TX timestamp correlation
 * via kernel error queues and lock-free statistics collection with percentiles.
 * Supports CSV logging, signal-safe monitoring and timestamp delta analysis for network performance
 * characterization between server and client endpoints.
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

#include "timestamp_common.h"
#include "timestamp_logging.h"
#include <stdbool.h>
#include <signal.h>

/* Timing constants for optimal time checking across all PPS rates */
#define MAX_TIME_CHECK_INTERVAL_NS 100000000LL
#define MAX_ITERATION_CHECK_INTERVAL 50

/* Function prototypes */
size_t construct_return_packet(char *return_buffer, const char *original_packet);

/* Forward declarations for mode-specific functions */
int run_server_oneway(const char *rx_if_name, int port, int rx_cpu);
int run_server_roundtrip(const char *rx_if_name, const char *tx_if_name, int port, 
                        int rx_cpu, int tx_cpu);

/*
 * GLOBAL STATE MANAGEMENT
 */

/* CSV logging configuration */
static csv_config_t csv_config = {
    .csv_enabled = 0,
    .csv_filename = {0},
    .tx_csv_filename = {0},
    .log_cpu = 0  /* Default to CPU0 */
};

/* Stats collection configuration */
stats_config_t stats_config = {
    .enabled = 0,
    .buffer_size = 5000000,        /* 5M packet entries default */
    .bin_width_us = 10,            /* 10us histogram bins widths default */
    .max_bins = 1000               /* 1000 histogram bins default */
};

/* Stats collector */
stats_collector_t *global_stats_collector = NULL;

/* Runtime control */
static int time_seconds = 0;
static volatile int graceful_shutdown = 0;

/* Arrays for timestamp correlation */
static char (*server_client_ips)[INET_ADDRSTRLEN] = NULL;
static int *server_client_ports = NULL;

/* RX correlation arrays */
static struct timespec *server_hw_rx_timestamps = NULL;
static struct timespec *server_ker_rx_timestamps = NULL;
static struct timespec *server_app_rx_timestamps = NULL;

/* Processing state */
static csv_ring_buffer_t *tx_csv_buffer = NULL;
static int tx_sockfd_global = -1;

/* TX timestamp processing CPU configuration */
static int tx_timestamp_cpu = 0;  /* Default to CPU0 */

/*
 * UTILITY & HELPER FUNCTIONS
 */

/* Helper functions for min/max calculations */
static inline size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

static inline int max(int a, int b) {
    return a > b ? a : b;
}

/*
 * CONFIGURATION & ARGUMENT PROCESSING
 */

/* Stats argument parsing function */
void parse_stats_argument(const char *arg, stats_config_t *config) {
    if (!arg || !config) return;
    
    char *arg_copy = strdup(arg);
    if (!arg_copy) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to allocate memory for stats argument parsing");
        exit(EXIT_FAILURE);
    }
    
    /* Parse buffer size */
    char *token = strtok(arg_copy, ",");
    if (token) {
        /* Parse buffer size with optional M/K suffix */
        char *endptr;
        unsigned long size = strtoul(token, &endptr, 10);
        
        if (endptr && *endptr != '\0') {
            if (*endptr == 'M' || *endptr == 'm') {
                size = size * 1000000;
            } else if (*endptr == 'K' || *endptr == 'k') {
                size = size * 1000;
            } else {
                HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Invalid buffer size suffix '%c' (use M or K)", *endptr);
                free(arg_copy);
                exit(EXIT_FAILURE);
            }
        }
        
        /* Validate buffer size range */
        if (size < 10000 || size > 10000000) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Buffer size must be between 10K and 10M entries");
            free(arg_copy);
            exit(EXIT_FAILURE);
        }
        
        config->buffer_size = (uint32_t)size;
    }
    
    /* Parse optional bin width */
    while ((token = strtok(NULL, ",")) != NULL) {
        if (strncmp(token, "bw=", 3) == 0) {
            unsigned long bin_width = strtoul(token + 3, NULL, 10);
            
            /* Validate bin width range */
            if (bin_width < 1 || bin_width > 1000) {
                HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Bin width must be between 1 and 1000 microseconds");
                free(arg_copy);
                exit(EXIT_FAILURE);
            }
            
            config->bin_width_us = (uint32_t)bin_width;
            
        } else if (strncmp(token, "bn=", 3) == 0) {
            unsigned long bin_count = strtoul(token + 3, NULL, 10);
            
            /* Validate bin count range */
            if (bin_count < 10 || bin_count > 10000) {
                HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Bin count must be between 10 and 10000");
                free(arg_copy);
                exit(EXIT_FAILURE);
            }
            
            config->max_bins = (uint32_t)bin_count;
            
        } else {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Unknown stats parameter '%s' (use bw= or bn=)", token);
            free(arg_copy);
            exit(EXIT_FAILURE);
        }
    }
    
    free(arg_copy);
}

/** Parse log level from string */
hw_log_level_t parse_log_level(const char* level_str) {
    if (strcmp(level_str, "DEBUG") == 0) return HW_LOG_LEVEL_DEBUG;
    else if (strcmp(level_str, "INFO") == 0) return HW_LOG_LEVEL_INFO;
    else if (strcmp(level_str, "WARN") == 0) return HW_LOG_LEVEL_WARN;
    else if (strcmp(level_str, "ERROR") == 0) return HW_LOG_LEVEL_ERROR;
    else {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Unsupported log level '%s'. Supported: DEBUG|INFO|WARN|ERROR", level_str);
        exit(EXIT_FAILURE);
    }
}

/* Parse log components from comma-separated string */
void parse_log_components(const char* comp_str) {
    if (!comp_str) return;
    
    /* Disable all components first */
    hw_log_disable_component(HW_LOG_COMPONENT_MAIN);
    hw_log_disable_component(HW_LOG_COMPONENT_CLIENT);
    hw_log_disable_component(HW_LOG_COMPONENT_SERVER);
    hw_log_disable_component(HW_LOG_COMPONENT_STATS);
    hw_log_disable_component(HW_LOG_COMPONENT_CSV);
    hw_log_disable_component(HW_LOG_COMPONENT_NETWORK);
    hw_log_disable_component(HW_LOG_COMPONENT_SIGNAL);
    
    char *comp_copy = strdup(comp_str);
    if (!comp_copy) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to allocate memory for component parsing");
        exit(EXIT_FAILURE);
    }
    
    char *token = strtok(comp_copy, ",");
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';
        
        if (strcmp(token, "MAIN") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_MAIN);
        } else if (strcmp(token, "CLIENT") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_CLIENT);
        } else if (strcmp(token, "SERVER") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_SERVER);
        } else if (strcmp(token, "STATS") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_STATS);
        } else if (strcmp(token, "CSV") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_CSV);
        } else if (strcmp(token, "NETWORK") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_NETWORK);
        } else if (strcmp(token, "SIGNAL") == 0) {
            hw_log_enable_component(HW_LOG_COMPONENT_SIGNAL);
        } else {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Unknown log component '%s'. Supported: MAIN|CLIENT|SERVER|STATS|CSV|NETWORK|SIGNAL", token);
            free(comp_copy);
            exit(EXIT_FAILURE);
        }
        
        token = strtok(NULL, ",");
    }
    
    free(comp_copy);
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage (one-way mode): %s --one-way --rx-interface <interface> --port <port> [OPTIONS]\n", prog_name);
    fprintf(stderr, "Usage (round-trip mode): %s --round-trip --rx-interface <interface> --port <port> --tx-interface <interface> [OPTIONS]\n\n", prog_name);

    fprintf(stderr, "Mode argument (exactly one required):\n");
    fprintf(stderr, "  --one-way                    Only receive packets\n");
    fprintf(stderr, "  --round-trip                 Receive and send return packets\n\n");
    
    fprintf(stderr, "Required arguments:\n");
    fprintf(stderr, "  --rx-interface <interface>   Network interface name for receiving packets\n");
    fprintf(stderr, "  --port <port>                Port number to listen on\n\n");

    fprintf(stderr, "One-way mode options:\n");
    fprintf(stderr, "  --rx-cpu <cpu>               CPU core number for receive operations (default: 4)\n\n");    

    fprintf(stderr, "Round-trip mode options:\n");
    fprintf(stderr, "  --tx-interface <interface>   Network interface name for transmitting return packets (required)\n");
    fprintf(stderr, "  --rx-cpu <cpu>               CPU core number for receive operations (requires --tx-cpu if specified)\n");
    fprintf(stderr, "  --tx-cpu <cpu>               CPU core number for transmit operations (requires --rx-cpu if specified)\n");
    fprintf(stderr, "  --tx-timestamp-cpu <cpu>     CPU core number for TX timestamp processing thread (default: 0)\n");
    fprintf(stderr, "                               Default: rx=4, tx=5\n");

    fprintf(stderr, "Optional arguments:\n");
    fprintf(stderr, "  --time <seconds>             Run for specified number of seconds then exit\n");
    fprintf(stderr, "  --output-csv[=filename]      Enable CSV logging of timestamps to filename\n");
    fprintf(stderr, "  --log-cpu <cpu>              CPU core number for CSV logging thread (requires --output-csv, default: 0)\n");
    fprintf(stderr, "  --stats[=config]             Show timestamp delta latency statistics at program completion\n");
    fprintf(stderr, "                               Format: [max-packets-to-evaluate],[bw=bin-width(usec)],[bn=max-bins]\n");
    fprintf(stderr, "                               Defaults: 5M,10us,1000\n");
    fprintf(stderr, "                               Example: --stats=1M,bw=5,bn=100\n");
    fprintf(stderr, "  --log-level <level>          Set logging level (DEBUG|INFO|WARN|ERROR, default: INFO)\n");
    fprintf(stderr, "  --log-component <component>  Enable specific log components (comma-separated)\n");
    fprintf(stderr, "                               Components: MAIN|CLIENT|SERVER|STATS|CSV|NETWORK|SIGNAL (default: ALL)\n");
    fprintf(stderr, "  --help                       Show this help message\n\n");
}

/*
 * SIGNAL & PROCESS MANAGEMENT
 */

/* Signal handler for graceful shutdown */
void signal_handler(int sig __attribute__((unused))) {
    graceful_shutdown = 1;
    HW_SIGNAL_LOG("Graceful shutdown initiated");
}

/* Setup signal handling for graceful shutdown */
void setup_signal_handling(void) {
    signal(SIGINT, signal_handler);   /* ctl-c */
    signal(SIGTERM, signal_handler);  /* Termination signal */
    HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Signal handling initialized (SIGINT, SIGTERM)");
}

/*
 * TX TIMESTAMP CORRELATION SYSTEM
 */

void cleanup_server_tx_correlation(void);

/* Helper functions for simple array-based TX correlation */
int init_server_tx_correlation(void) {
        
    /* Client IPs */
    server_client_ips = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(char[INET_ADDRSTRLEN]));
    if (!server_client_ips) {
        server_client_ips = calloc(MAX_SEQUENCE_NUMBERS, sizeof(char[INET_ADDRSTRLEN]));
        if (!server_client_ips) {
            cleanup_server_tx_correlation();
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for client IP array (aligned_alloc not available)");
    } else {
        memset(server_client_ips, 0, MAX_SEQUENCE_NUMBERS * sizeof(char[INET_ADDRSTRLEN]));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned client IP array allocation");
    }
    
    /* Client ports */
    server_client_ports = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(int));
    if (!server_client_ports) {
        server_client_ports = calloc(MAX_SEQUENCE_NUMBERS, sizeof(int));
        if (!server_client_ports) {
            cleanup_server_tx_correlation();
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for client ports array (aligned_alloc not available)");
    } else {
        memset(server_client_ports, 0, MAX_SEQUENCE_NUMBERS * sizeof(int));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned client port array allocation");
    }
    
    /* Hardware RX timestamps */
    server_hw_rx_timestamps = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
    if (!server_hw_rx_timestamps) {
        server_hw_rx_timestamps = calloc(MAX_SEQUENCE_NUMBERS, sizeof(struct timespec));
        if (!server_hw_rx_timestamps) {
            cleanup_server_tx_correlation();
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for HW RX timestamps array");
    } else {
        memset(server_hw_rx_timestamps, 0, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned HW RX timestamps array allocation");
    }
    
    /* Kernel RX timestamps */
    server_ker_rx_timestamps = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
    if (!server_ker_rx_timestamps) {
        server_ker_rx_timestamps = calloc(MAX_SEQUENCE_NUMBERS, sizeof(struct timespec));
        if (!server_ker_rx_timestamps) {
            cleanup_server_tx_correlation();
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for kernel RX timestamps array");
    } else {
        memset(server_ker_rx_timestamps, 0, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned kernel RX timestamps array allocation");
    }
    
    /* Application RX timestamps */
    server_app_rx_timestamps = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
    if (!server_app_rx_timestamps) {
        server_app_rx_timestamps = calloc(MAX_SEQUENCE_NUMBERS, sizeof(struct timespec));
        if (!server_app_rx_timestamps) {
            cleanup_server_tx_correlation();
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for app RX timestamps array");
    } else {
        memset(server_app_rx_timestamps, 0, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned app RX timestamps array allocation");
    }
    
    return 0;
}

/* Cleanup server TX correlation arrays */
void cleanup_server_tx_correlation(void) {
    if (server_client_ips) {
        free(server_client_ips);
        server_client_ips = NULL;
    }
    if (server_client_ports) {
        free(server_client_ports);
        server_client_ports = NULL;
    }
    if (server_hw_rx_timestamps) {
        free(server_hw_rx_timestamps);
        server_hw_rx_timestamps = NULL;
    }
    if (server_ker_rx_timestamps) {
        free(server_ker_rx_timestamps);
        server_ker_rx_timestamps = NULL;
    }
    if (server_app_rx_timestamps) {
        free(server_app_rx_timestamps);
        server_app_rx_timestamps = NULL;
    }
}

/* Simple direct array storage (O(1) performance) for correlation data */
static inline void store_server_rx_timestamps(uint32_t seq_num, struct timespec hw_rx_ts, 
                                              struct timespec ker_rx_ts, struct timespec app_rx_ts,
                                              const char *client_ip, int client_port) {
    if (server_hw_rx_timestamps && server_ker_rx_timestamps && server_app_rx_timestamps && 
        server_client_ips && server_client_ports) {
        uint32_t index = get_circular_index(seq_num);
        
        server_hw_rx_timestamps[index] = hw_rx_ts;
        server_ker_rx_timestamps[index] = ker_rx_ts;
        server_app_rx_timestamps[index] = app_rx_ts;
        strncpy(server_client_ips[index], client_ip, INET_ADDRSTRLEN - 1);
        server_client_ips[index][INET_ADDRSTRLEN - 1] = '\0';
        server_client_ports[index] = client_port;
    }
}

/* TX timestamp processing */
int process_tx_timestamps_in_main_thread(int sockfd) {
    if (!tx_csv_buffer && !stats_config.enabled) return 0;
    
    struct msghdr msg;
    char packet_data[64];
    char control_data[256];
    struct iovec iov = {.iov_base = packet_data, .iov_len = sizeof(packet_data)};
    
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_data;
    msg.msg_controllen = sizeof(control_data);
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    
    int processed_timestamps = 0;
    
    /* Process in batches per call */
    for (int i = 0; i < TX_TIMESTAMP_BATCH_SIZE; i++) {
        int ret = recvmsg(sockfd, &msg, MSG_DONTWAIT | MSG_ERRQUEUE);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* No more messages */
            }
            continue;
        }
        
        /* Extract sequence number and timestamp */
        struct cmsghdr *cmsg;
        uint32_t seq_num = 0;
        struct __kernel_timespec kernel_tx_ts = {0};
        
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
                struct sock_extended_err *serr = (struct sock_extended_err *)CMSG_DATA(cmsg);
                /* Extract sequence number from error queue message */
                uint32_t payload_seq = 0;
                uint32_t kernel_seq = serr->ee_data;
                
                if (ret >= 46) {
                    /* Extract from UDP payload */
                    payload_seq = ntohl(*(uint32_t*)(packet_data + 42));
                } else if (ret >= 4) {
                    /* Fallback: try from beginning */
                    payload_seq = ntohl(*(uint32_t*)packet_data);
                }
                
                /* Use payload sequence if available, otherwise fall back to kernel sequence (to be implemented) */
                if (ret >= 4) {
                    seq_num = payload_seq;
                } else {
                    seq_num = kernel_seq;
                    continue;
                }
            } else if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING_NEW) {
                struct scm_timestamping64 *tss = (struct scm_timestamping64 *)CMSG_DATA(cmsg);
                kernel_tx_ts = tss->ts[0];
            }
        }
        
        /* Direct array lookup */
        if (server_client_ips && server_client_ports) {
            uint32_t index = get_circular_index(seq_num);
            char *client_ip = server_client_ips[index];
            int client_port = server_client_ports[index];
            
            /* Conditional CSV processing */
            if (csv_config.csv_enabled && tx_csv_buffer) {
                csv_entry_t csv_entry;
                create_csv_server_tx(&csv_entry, seq_num, client_ip, (uint16_t)client_port,
                                   kernel_tx_ts.tv_sec * 1000000000ULL + kernel_tx_ts.tv_nsec);
                
                /* Enqueue to TX CSV buffer */
                csv_enqueue_fast(tx_csv_buffer, &csv_entry);
                processed_timestamps++;
            }
            
            /* Conditional stats processing */
            if (stats_config.enabled && global_stats_collector) {
                update_stats_buffer_with_tx_timestamp(seq_num, kernel_tx_ts.tv_sec * 1000000000ULL + kernel_tx_ts.tv_nsec, STATS_SERVER_ROUNDTRIP);
            }
        }
    }
    
    return processed_timestamps;
}

/*
 * RX & TX THREAD MANAGEMENT
 */

/* Return packet request structure for inter-thread communication */
typedef struct {
    uint32_t seq_num;
    struct sockaddr_in return_addr;
    char packet_data[8];
    size_t packet_size;
    int original_client_src_port;
} ReturnPacketRequest;

/* Lock-free circular buffer for return packet requests */
typedef struct {
    ReturnPacketRequest *requests;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t size_mask;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} ReturnPacketQueue;

/* TX thread data structure */
typedef struct {
    int tx_sockfd;
    ReturnPacketQueue *tx_queue;
    volatile int *running;
    int tx_cpu;
    
    /* Batch sending optimization structures */
    struct mmsghdr *tx_msgs;
    struct iovec *tx_iovecs;
    char *tx_packet_buffers;
    int max_batch_size;
    
    /* CSV buffer for main CSV writing */
    csv_ring_buffer_t *csv_buffer;
} TxThreadData;

/* Lock-free SPSC queue implementation */
ReturnPacketQueue* create_return_packet_queue(uint32_t size) {
    if (size == 0 || (size & (size - 1)) != 0) {
        size = 4096;
    }
    
    ReturnPacketQueue* queue = aligned_alloc(64, sizeof(ReturnPacketQueue));
    if (!queue) return NULL;
    
    queue->requests = aligned_alloc(64, size * sizeof(ReturnPacketRequest));
    if (!queue->requests) {
        free(queue);
        return NULL;
    }
    
    queue->head = 0;
    queue->tail = 0;
    queue->size_mask = size - 1;
    
    /* Initialize pthread primitives */
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
    
    return queue;
}

/* Cleanup return packet queue */
void destroy_return_packet_queue(ReturnPacketQueue* queue) {
    if (!queue) return;
    
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    
    if (queue->requests) free(queue->requests);
    free(queue);
}

/* Lock-free enqueue */
bool enqueue_return_packet_request(ReturnPacketQueue* queue, const ReturnPacketRequest* req) {
    uint32_t current_tail = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);
    uint32_t next_tail = (current_tail + 1) & queue->size_mask;
    
    if (next_tail == __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE)) {
        return false;
    }
    
    queue->requests[current_tail] = *req;
    
    __atomic_store_n(&queue->tail, next_tail, __ATOMIC_RELEASE);
    
    return true;
}

/* Lock-free dequeue */
bool dequeue_return_packet_request(ReturnPacketQueue* queue, ReturnPacketRequest* req) {
    uint32_t current_head = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);
    
    if (current_head == __atomic_load_n(&queue->tail, __ATOMIC_ACQUIRE)) {
        return false;
    }
    
    *req = queue->requests[current_head];
    
    __atomic_store_n(&queue->head, (current_head + 1) & queue->size_mask, __ATOMIC_RELAXED);
    
    return true;
}

/* Batch dequeue */
int dequeue_return_packet_batch(ReturnPacketQueue* queue, ReturnPacketRequest* reqs, int batch_size) {
    int count = 0;
    
    while (count < batch_size && dequeue_return_packet_request(queue, &reqs[count])) {
        count++;
    }
    
    return count;
}

/* TX thread function */
void* tx_thread_func(void* arg) {
    TxThreadData *tx_data = (TxThreadData*)arg;
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX thread started");
    
    /* Local packet counter for this thread */
    uint64_t local_packets_sent = 0;
        
    /* Dynamic batch sizing based on queue depth and max capacity */
    ReturnPacketRequest batch_requests[BATCH_SIZE];
    
    while (*tx_data->running) {
        /* Adaptive batching - use queue depth to determine optimal batch size */
        uint32_t current_head = __atomic_load_n(&tx_data->tx_queue->head, __ATOMIC_RELAXED);
        uint32_t current_tail = __atomic_load_n(&tx_data->tx_queue->tail, __ATOMIC_ACQUIRE);
        uint32_t queue_depth = (current_tail - current_head) & tx_data->tx_queue->size_mask;
        
        int target_batch_size = queue_depth > 0 ? 
            (int)min((uint32_t)tx_data->max_batch_size, queue_depth) : 1;
        target_batch_size = max(target_batch_size, 1);
        
        /* Batch dequeue with dynamic sizing */
        int batch_count = dequeue_return_packet_batch(tx_data->tx_queue, batch_requests, target_batch_size);
        
        if (batch_count == 0) {
            continue;
        }
        
        /* Generate TX timestamps */
        struct timespec batch_tx_timestamps[BATCH_SIZE];
        for (int i = 0; i < batch_count; i++) {
            batch_tx_timestamps[i] = get_system_time();
        }
        
        /* Prepare all packets in batch */
        for (int i = 0; i < batch_count; i++) {
            ReturnPacketRequest *req = &batch_requests[i];
            
            char *packet_buf = tx_data->tx_packet_buffers + (i * RETURN_PACKET_SIZE);
            size_t packet_size = construct_return_packet(packet_buf, req->packet_data);
            
            tx_data->tx_iovecs[i].iov_len = packet_size;
            
            tx_data->tx_msgs[i].msg_hdr.msg_name = &req->return_addr;
            tx_data->tx_msgs[i].msg_hdr.msg_namelen = sizeof(req->return_addr);
        }
        
        /* Send */
        int sent = sendmmsg(tx_data->tx_sockfd, tx_data->tx_msgs, batch_count, MSG_DONTWAIT);
        
        if (sent > 0) {
            if (sent < batch_count) {
                static time_t last_partial_log = 0;
                static uint32_t partial_send_count = 0;
                time_t now = time(NULL);
                
                __atomic_fetch_add(&partial_send_count, 1, __ATOMIC_RELAXED);
                
                if (now > last_partial_log) {
                    last_partial_log = now;
                }
            }
            
            /* Update counter for successful sends */
            local_packets_sent += sent;
            __atomic_fetch_add(&g_packets_sent, sent, __ATOMIC_RELAXED);
            
            /* Process correlation */
            for (int i = 0; i < sent; i++) {
                ReturnPacketRequest *req = &batch_requests[i];
                
                /* Conditional stats processing */
                if (stats_config.enabled && global_stats_collector) {
                    update_stats_buffer_with_app_tx_timestamp(req->seq_num, 
                        batch_tx_timestamps[i].tv_sec * 1000000000ULL + batch_tx_timestamps[i].tv_nsec,
                        0, 
                        STATS_SERVER_ROUNDTRIP);
                }
                
                /* Conditional CSV processing */
                if (csv_config.csv_enabled && tx_data->csv_buffer && server_hw_rx_timestamps && server_ker_rx_timestamps && 
                    server_app_rx_timestamps && server_client_ips && server_client_ports) {
                    
                    uint32_t index = get_circular_index(req->seq_num);
                    
                    /* Convert timestamps to nanoseconds */
                    uint64_t hw_rx_ns = server_hw_rx_timestamps[index].tv_sec * 1000000000ULL + server_hw_rx_timestamps[index].tv_nsec;
                    uint64_t ker_rx_ns = server_ker_rx_timestamps[index].tv_sec * 1000000000ULL + server_ker_rx_timestamps[index].tv_nsec;
                    uint64_t app_rx_ns = server_app_rx_timestamps[index].tv_sec * 1000000000ULL + server_app_rx_timestamps[index].tv_nsec;
                    uint64_t app_tx_ns = batch_tx_timestamps[i].tv_sec * 1000000000ULL + batch_tx_timestamps[i].tv_nsec;
                    
                    csv_entry_t csv_entry;
                    create_csv_server_roundtrip_main(&csv_entry, req->seq_num, server_client_ips[index], 
                                                   (uint16_t)req->original_client_src_port, 
                                                   hw_rx_ns, ker_rx_ns, app_rx_ns, app_tx_ns);
                    
                    /* Enqueue to CSV buffer */
                    csv_enqueue_fast(tx_data->csv_buffer, &csv_entry);
                }
            }
            
        } else if (sent == 0) {
            static time_t last_zero_log = 0;
            static uint32_t zero_send_count = 0;
            time_t now = time(NULL);
            
            __atomic_fetch_add(&zero_send_count, 1, __ATOMIC_RELAXED);
            
            if (now > last_zero_log + 5) {
                last_zero_log = now;
            }
            
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
                
            } else {
                static time_t last_fallback_log = 0;
                static uint32_t fallback_count = 0;
                time_t now = time(NULL);
                
                __atomic_fetch_add(&fallback_count, 1, __ATOMIC_RELAXED);
                
                if (errno != EINTR && now > last_fallback_log) {
                    last_fallback_log = now;
                }
                
                /* Fallback to sendto() */
                int fallback_sent = 0;
                for (int i = 0; i < batch_count; i++) {
                    ReturnPacketRequest *req = &batch_requests[i];
                    
                    char *packet_buf = tx_data->tx_packet_buffers + (i * RETURN_PACKET_SIZE);
                    size_t packet_size = tx_data->tx_iovecs[i].iov_len;
                    
                    ssize_t individual_sent = sendto(tx_data->tx_sockfd, packet_buf, packet_size, MSG_DONTWAIT,
                                                   (struct sockaddr*)&req->return_addr, sizeof(req->return_addr));
                    
                    if (individual_sent > 0) {
                        fallback_sent++;
                    }
                }
                
                if (fallback_sent > 0) {
                    local_packets_sent += fallback_sent;
                    __atomic_fetch_add(&g_packets_sent, fallback_sent, __ATOMIC_RELAXED);
                    
                    if (now > last_fallback_log) {
                    }
                }
            }
        }
        
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX thread terminated, sent %llu packets", 
                 (unsigned long long)local_packets_sent);
    return (void*)(uintptr_t)local_packets_sent;
}

/* Return packet construction */
size_t construct_return_packet(char *return_buffer, const char *original_packet) {
    size_t offset = 0;
    
    memcpy(return_buffer + offset, original_packet, 4);
    offset += 4;
    
    return offset;
}

/*
 * MAIN ENTRY POINT
 */

int main(int argc, char *argv[]) {
    const char *rx_if_name = NULL;
    const char *tx_if_name = NULL;
    int port = 0;
    int one_way_mode = 0;
    int round_trip_mode = 0;
    int rx_cpu = 4;  /* Default RX CPU core */
    int tx_cpu = 5;  /* Default TX CPU core */
    
    static struct option long_options[] = {
        {"rx-interface", required_argument, 0, 'i'},
        {"port", required_argument, 0, 'p'},
        {"output-csv", optional_argument, 0, 'C'},
        {"log-cpu", required_argument, 0, 'L'},
        {"one-way", no_argument, 0, 'o'},
        {"round-trip", no_argument, 0, 'r'},
        {"tx-interface", required_argument, 0, 't'},
        {"rx-cpu", required_argument, 0, 'x'},
        {"tx-cpu", required_argument, 0, 'y'},
        {"time", required_argument, 0, 'T'},
        {"stats", optional_argument, 0, 'S'},
        {"log-level", required_argument, 0, 'l'},
        {"log-component", required_argument, 0, 'c'},
        {"tx-timestamp-cpu", required_argument, 0, 'X'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "i:p:C::L:ort:x:y:T:S::l:c:X:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                rx_if_name = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'C':
                csv_config.csv_enabled = 1;
                if (optarg) {
                    strncpy(csv_config.csv_filename, optarg, sizeof(csv_config.csv_filename) - 1);
                    csv_config.csv_filename[sizeof(csv_config.csv_filename) - 1] = '\0';
                } else {
                    snprintf(csv_config.csv_filename, sizeof(csv_config.csv_filename), 
                             "server_timestamps_%d.csv", getpid());
                }
                const char *dot_pos_csv = strrchr(csv_config.csv_filename, '.');
                if (dot_pos_csv) {
                    snprintf(csv_config.tx_csv_filename, sizeof(csv_config.tx_csv_filename), 
                             "%.*s_tx%s", (int)(dot_pos_csv - csv_config.csv_filename), 
                             csv_config.csv_filename, dot_pos_csv);
                } else {
                    snprintf(csv_config.tx_csv_filename, sizeof(csv_config.tx_csv_filename), 
                             "%s_tx.csv", csv_config.csv_filename);
                }
                break;
            case 'L':
                csv_config.log_cpu = atoi(optarg);
                if (csv_config.log_cpu < 0) {
                    fprintf(stderr, "Error: --log-cpu must be >= 0\n");
                    return EXIT_FAILURE;
                }
                break;
            case 'o':
                one_way_mode = 1;
                break;
            case 'r':
                round_trip_mode = 1;
                break;
            case 't':
                tx_if_name = optarg;
                break;
            case 'x':
                rx_cpu = atoi(optarg);
                break;
            case 'y':
                tx_cpu = atoi(optarg);
                break;
            case 'T':
                time_seconds = atoi(optarg);
                break;
            case 'S':
                stats_config.enabled = 1;
                if (optarg) {
                    parse_stats_argument(optarg, &stats_config);
                } else {
                    stats_config.buffer_size = 5000000;        /* 5M packet entries default */
                    stats_config.bin_width_us = 10;            /* 10us bin widths default */
                    stats_config.max_bins = 1000;              /* 1000 bins default */
                }
                break;
            case 'l':
                hw_log_set_level(parse_log_level(optarg));
                break;
            case 'c':
                parse_log_components(optarg);
                break;
            case 'X':
                tx_timestamp_cpu = atoi(optarg);
                if (tx_timestamp_cpu < 0) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Error: --tx-timestamp-cpu must be >= 0");
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
            case '?':
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    
    /* Validate flag argument dependencies */
    if (csv_config.log_cpu != 0 && !csv_config.csv_enabled) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--log-cpu can only be used with --output-csv");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    
    if (!rx_if_name || port == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--rx-interface and --port are required");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (time_seconds < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--time value must be positive");
        exit(EXIT_FAILURE);
    }
    
    if (one_way_mode && round_trip_mode) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Cannot specify both --one-way and --round-trip");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (!one_way_mode && !round_trip_mode) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Must specify either --one-way or --round-trip");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (round_trip_mode && !tx_if_name) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--round-trip requires --tx-interface");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (port <= 0 || port > 65535) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Port must be between 1 and 65535");
        exit(EXIT_FAILURE);
    }

    if (one_way_mode) {
        int tx_cpu_specified = (tx_cpu != 5);
        if (tx_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--tx-cpu is not supported in --one-way mode");
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
        int tx_timestamp_cpu_specified = (tx_timestamp_cpu != 0);
        if (tx_timestamp_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--tx-timestamp-cpu is not supported in --one-way mode");
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }   
    } else if (round_trip_mode) {
        int rx_cpu_specified = (rx_cpu != 4);
        int tx_cpu_specified = (tx_cpu != 5);
        
        if (rx_cpu_specified && !tx_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "In round-trip mode, if --rx-cpu is specified, --tx-cpu must also be specified");
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
        if (tx_cpu_specified && !rx_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "In round-trip mode, if --tx-cpu is specified, --rx-cpu must also be specified");
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    
    /* Initialize console logging system */
    hw_log_init();
    
    /* Setup signal handling for graceful shutdown */
    setup_signal_handling();
    
    /* Initialize stats system if enabled */
    if (stats_config.enabled) {
        stats_mode_type_t mode = one_way_mode ? STATS_SERVER_ONEWAY : STATS_SERVER_ROUNDTRIP;
        global_stats_collector = create_stats_collector(stats_config.buffer_size, mode);
        if (!global_stats_collector) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to initialize statistics system");
            return EXIT_FAILURE;
        }
    }
    
    /* Dispatch to mode-specific implementations */
    if (one_way_mode) {
        return run_server_oneway(rx_if_name, port, rx_cpu);
    } else {
        return run_server_roundtrip(rx_if_name, tx_if_name, port, rx_cpu, tx_cpu);
    }
}

/*
 * CORE EXECUTION ENGINES
 */

/* One-way mode implementation */
int run_server_oneway(const char *rx_if_name, int port, int rx_cpu) {
    long long program_duration_ns = 0;
    if (time_seconds > 0) {
        program_duration_ns = (long long)time_seconds * 1000000000LL;
    }

    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Server configuration:");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "RX Interface: %s", rx_if_name);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Port: %d", port);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "RX CPU: %d", rx_cpu);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "One-way mode");
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "RX socket setup");
    
    /* Create RX UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Socket creation failed");
        return EXIT_FAILURE;
    }
    
    /* Apply performance optimizations to RX socket */
    if (optimize_socket_performance(sockfd, rx_cpu, 0) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Configure hardware timestamping on the RX interface */
    struct ifreq hwtstamp_ifreq;
    struct hwtstamp_config hwconfig;
    memset(&hwtstamp_ifreq, 0, sizeof(hwtstamp_ifreq));
    memset(&hwconfig, 0, sizeof(hwconfig));
    strncpy(hwtstamp_ifreq.ifr_name, rx_if_name, IFNAMSIZ-1);
    hwconfig.tx_type = HWTSTAMP_TX_OFF;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
    hwtstamp_ifreq.ifr_data = (void*)&hwconfig;
    
    if (ioctl(sockfd, SIOCSHWTSTAMP, &hwtstamp_ifreq) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Hardware timestamping not supported on %s: %s", 
                rx_if_name, strerror(errno));
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Continuing with software timestamping only");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "RX hardware timestamping enabled on %s", rx_if_name);
    }

    /* Set up RX socket timestamping */
    if (setup_timestamping(sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to setup RX socket timestamping");
        close(sockfd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Bind RX socket to specific interface */
    struct ifreq if_opts;
    memset(&if_opts, 0, sizeof(if_opts));
    strncpy(if_opts.ifr_name, rx_if_name, IFNAMSIZ-1);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &if_opts, sizeof(if_opts)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "RX SO_BINDTODEVICE failed");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Bind to port */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "RX socket bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    /* Conditional CSV buffer creation */
    csv_ring_buffer_t *csv_buffer = NULL;
    if (csv_config.csv_enabled) {
        csv_type_t server_csv_type = CSV_SERVER_MAIN_ONEWAY;
        csv_buffer = csv_buffer_create(
            1048576,
            csv_config.csv_filename,
            server_csv_type,
            10000,
            csv_config.log_cpu
        );
        if (!csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to create high-performance CSV buffer");
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "CSV logging initialized");
    } else {
    }

    /* Setup signal-based PPS reporting */
    setup_stats_reporting_hotpath();

    char packet_buffer[MAX_PACKET_SIZE];
    char ctrl_buffer[CMSG_BUFFER_SIZE];
    struct sockaddr_in src_addr;
    struct iovec iov = { .iov_base = packet_buffer, .iov_len = MAX_PACKET_SIZE };
    struct msghdr msg = {
        .msg_name = &src_addr,
        .msg_namelen = sizeof(src_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buffer,
        .msg_controllen = sizeof(ctrl_buffer),
        .msg_flags = 0
    };
    
    /* Optimize process scheduling and CPU affinity */
    optimize_process_scheduling(rx_cpu);
    
    /* Calibrate CPU frequency for precise timing (on bound RX CPU) */
    calibrate_cpu_freq();
    
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Listening in one-way mode");
    printf("\n");

    /* Initialize performance tracking */ 
    long long start_time = monotonic_time_ns();
    uint64_t total_packets_received = 0;

    /* Hybrid time checking with minimal hot path impact */
    uint64_t loop_counter = 0;
    uint64_t last_time_check_cycles = rdtsc();
    uint64_t max_cycles_between_checks = (uint64_t)(cpu_freq_ghz * 1e9 * 0.1);

    /* Pre-allocate and reuse packet data structure */
    RoundTripData pkt_data;
    
    while (1) {
        /* Check for graceful shutdown signal */
        if (graceful_shutdown) {
            break;
        }
        loop_counter++;
        
        /* Hybrid time check - whichever condition hits first */
        uint64_t cycles_since_check = rdtsc() - last_time_check_cycles;
        bool should_check_time = (loop_counter % MAX_ITERATION_CHECK_INTERVAL == 0) || 
                                (cycles_since_check >= max_cycles_between_checks);
        
        if (should_check_time) {
            if (program_duration_ns > 0) {
                long long current_time = monotonic_time_ns();
                if ((current_time - start_time) >= program_duration_ns) {
                    printf("\n");
                    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Finished run time");
                    printf("\n");
                    break;
                }
                last_time_check_cycles = rdtsc();
            }
            
            /* Display stats if ready */
            display_stats_if_ready();
        }
        
        ssize_t num_bytes = recvmsg(sockfd, &msg, MSG_DONTWAIT);
        if (num_bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "sendmmsg failed");
            break;
        }

        /* Fast structure reset (initialize all timestamp fields) */
        memset(&pkt_data, 0, sizeof(pkt_data));
        pkt_data.seq_num = -1;
        
        /* Capture application RX timestamp immediately after recvmsg() */
        pkt_data.svr_app_rx_ts = get_app_timestamp();

        /* Extract timestamps from control messages */
        extract_rx_timestamps(&msg, &pkt_data.svr_hw_rx_ts, &pkt_data.svr_ker_rx_ts);

        /* Extract sequence number */
        if (num_bytes >= 4) {
            pkt_data.seq_num = ntohl(*(uint32_t *)packet_buffer);
        }

        /* Extract client source IP and port */
        char src_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip_str, INET_ADDRSTRLEN);
        int client_src_port = ntohs(src_addr.sin_port);
        
        /* Update local counter for final execution details */
        total_packets_received++;
        
        /* Update atomic counter for signal-based PPS reporting */
        __atomic_fetch_add(&g_packets_received, 1, __ATOMIC_RELAXED);
        
        /* Convert timestamps to nanoseconds */
        uint64_t hw_rx_ns = pkt_data.svr_hw_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_hw_rx_ts.tv_nsec;
        uint64_t ker_rx_ns = pkt_data.svr_ker_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_ker_rx_ts.tv_nsec;
        uint64_t app_rx_ns = pkt_data.svr_app_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_app_rx_ts.tv_nsec;
        
        /* Conditional CSV processing */
        if (csv_config.csv_enabled && csv_buffer) {
            csv_entry_t csv_entry;
            create_csv_server_oneway_main(&csv_entry, pkt_data.seq_num, src_ip_str,
                                         (uint16_t)client_src_port, hw_rx_ns, ker_rx_ns, app_rx_ns);
            
            /* Enqueue to CSV buffer */
            csv_enqueue_fast(csv_buffer, &csv_entry);
        }

        /* Conditional stats processing */
        if (stats_config.enabled && global_stats_collector) {
            create_minimal_stats_entry(pkt_data.seq_num, client_src_port, src_ip_str,
                TIMESTAMP_MODE_SERVER_ONEWAY, STATS_SERVER_ONEWAY);
            
            update_stats_buffer_with_rx_timestamps(pkt_data.seq_num,
                hw_rx_ns, ker_rx_ns, app_rx_ns, 0, STATS_SERVER_ONEWAY);
        }        
    }
    
    /* Cleanup stats system */
    if (stats_config.enabled && global_stats_collector) {
        if (buffer_has_data(global_stats_collector)) {
            stats_analysis_result_t *result = calloc(1, sizeof(stats_analysis_result_t));
            if (result) {
                stats_mode_type_t mode = (stats_mode_type_t)global_stats_collector->program_mode;
                if (initialize_analysis_result(result, mode, &stats_config) == 0) {
                    process_buffer_for_analysis(global_stats_collector, result);
                    display_analysis_results(result, 0, total_packets_received);
                    cleanup_analysis_result(result);
                }
                free(result);
            }
        }
        destroy_stats_collector(global_stats_collector);
        global_stats_collector = NULL;
    } else if (global_stats_collector) {
        destroy_stats_collector(global_stats_collector);
        global_stats_collector = NULL;
    }
    
    /* Final execution details */  
    printf("\n");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "EXECUTION DETAILS");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "=================");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Total packets received: %llu", (unsigned long long)total_packets_received);
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Timestamps CSV filename: %s", csv_config.csv_filename);
    }

    /* Close RX socket */
    close(sockfd);

    /* Cleanup and flush CSV buffer */
    if (csv_buffer) {
        csv_buffer_destroy(csv_buffer);
    }
    
    /* Cleanup signal-based PPS reporting */
    cleanup_stats_reporting_hotpath();
        
    return 0;
}

/* Round-trip mode */
int run_server_roundtrip(const char *rx_if_name, const char *tx_if_name, int port, 
                        int rx_cpu, int tx_cpu) {
    
    long long program_duration_ns = 0;
    if (time_seconds > 0) {
        program_duration_ns = (long long)time_seconds * 1000000000LL;
    }
    
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Server configuration:");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "RX Interface: %s", rx_if_name);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Port: %d", port);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "TX CPU: %d, RX CPU: %d", tx_cpu, rx_cpu);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Round-trip mode");
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX socket setup");
    
    /* Create RX UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "RX Socket creation failed");
        return EXIT_FAILURE;
    }
    
    /* Create TX UDP socket */
    int tx_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "TX Socket creation failed");
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    /* Configure TX socket */
    struct ifreq tx_if_opts;
    memset(&tx_if_opts, 0, sizeof(tx_if_opts));
    strncpy(tx_if_opts.ifr_name, tx_if_name, IFNAMSIZ-1);
    
    /* Bind TX socket to specific interface */
    if (setsockopt(tx_sockfd, SOL_SOCKET, SO_BINDTODEVICE, &tx_if_opts, sizeof(tx_if_opts)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "TX SO_BINDTODEVICE failed");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    
    /* Apply performance optimizations to TX socket */
    if (optimize_socket_performance(tx_sockfd, tx_cpu, 1) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to optimize TX socket");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }

    /* Apply performance optimizations to RX socket */
    if (optimize_socket_performance(sockfd, rx_cpu, 0) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to optimize RX socket");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
        
    /* Setup timestamping on TX socket */
    if (setup_tx_timestamping(tx_sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to setup TX socket timestamping");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX timestamping enabled on socket");
    
    /* Conditional TX CSV buffer creation */
    if (csv_config.csv_enabled) {
        tx_csv_buffer = csv_buffer_create(
            1048576,
            csv_config.tx_csv_filename,
            CSV_SERVER_TX,
            10000,
            csv_config.log_cpu
        );
        if (!tx_csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to create high-performance TX CSV buffer");
            close(sockfd);
            close(tx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX CSV logging initialized: %s", csv_config.tx_csv_filename);
    } else {
        tx_csv_buffer = NULL;
    }
    
    /* Initialize TX correlation system */
    if (init_server_tx_correlation() < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to initialize TX correlation system");
        csv_buffer_destroy(tx_csv_buffer);
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX correlation system initialized");
    
    /* Store socket for background processing */
    tx_sockfd_global = tx_sockfd;
    
    /* Start dedicated TX timestamp processing thread */
    pthread_t tx_timestamp_thread;
    tx_timestamp_thread_data_t *tx_timestamp_thread_data = NULL;
    
    if (start_tx_timestamp_processing_thread(tx_sockfd, tx_timestamp_cpu, &tx_timestamp_thread, &tx_timestamp_thread_data) != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to start TX timestamp processing thread for server round-trip mode");
        csv_buffer_destroy(tx_csv_buffer);
        cleanup_server_tx_correlation();
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX timestamp processing thread started");

    /* Configure hardware timestamping on the RX interface */
    struct ifreq hwtstamp_ifreq;
    struct hwtstamp_config hwconfig;
    memset(&hwtstamp_ifreq, 0, sizeof(hwtstamp_ifreq));
    memset(&hwconfig, 0, sizeof(hwconfig));
    strncpy(hwtstamp_ifreq.ifr_name, rx_if_name, IFNAMSIZ-1);
    hwconfig.tx_type = HWTSTAMP_TX_OFF;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
    hwtstamp_ifreq.ifr_data = (void*)&hwconfig;
    
    if (ioctl(sockfd, SIOCSHWTSTAMP, &hwtstamp_ifreq) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Hardware timestamping not supported on %s: %s", 
                rx_if_name, strerror(errno));
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Continuing with software timestamping only");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Hardware timestamping enabled on %s", rx_if_name);
    }

    /* Set up RX socket timestamping */
    if (setup_timestamping(sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to setup RX socket timestamping");
        close(sockfd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Bind RX socket to specific interface */
    struct ifreq if_opts;
    memset(&if_opts, 0, sizeof(if_opts));
    strncpy(if_opts.ifr_name, rx_if_name, IFNAMSIZ-1);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &if_opts, sizeof(if_opts)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "RX SO_BINDTODEVICE failed");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    
    /* Bind to port */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "RX Socket bind failed");
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }

    /* Conditional CSV buffer creation */
    csv_ring_buffer_t *csv_buffer = NULL;
    if (csv_config.csv_enabled) {
        csv_type_t server_csv_type = CSV_SERVER_MAIN_ROUNDTRIP;
        csv_buffer = csv_buffer_create(
            1048576,
            csv_config.csv_filename,
            server_csv_type,
            10000,
            csv_config.log_cpu
        );
        if (!csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to create high-performance CSV buffer");
            close(sockfd);
            close(tx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "CSV logging initialized");
    } else {
    }

    /* Setup signal-based PPS reporting */
    setup_stats_reporting_hotpath();

    /* Buffers and structures */
    char packet_buffer[MAX_PACKET_SIZE];
    char ctrl_buffer[CMSG_BUFFER_SIZE];
    
    struct sockaddr_in src_addr;
    struct iovec iov = { .iov_base = packet_buffer, .iov_len = MAX_PACKET_SIZE };
    struct msghdr msg = {
        .msg_name = &src_addr,
        .msg_namelen = sizeof(src_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buffer,
        .msg_controllen = sizeof(ctrl_buffer),
        .msg_flags = 0
    };
    
    /* Thread management */
    pthread_t tx_thread = 0;
    volatile int tx_thread_running = 0;
    ReturnPacketQueue *tx_queue = NULL;
    TxThreadData tx_thread_data = {0};
    uint64_t total_packets_received = 0;        /* Owned by RX thread */
    volatile uint64_t total_packets_sent = 0;   /* Owned by TX thread */
    
    /* Create lock-free return packet queue */
    tx_queue = create_return_packet_queue(4096);
    if (!tx_queue) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to create TX queue");
        csv_buffer_destroy(csv_buffer);
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    
    /* Setup TX thread data */
    tx_thread_data.tx_sockfd = tx_sockfd;
    tx_thread_data.tx_queue = tx_queue;
    tx_thread_data.running = &tx_thread_running;
    tx_thread_data.tx_cpu = tx_cpu;
    tx_thread_data.max_batch_size = BATCH_SIZE;
    tx_thread_data.csv_buffer = csv_buffer;
    
    /* TX Messages allocation with fallback */
    tx_thread_data.tx_msgs = aligned_alloc(64, BATCH_SIZE * sizeof(struct mmsghdr));
    if (!tx_thread_data.tx_msgs) {
        tx_thread_data.tx_msgs = calloc(BATCH_SIZE, sizeof(struct mmsghdr));
        if (!tx_thread_data.tx_msgs) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to allocate TX msgs structures (both aligned and standard allocation failed)");
            destroy_return_packet_queue(tx_queue);
            csv_buffer_destroy(csv_buffer);
            close(sockfd);
            close(tx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using standard allocation for msgs (aligned_alloc not available)");
    } else {
        memset(tx_thread_data.tx_msgs, 0, BATCH_SIZE * sizeof(struct mmsghdr));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned msgs allocation");
    }
    
    /* IO vectors allocation with fallback */
    tx_thread_data.tx_iovecs = aligned_alloc(64, BATCH_SIZE * sizeof(struct iovec));
    if (!tx_thread_data.tx_iovecs) {
        tx_thread_data.tx_iovecs = calloc(BATCH_SIZE, sizeof(struct iovec));
        if (!tx_thread_data.tx_iovecs) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to allocate TX iovecs structures (both aligned and standard allocation failed)");
            free(tx_thread_data.tx_msgs);
            destroy_return_packet_queue(tx_queue);
            csv_buffer_destroy(csv_buffer);
            close(sockfd);
            close(tx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for iovecs (aligned_alloc not available)");
    } else {
        memset(tx_thread_data.tx_iovecs, 0, BATCH_SIZE * sizeof(struct iovec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned iovecs allocation");
    }
    
    /* Pre-allocate packet buffers for batch sending with fallback */
    tx_thread_data.tx_packet_buffers = aligned_alloc(64, BATCH_SIZE * RETURN_PACKET_SIZE);
    if (!tx_thread_data.tx_packet_buffers) {
        tx_thread_data.tx_packet_buffers = calloc(BATCH_SIZE, RETURN_PACKET_SIZE);
        if (!tx_thread_data.tx_packet_buffers) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to allocate TX packet buffers (both aligned and standard allocation failed)");
            free(tx_thread_data.tx_msgs);
            free(tx_thread_data.tx_iovecs);
            destroy_return_packet_queue(tx_queue);
            csv_buffer_destroy(csv_buffer);
            close(sockfd);
            close(tx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Using standard allocation for packet buffers (aligned_alloc not available)");
    } else {
        memset(tx_thread_data.tx_packet_buffers, 0, BATCH_SIZE * RETURN_PACKET_SIZE);
        HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "Using 64-byte aligned packet buffer allocation");
    }
    
    /* Initialize message structures (no control messages - let kernel auto-assign correlation IDs) */
    for (int i = 0; i < BATCH_SIZE; i++) {
        tx_thread_data.tx_iovecs[i].iov_base = tx_thread_data.tx_packet_buffers + (i * RETURN_PACKET_SIZE);
        tx_thread_data.tx_iovecs[i].iov_len = RETURN_PACKET_SIZE;
        
        tx_thread_data.tx_msgs[i].msg_hdr.msg_iov = &tx_thread_data.tx_iovecs[i];
        tx_thread_data.tx_msgs[i].msg_hdr.msg_iovlen = 1;
        tx_thread_data.tx_msgs[i].msg_hdr.msg_control = NULL;
        tx_thread_data.tx_msgs[i].msg_hdr.msg_controllen = 0;
        tx_thread_data.tx_msgs[i].msg_hdr.msg_name = NULL;
        tx_thread_data.tx_msgs[i].msg_hdr.msg_namelen = 0;
    }
    
    /* Create TX thread with real-time priority */
    tx_thread_running = 1;

    if (create_realtime_thread(&tx_thread, tx_thread_func, &tx_thread_data, tx_cpu, 99, "Server TX") != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "Failed to create realtime TX thread");
        tx_thread_running = 0;
        destroy_return_packet_queue(tx_queue);
        csv_buffer_destroy(csv_buffer);
        close(sockfd);
        close(tx_sockfd);
        return EXIT_FAILURE;
    }
    
    /* Optimize process scheduling and CPU affinity */
    optimize_process_scheduling(rx_cpu);
    
    /* Calibrate CPU frequency for precise timing (on bound RX CPU) */
    calibrate_cpu_freq();
        
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Listening in round-trip mode");
    printf("\n");

    /* Initialize performance tracking */ 
    long long start_time = monotonic_time_ns();

    /* Hybrid time checking with minimal hot path impact */
    uint64_t loop_counter_rt = 0;
    uint64_t last_time_check_cycles_rt = rdtsc();
    uint64_t max_cycles_between_checks_rt = (uint64_t)(cpu_freq_ghz * 1e9 * 0.1);

    /* Pre-allocate and reuse packet data structure */
    RoundTripData pkt_data;

    while (1) {
        /* Check for graceful shutdown signal */
        if (graceful_shutdown) {
            break;
        }
        loop_counter_rt++;
        
        /* Hybrid time check - whichever condition hits first */
        uint64_t cycles_since_check_rt = rdtsc() - last_time_check_cycles_rt;
        bool should_check_time_rt = (loop_counter_rt % MAX_ITERATION_CHECK_INTERVAL == 0) || 
                                   (cycles_since_check_rt >= max_cycles_between_checks_rt);
        
        if (should_check_time_rt) {
            long long current_time = 0;
            
            if (program_duration_ns > 0) {
                current_time = monotonic_time_ns();
                if ((current_time - start_time) >= program_duration_ns) {
                    printf("\n");
                    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Finished run time");
                    printf("\n");
                    break;
                }
                last_time_check_cycles_rt = rdtsc();
                
            }
            
            /* Display stats if ready */
            display_stats_if_ready();
        }
        
        ssize_t num_bytes = recvmsg(sockfd, &msg, MSG_DONTWAIT);
        if (num_bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            HW_LOG_ERROR(HW_LOG_COMPONENT_SERVER, "sendmmsg failed");
            break;
        }

        /* Fast structure reset (initialize all timestamp fields) */
        pkt_data.seq_num = -1;
        
        /* Capture application RX timestamp immediately after recvmsg() */
        pkt_data.svr_app_rx_ts = get_app_timestamp();

        /* Extract timestamps from control messages */
        extract_rx_timestamps(&msg, &pkt_data.svr_hw_rx_ts, &pkt_data.svr_ker_rx_ts);

        /* Extract sequence number and client RX port for routing return packet */
        int client_rx_port = 0;
        
        if (num_bytes >= 8) {
            pkt_data.seq_num = ntohl(*(uint32_t *)packet_buffer);
            client_rx_port = (int)ntohl(*(uint32_t *)(packet_buffer + 4));
        } else if (num_bytes >= 4) {
            pkt_data.seq_num = ntohl(*(uint32_t *)packet_buffer);
        }

        /* Extract client source IP and port */
        char src_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip_str, INET_ADDRSTRLEN);
        int client_src_port = ntohs(src_addr.sin_port);
        
        /* Update local counter for final execution details */
        __atomic_fetch_add(&g_packets_received, 1, __ATOMIC_RELAXED);
        total_packets_received++;
                
        /* Queue return packet request for TX thread */
        if (num_bytes >= 8 && tx_queue) {
            /* Determine return port - set by client for RX */
            int return_port = (client_rx_port > 0) ? client_rx_port : client_src_port;
            
            /* Create return packet request for TX thread */
            ReturnPacketRequest tx_request = {0};
            tx_request.seq_num = pkt_data.seq_num;
            tx_request.return_addr.sin_family = AF_INET;
            tx_request.return_addr.sin_addr = src_addr.sin_addr;
            tx_request.return_addr.sin_port = htons(return_port);
            tx_request.original_client_src_port = client_src_port;
            memcpy(tx_request.packet_data, packet_buffer, min(8, num_bytes));
            tx_request.packet_size = num_bytes;
            
            /* Enqueue for TX thread (lock-free) */
            if (!enqueue_return_packet_request(tx_queue, &tx_request)) {
            }
        }
        
        /* Prepare packet data for round-trip CSV formatting */
        strncpy(pkt_data.clt_src_ip, src_ip_str, INET_ADDRSTRLEN-1);
        pkt_data.clt_src_port = client_src_port;
        
        /* Store RX timestamps in correlation arrays */
        struct timespec hw_rx_ts = { .tv_sec = pkt_data.svr_hw_rx_ts.tv_sec, .tv_nsec = pkt_data.svr_hw_rx_ts.tv_nsec };
        struct timespec ker_rx_ts = { .tv_sec = pkt_data.svr_ker_rx_ts.tv_sec, .tv_nsec = pkt_data.svr_ker_rx_ts.tv_nsec };
        struct timespec app_rx_ts = { .tv_sec = pkt_data.svr_app_rx_ts.tv_sec, .tv_nsec = pkt_data.svr_app_rx_ts.tv_nsec };
        store_server_rx_timestamps(pkt_data.seq_num, hw_rx_ts, ker_rx_ts, app_rx_ts, src_ip_str, client_src_port);
        
        /* Process RX timestamps for stats immediately */
        if (stats_config.enabled && global_stats_collector) {
            create_minimal_stats_entry(pkt_data.seq_num, client_src_port, src_ip_str,
                TIMESTAMP_MODE_SERVER_ROUNDTRIP, STATS_SERVER_ROUNDTRIP);
            
            update_stats_buffer_with_rx_timestamps(pkt_data.seq_num,
                pkt_data.svr_hw_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_hw_rx_ts.tv_nsec,
                pkt_data.svr_ker_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_ker_rx_ts.tv_nsec,
                pkt_data.svr_app_rx_ts.tv_sec * 1000000000ULL + pkt_data.svr_app_rx_ts.tv_nsec,
                0,
                STATS_SERVER_ROUNDTRIP);
        }   
    }

    /* Stop TX timestamp processing thread first */
    stop_tx_timestamp_processing_thread(tx_timestamp_thread, tx_timestamp_thread_data);

    /* Process any remaining TX timestamps in the error queue */
    if (graceful_shutdown) {
        if (tx_sockfd_global >= 0) {
            int total_processed = 0;
            int batch_processed;
            do {
                batch_processed = process_tx_timestamps_in_main_thread(tx_sockfd_global);
                total_processed += batch_processed;
            } while (batch_processed > 0);
        }
    }
    
    /* Stop TX thread */
    if (tx_thread_running) {
        tx_thread_running = 0;
        
        /*  Give thread a moment to finish gracefully */
        usleep(100000);
        
        /* Wait for thread to complete */
        void *tx_result = NULL;
        if (pthread_join(tx_thread, &tx_result) == 0) {
            total_packets_sent = (uint64_t)(uintptr_t)tx_result;
            HW_LOG_DEBUG(HW_LOG_COMPONENT_SERVER, "TX thread returned %llu packets sent", 
                         (unsigned long long)total_packets_sent);
        } else {
            HW_LOG_WARN(HW_LOG_COMPONENT_SERVER, "Failed to join TX thread - TX packet count unavailable");
            total_packets_sent = 0;
        }
    }
    
    /* Cleanup stats system */
    if (stats_config.enabled && global_stats_collector) {
        if (buffer_has_data(global_stats_collector)) {
            stats_analysis_result_t *result = calloc(1, sizeof(stats_analysis_result_t));
            if (result) {
                stats_mode_type_t mode = (stats_mode_type_t)global_stats_collector->program_mode;
                if (initialize_analysis_result(result, mode, &stats_config) == 0) {
                    process_buffer_for_analysis(global_stats_collector, result);
                    display_analysis_results(result, total_packets_received, total_packets_sent);
                    cleanup_analysis_result(result);
                }
                free(result);
            }
        }
        destroy_stats_collector(global_stats_collector);
        global_stats_collector = NULL;
    } else if (global_stats_collector) {
        destroy_stats_collector(global_stats_collector);
        global_stats_collector = NULL;
    }
    
    /* Final execution details */
    printf("\n");    
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "EXECUTION DETAILS");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "=================");
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Total packets received: %llu", (unsigned long long)total_packets_received);
    HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Total return packets sent: %llu", (unsigned long long)total_packets_sent);
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "Timestamps CSV filename: %s", csv_config.csv_filename);
        HW_LOG_INFO(HW_LOG_COMPONENT_SERVER, "TX timestamps CSV filename: %s", csv_config.tx_csv_filename);
    }

    /* Cleanup sendmmsg() structures for TX thread */
    if (tx_thread_data.tx_msgs) {
        free(tx_thread_data.tx_msgs);
        tx_thread_data.tx_msgs = NULL;
    }
    if (tx_thread_data.tx_iovecs) {
        free(tx_thread_data.tx_iovecs);
        tx_thread_data.tx_iovecs = NULL;
    }
    if (tx_thread_data.tx_packet_buffers) {
        free(tx_thread_data.tx_packet_buffers);
        tx_thread_data.tx_packet_buffers = NULL;
    }
    
    /* Close RX and TX sockets */
    close(sockfd);
    close(tx_sockfd);

    /* Cleanup TX queue */
    if (tx_queue) {
        destroy_return_packet_queue(tx_queue);
    }
    
    /* Cleanup server TX correlation system */
    cleanup_server_tx_correlation();
    
    /* Cleanup and flush CSV buffers */
    if (csv_buffer) {
        csv_buffer_destroy(csv_buffer);
    }
    if (tx_csv_buffer) {
        csv_buffer_destroy(tx_csv_buffer);
    }
    
    /* Cleanup signal-based PPS reporting */
    cleanup_stats_reporting_hotpath();
    
    return 0;
}
