/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_client.c
 * 
 * Summary: UDP client for EC2 timestamp latency measurements, to be used with timestamp_server.c.
 * 
 * Description: UDP client supporting one-way and round-trip timestamp measurement modes
 * sub-microsecond precision. Features multi-threaded architecture with dedicated TX/RX
 * processing, hardware/software timestamp collection, TSC-based timing, CPU affinity optimization
 * real-time scheduling, and batch packet transmission using sendmmsg(). Includes TX timestamp correlation 
 * via kernel error queues, lock-free statistics collection with percentiles and CSV logging. 
 * Supports PPS control, signal-safe monitoring and timestamp delta analysis for network performance
 * characterization between client and server endpoints.
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

/*
 * GLOBAL STATE MANAGEMENT
 */

/* Client source information */
char client_src_ip[INET_ADDRSTRLEN] = {0};
int client_src_port = 0;

/* CSV logging configuration */
static csv_config_t csv_config = {
    .csv_enabled = 0,
    .csv_filename = {0},
    .tx_csv_filename = {0},
    .log_cpu = 0 /* Default to CPU0 */
};

/* Stats collection configuration */
stats_config_t stats_config = {
    .enabled = 0,
    .buffer_size = 5000000,        /* 5M packet entries default */
    .bin_width_us = 10,            /* 10us histogram bins widths default */
    .max_bins = 1000               /* 1000 histogram bins default */
};

/* Stats collector and shutdown flag */
stats_collector_t *global_stats_collector = NULL;
static volatile int graceful_shutdown = 0;

/* Arrays for timestamp correlation */
static struct timespec *app_tx_timestamps = NULL;
static uint64_t *app_tx_tsc_values = NULL;      /* TSC correlation array */

/* CSV buffers and socket management */
static csv_ring_buffer_t *tx_csv_buffer = NULL;
static int tx_sockfd_global = -1;

/* RX thread - shared variable for cleanup handler */
static volatile uint64_t final_rx_count = 0;

/* TX timestamp processing CPU configuration */
static int tx_timestamp_cpu = 0;  /* Default to CPU0 */

/*
 * UTILITY & HELPER FUNCTIONS
 */

/* TSC validation function for round-trip mode */
static int validate_tsc_for_operation(void) {
    if (!check_tsc_invariant()) {
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "TSC invariant not supported - TSC timestamps disabled");
        return 0;  /* Disable TSC */
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TSC invariant detected - TSC timestamps enabled");
    return 1;  /* Enable TSC */
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
    fprintf(stderr, "Usage (one-way mode): %s --one-way --dst-ip <ip> --dst-port <port> --pps <pps> --pkt-size <bytes> [OPTIONS]\n", prog_name);
    fprintf(stderr, "Usage (round-trip mode): %s --round-trip --dst-ip <ip> --dst-port <port> --pps <pps> --pkt-size <bytes> [OPTIONS]\n\n", prog_name);

    fprintf(stderr, "Mode argument (exactly one required):\n");
    fprintf(stderr, "  --one-way                    Only send packets\n");
    fprintf(stderr, "  --round-trip                 Send and receive return packets\n\n");
    
    fprintf(stderr, "Required arguments:\n");
    fprintf(stderr, "  --dst-ip <ip>                Destination IP address\n");
    fprintf(stderr, "  --dst-port <port>            Destination port number\n");
    fprintf(stderr, "  --pps <pps>                  Packets to send per second (> 0)\n");
    fprintf(stderr, "  --pkt-size <bytes>           Packet size in bytes (>= 12)\n\n");
    
    fprintf(stderr, "One-way mode options:\n");
    fprintf(stderr, "  --tx-cpu <cpu>               CPU core number for transmit operations (default: 4)\n\n");
    
    fprintf(stderr, "Round-trip mode options:\n");
    fprintf(stderr, "  --rx-port <port>             Port number to listen on for return packets (required)\n");
    fprintf(stderr, "  --rx-interface <interface>   Network interface name for receiving packets (required)\n");
    fprintf(stderr, "  --tx-cpu <cpu>               CPU core number for transmit operations (requires --rx-cpu if specified)\n");
    fprintf(stderr, "  --rx-cpu <cpu>               CPU core number for receive operations (requires --tx-cpu if specified)\n");
    fprintf(stderr, "                               Default: tx=4, rx=5\n\n");

    fprintf(stderr, "Optional arguments:\n");
    fprintf(stderr, "  --time <seconds>             Run for specified number of seconds then exit\n");
    fprintf(stderr, "  --tx-interface <interface>   Network interface name for transmitting packets\n");
    fprintf(stderr, "  --output-csv[=filename]      Enable CSV logging of timestamps to filename\n");
    fprintf(stderr, "  --log-cpu <cpu>              CPU core number for CSV logging thread (requires --output-csv, default: 0)\n");
    fprintf(stderr, "  --tx-timestamp-cpu <cpu>     CPU core number for TX timestamp processing thread (default: 0)\n");
    fprintf(stderr, "  --stats[=config]             Show timestamp delta latency statistics at program completion\n");
    fprintf(stderr, "                               Format: [max-packets-to-evaluate],[bw=bin-width(usec)],[bn=max-bins]\n");
    fprintf(stderr, "                               Defaults: 5M,10us,1000\n");
    fprintf(stderr, "                               Example: --stats=1M,bw=5,bn=100\n");
    fprintf(stderr, "  --log-level <level>          Set output logging level (DEBUG|INFO|WARN|ERROR, default: INFO)\n");
    fprintf(stderr, "  --log-component <component>  Enable specific output log components (comma-separated)\n");
    fprintf(stderr, "                               Components: MAIN|CLIENT|SERVER|STATS|CSV|NETWORK|SIGNAL (default: ALL)\n");
    fprintf(stderr, "  --help                       Show this help message\n");
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

/* Helper functions for simple array-based TX correlation */
int init_tx_correlation(void) {
    
    /* Application TX timestamps */
    app_tx_timestamps = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
    if (!app_tx_timestamps) {
        app_tx_timestamps = calloc(MAX_SEQUENCE_NUMBERS, sizeof(struct timespec));
        if (!app_tx_timestamps) {
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for timestamps array (aligned_alloc not available)");
    } else {
        memset(app_tx_timestamps, 0, MAX_SEQUENCE_NUMBERS * sizeof(struct timespec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned timestamp array allocation");
    }
    
    /* Application TX TSC timestamps */
    app_tx_tsc_values = aligned_alloc(64, MAX_SEQUENCE_NUMBERS * sizeof(uint64_t));
    if (!app_tx_tsc_values) {
        app_tx_tsc_values = calloc(MAX_SEQUENCE_NUMBERS, sizeof(uint64_t));
        if (!app_tx_tsc_values) {
            free(app_tx_timestamps);
            app_tx_timestamps = NULL;
            return -1;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for TSC array (aligned_alloc not available)");
    } else {
        memset(app_tx_tsc_values, 0, MAX_SEQUENCE_NUMBERS * sizeof(uint64_t));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned TSC array allocation");
    }
    
    return 0;
}

/* Cleanup server TX correlation arrays */
void cleanup_tx_correlation(void) {
    if (app_tx_timestamps) {
        free(app_tx_timestamps);
        app_tx_timestamps = NULL;
    }
    if (app_tx_tsc_values) {
        free(app_tx_tsc_values);
        app_tx_tsc_values = NULL;
    }
}

/* Simple direct array storage (O(1) performance) */
static inline void store_tx_timestamp(uint32_t seq_num, struct timespec app_tx_ts) {
    if (app_tx_timestamps) {
        uint32_t index = get_circular_index(seq_num);
        app_tx_timestamps[index] = app_tx_ts;
    }
}

/* Simple direct array storage (O(1) performance) with TSC timestamps for round-trip mode */
static inline void store_tx_timestamps(uint32_t seq_num, struct timespec app_tx_ts, uint64_t tsc_value) {
    if (app_tx_timestamps && app_tx_tsc_values) {
        uint32_t index = get_circular_index(seq_num);
        app_tx_timestamps[index] = app_tx_ts;
        app_tx_tsc_values[index] = tsc_value;
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
                
                /* Use payload sequence number, (kernel sequence to be implemented later if needed) */
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
        
        /* Conditional CSV processing */
        if (csv_config.csv_enabled && tx_csv_buffer) {
            csv_entry_t csv_entry;
            create_csv_client_oneway_tx(&csv_entry, seq_num, client_src_ip, 
                                       (uint16_t)client_src_port, 
                                       kernel_tx_ts.tv_sec * 1000000000ULL + kernel_tx_ts.tv_nsec);
            
            /* Enqueue to TX CSV buffer */
            csv_enqueue_fast(tx_csv_buffer, &csv_entry);
            processed_timestamps++;
        }
        
        /* Stats buffer update for TX timestamp correlation */
        if (stats_config.enabled && global_stats_collector) {            
            stats_mode_type_t correlation_mode = (global_stats_collector->program_mode == STATS_CLIENT_ONEWAY) ? 
                                                STATS_CLIENT_ONEWAY : STATS_CLIENT_ROUNDTRIP;
            update_stats_buffer_with_tx_timestamp(seq_num, kernel_tx_ts.tv_sec * 1000000000ULL + kernel_tx_ts.tv_nsec, correlation_mode);
        }
    }
    
    return processed_timestamps;
}

/*
 * RX THREAD MANAGEMENT
 */

/* RX thread data and synchronization */
typedef struct {
    int rx_sockfd;
    volatile int *running;
    csv_ring_buffer_t *csv_buffer;
} RxThreadData;

/* RX thread cleanup handler - called on thread cancellation */
void rx_cleanup_handler(void* arg) {
    uint64_t* local_count = (uint64_t*)arg;
    __atomic_store_n(&final_rx_count, *local_count, __ATOMIC_RELAXED);
}

/* Dedicated RX thread function */
void* rx_thread_func(void* arg) {
    RxThreadData *rx_data = (RxThreadData*)arg;
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "RX thread started");
    
    /* Single packet receive structures */
    char packet_buffer[RETURN_PACKET_SIZE];
    char ctrl_buffer[CMSG_BUFFER_SIZE];
    struct sockaddr_in src_addr;
    struct iovec iov = { .iov_base = packet_buffer, .iov_len = sizeof(packet_buffer) };
    struct msghdr msg = {
        .msg_name = &src_addr,
        .msg_namelen = sizeof(src_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buffer,
        .msg_controllen = sizeof(ctrl_buffer),
        .msg_flags = 0
    };
    
    uint64_t local_packets_received = 0;
    
    /* Register cleanup handler to preserve packet count on cancellation */
    pthread_cleanup_push(rx_cleanup_handler, &local_packets_received);
    
    /* Pre-allocate and reuse packet data structure */
    RoundTripData rt_data;
    
    /* Busy polling loop */
    while (*rx_data->running) {
        ssize_t packet_size = recvmsg(rx_data->rx_sockfd, &msg, MSG_DONTWAIT);
        if (packet_size <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (*rx_data->running) {
                HW_HOTPATH_COUNT_ERROR();
            }
            break;
        }
        
        /* Fast structure reset (initialize all timestamp fields) */
        memset(&rt_data, 0, sizeof(rt_data));
        rt_data.seq_num = -1;
        
        /* Capture with conditional TSC */
        uint64_t rx_tsc = g_tsc_reliable ? CAPTURE_TSC_TIMESTAMP() : 0;
        struct __kernel_timespec app_rx_ts = get_app_timestamp();
        
        char *packet_data = packet_buffer;
        
        /* Extract timestamps from control messages */
        extract_rx_timestamps(&msg, &rt_data.clt_hw_rx_ts, &rt_data.clt_ker_rx_ts);
        
        /* Use immediate per-packet application RX timestamp */
        rt_data.clt_app_rx_ts = app_rx_ts;
        
        /* Extract return packet payload */
        if (packet_size >= 4) {
            /* Sequence number */
            rt_data.seq_num = ntohl(*(uint32_t *)packet_data);
            
            if (app_tx_timestamps && app_tx_tsc_values) {
                uint32_t index = get_circular_index(rt_data.seq_num);
                struct timespec tx_ts = app_tx_timestamps[index];
                uint64_t tx_tsc = app_tx_tsc_values[index];
                
                rt_data.clt_app_tx_ts.tv_sec = tx_ts.tv_sec;
                rt_data.clt_app_tx_ts.tv_nsec = tx_ts.tv_nsec;
                rt_data.clt_app_tx_tsc_ts = TSC_TO_TIMESPEC(tx_tsc);
            }
            
            /* Set RX TSC timestamp */
            rt_data.clt_app_rx_tsc_ts = TSC_TO_TIMESPEC(rx_tsc);
            
            /* Timestamp processing for stats */
            if (stats_config.enabled && global_stats_collector) {
                update_stats_buffer_with_rx_timestamps(rt_data.seq_num,
                    rt_data.clt_hw_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_hw_rx_ts.tv_nsec,
                    rt_data.clt_ker_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_ker_rx_ts.tv_nsec,
                    rt_data.clt_app_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_app_rx_ts.tv_nsec,
                    rt_data.clt_app_rx_tsc_ts.tv_sec * 1000000000ULL + rt_data.clt_app_rx_tsc_ts.tv_nsec,
                    STATS_CLIENT_ROUNDTRIP);
            }
            
            /* Convert timestamps to nanoseconds */
            uint64_t tx_tsc_ns = rt_data.clt_app_tx_tsc_ts.tv_sec * 1000000000ULL + rt_data.clt_app_tx_tsc_ts.tv_nsec;
            uint64_t app_tx_ns = rt_data.clt_app_tx_ts.tv_sec * 1000000000ULL + rt_data.clt_app_tx_ts.tv_nsec;
            uint64_t hw_rx_ns = rt_data.clt_hw_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_hw_rx_ts.tv_nsec;
            uint64_t ker_rx_ns = rt_data.clt_ker_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_ker_rx_ts.tv_nsec;
            uint64_t rx_tsc_ns = rt_data.clt_app_rx_tsc_ts.tv_sec * 1000000000ULL + rt_data.clt_app_rx_tsc_ts.tv_nsec;
            uint64_t app_rx_ns = rt_data.clt_app_rx_ts.tv_sec * 1000000000ULL + rt_data.clt_app_rx_ts.tv_nsec;
            
            /* Conditional CSV processing */
            if (csv_config.csv_enabled && rx_data->csv_buffer) {
                csv_entry_t csv_entry;
                create_csv_client_roundtrip_rx(&csv_entry, rt_data.seq_num, client_src_ip, 
                                              (uint16_t)client_src_port, tx_tsc_ns, app_tx_ns,
                                              hw_rx_ns, ker_rx_ns, rx_tsc_ns, app_rx_ns);
                
                /* Enqueue to CSV buffer */
                csv_enqueue_fast(rx_data->csv_buffer, &csv_entry);
            }
            
            /* Increment packets receieved local counter */
            local_packets_received++;
            
            /* Update atomic counter for signal-based PPS reporting */
            __atomic_fetch_add(&g_packets_received, 1, __ATOMIC_RELAXED);
        }
    }
    
    /* Cleanup handler won't be called on normal exit, so pop it without execution */
    pthread_cleanup_pop(0);
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "RX thread terminated (received %llu packets)", 
           (unsigned long long)local_packets_received);
    return (void*)local_packets_received;
}

/*
 * MAIN ENTRY POINT
 */

/* Forward declarations for mode-specific functions */
int run_client_oneway(const char *dest_ip, int dest_port, int pps, int packet_size, 
                               int duration_seconds, int tx_cpu, const char *tx_interface);
int run_client_roundtrip(const char *dest_ip, int dest_port, int pps, int packet_size, 
                        int duration_seconds, int tx_cpu, int rx_cpu, int rx_port,
                        const char *tx_interface, const char *rx_interface);

int main(int argc, char *argv[]) {
    const char *dest_ip = NULL;
    int dest_port = 0;
    int pps = 0;
    int packet_size = 0;
    int duration_seconds = 0;         /* Run indefinitely by default */
    int tx_cpu = 4;                   /* Default TX CPU core */
    int rx_cpu = 5;                   /* Default RX CPU core */
    int rx_port = 0;                  /* No receive functionality by default */
    int round_trip_mode = 0;          /* Flag for round-trip mode */
    int one_way_mode = 0;             /* Flag for one-way mode */
    const char *tx_interface = NULL;  /* TX interface for socket binding */
    const char *rx_interface = NULL;  /* RX interface for round-trip mode */
    
    static struct option long_options[] = {
        {"dst-ip", required_argument, 0, 'i'},
        {"dst-port", required_argument, 0, 'p'},
        {"pps", required_argument, 0, 'r'},
        {"pkt-size", required_argument, 0, 's'},
        {"tx-interface", required_argument, 0, 'I'},
        {"time", required_argument, 0, 't'},
        {"tx-cpu", required_argument, 0, 'T'},
        {"rx-cpu", required_argument, 0, 'R'},
        {"rx-port", required_argument, 0, 'x'},
        {"round-trip", no_argument, 0, 'o'},
        {"one-way", no_argument, 0, 'w'},
        {"output-csv", optional_argument, 0, 'C'},
        {"log-cpu", required_argument, 0, 'L'},
        {"rx-interface", required_argument, 0, 'n'},
        {"stats", optional_argument, 0, 'S'},
        {"log-level", required_argument, 0, 'l'},
        {"log-component", required_argument, 0, 'c'},
        {"tx-timestamp-cpu", required_argument, 0, 'X'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "i:p:r:s:I:t:T:R:x:owC::L:n:S::l:c:X:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                dest_ip = optarg;
                break;
            case 'p':
                dest_port = atoi(optarg);
                break;
            case 'r':
                pps = atoi(optarg);
                break;
            case 's':
                packet_size = atoi(optarg);
                break;
            case 'I':
                tx_interface = optarg;
                break;
            case 't':
                duration_seconds = atoi(optarg);
                break;
            case 'T':
                tx_cpu = atoi(optarg);
                break;
            case 'R':
                rx_cpu = atoi(optarg);
                break;
            case 'x':
                rx_port = atoi(optarg);
                break;
            case 'o':
                round_trip_mode = 1;
                break;
            case 'w':
                one_way_mode = 1;
                break;
            case 'C':
                csv_config.csv_enabled = 1;
                if (optarg) {
                    strncpy(csv_config.csv_filename, optarg, sizeof(csv_config.csv_filename) - 1);
                    csv_config.csv_filename[sizeof(csv_config.csv_filename) - 1] = '\0';
                } else {
                    snprintf(csv_config.csv_filename, sizeof(csv_config.csv_filename), 
                             "client_timestamps_%d.csv", getpid());
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
                    HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Error: --log-cpu must be >= 0");
                    return EXIT_FAILURE;
                }
                break;
            case 'n':
                rx_interface = optarg;
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
                return EXIT_FAILURE;
        }
    }
    
    /* Validate flag argument dependencies */
    if (csv_config.log_cpu != 0 && !csv_config.csv_enabled) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--log-cpu can only be used with --output-csv");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    if (!one_way_mode && !round_trip_mode) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Either --one-way or --round-trip must be specified");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (one_way_mode && round_trip_mode) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Cannot specify both --one-way and --round-trip");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!dest_ip || dest_port == 0 || pps == 0 || packet_size == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "All arguments are required");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (dest_port <= 0 || dest_port > 65535) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Destination port must be between 1 and 65535");
        return EXIT_FAILURE;
    }
    if (pps <= 0 || packet_size < 12) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Packets per second must be > 0 and packet size must be >= 12 bytes");
        return EXIT_FAILURE;
    }
    if (packet_size > MAX_PACKET_SIZE) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Packet size must be <= %d bytes", MAX_PACKET_SIZE);
        return EXIT_FAILURE;
    }
    if (duration_seconds < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Duration must be >= 0 seconds (0 means run indefinitely)");
        return EXIT_FAILURE;
    }
    
    if (rx_port > 0 && (rx_port <= 0 || rx_port > 65535)) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "RX port must be between 1 and 65535");
        return EXIT_FAILURE;
    }
    
    if (one_way_mode) {
        if (rx_cpu != 5) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--rx-cpu is not supported in --one-way mode");
            return EXIT_FAILURE;
        }
        if (rx_port > 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--rx-port is not supported in --one-way mode");
            return EXIT_FAILURE;
        }
        if (rx_interface) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--rx-interface is not supported in --one-way mode");
            return EXIT_FAILURE;
        }
    }

    if (round_trip_mode && rx_port == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Round-trip mode requires --rx-port to be specified");
        return EXIT_FAILURE;
    }
    
    if (round_trip_mode && !rx_interface) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Round-trip mode requires --rx-interface to be specified");
        return EXIT_FAILURE;
    }
    
    if (one_way_mode) {
        int tx_cpu_specified = (tx_cpu != 4);
        if (tx_cpu_specified) {
            rx_cpu = 5;
        }
    } else if (round_trip_mode) {
        int tx_cpu_specified = (tx_cpu != 4);
        int rx_cpu_specified = (rx_cpu != 5);
        
        if (tx_cpu_specified && !rx_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "In round-trip mode, if --tx-cpu is specified, --rx-cpu must also be specified");
            return EXIT_FAILURE;
        }
        if (!tx_cpu_specified && rx_cpu_specified) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "In round-trip mode, if --rx-cpu is specified, --tx-cpu must also be specified");
            return EXIT_FAILURE;
        }
    }
    
    /* Initialize console logging system */
    hw_log_init();
    
    /* Setup signal handling for graceful shutdown */
    setup_signal_handling();
    
    /* Initialize stats system if enabled */
    if (stats_config.enabled) {
        stats_mode_type_t mode = one_way_mode ? STATS_CLIENT_ONEWAY : STATS_CLIENT_ROUNDTRIP;
        global_stats_collector = create_stats_collector(stats_config.buffer_size, mode);
        if (!global_stats_collector) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to initialize statistics system");
            return EXIT_FAILURE;
        }
    }
    
    /* Dispatch to mode-specific implementations */
    if (one_way_mode) {
        return run_client_oneway(dest_ip, dest_port, pps, packet_size, 
                                          duration_seconds, tx_cpu, tx_interface);
    } else {
        return run_client_roundtrip(dest_ip, dest_port, pps, packet_size, 
                                   duration_seconds, tx_cpu, rx_cpu, rx_port,
                                   tx_interface, rx_interface);
    }
}

/*
 * CORE EXECUTION ENGINES
 */

/* One-way mode implementation */
int run_client_oneway(const char *dest_ip, int dest_port, int pps, int packet_size, 
                               int duration_seconds, int tx_cpu, const char *tx_interface) {
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Client configuration:");
    if (tx_interface) {
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX Interface: %s", tx_interface);
    }
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Target: %s:%d, PPS: %d, Packet size: %d bytes", dest_ip, dest_port, pps, packet_size);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX CPU: %d", tx_cpu);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "One-way mode");
    if (csv_config.csv_enabled) {
    } else {
    }

    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX socket setup");
    
    /* Optimize process scheduling and CPU affinity */
    optimize_process_scheduling(tx_cpu);
    
    /* Calibrate CPU frequency for precise timing (on bound TX CPU) */
    calibrate_cpu_freq();
    
    /* Create UDP socket with low latency options */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Socket creation failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    
    /* Apply socket optimizations (with zero-copy for TX) */
    if (optimize_socket_performance(sockfd, tx_cpu, 1) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    /* Bind TX socket to specific interface */
    if (tx_interface) {
        struct ifreq tx_if_opts;
        memset(&tx_if_opts, 0, sizeof(tx_if_opts));
        strncpy(tx_if_opts.ifr_name, tx_interface, IFNAMSIZ-1);
        
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &tx_if_opts, sizeof(tx_if_opts)) < 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "TX SO_BINDTODEVICE failed: %s", strerror(errno));
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX socket bound to interface %s", tx_interface);
    }
    
    /* Setup TX timestamping on socket */
    if (setup_tx_timestamping(sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to setup TX socket timestamping");
        close(sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX timestamping enabled on socket");
    
    /* Conditional TX CSV buffer creation */
    if (csv_config.csv_enabled) {
        tx_csv_buffer = csv_buffer_create(
            1048576,
            csv_config.tx_csv_filename,
            CSV_CLIENT_TX,
            10000,
            csv_config.log_cpu
        );
        if (!tx_csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to create high-performance TX CSV buffer");
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX CSV logging initialized: %s", csv_config.tx_csv_filename);
    } else {
        tx_csv_buffer = NULL;
    }
    
    /* Initialize simple array-based TX correlation system */
    if (init_tx_correlation() < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to initialize TX correlation system");
        csv_buffer_destroy(tx_csv_buffer);
        close(sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX array correlation system initialized");
    
    /* Store socket for background processing */
    tx_sockfd_global = sockfd;
    
    /* Start dedicated TX timestamp processing thread */
    pthread_t tx_timestamp_thread;
    tx_timestamp_thread_data_t *tx_thread_data = NULL;
    
    if (start_tx_timestamp_processing_thread(sockfd, tx_timestamp_cpu, &tx_timestamp_thread, &tx_thread_data) != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to start TX timestamp processing thread for one-way mode");
        csv_buffer_destroy(tx_csv_buffer);
        cleanup_tx_correlation();
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX timestamp processing thread started");

    /* Setup signal-based PPS reporting */
    setup_stats_reporting_hotpath();

    /* Conditional main CSV buffer creation */
    csv_ring_buffer_t *csv_buffer = NULL;
    if (csv_config.csv_enabled) {
        csv_type_t client_csv_type = CSV_CLIENT_MAIN_ONEWAY;
        csv_buffer = csv_buffer_create(
            1048576,
            csv_config.csv_filename,
            client_csv_type,
            10000,
            csv_config.log_cpu
        );
        if (!csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to create high-performance CSV buffer");
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "CSV logging initialized");
    } else {
    }

    /* Configure destination address */
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) <= 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Address conversion failed: %s", strerror(errno));
        csv_buffer_destroy(csv_buffer);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Connect UDP socket */
    if (connect(sockfd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "UDP socket connect failed: %s", strerror(errno));
        csv_buffer_destroy(csv_buffer);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Capture client source IP and port */
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sockfd, (struct sockaddr*)&local_addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &local_addr.sin_addr, client_src_ip, INET_ADDRSTRLEN);
        client_src_port = ntohs(local_addr.sin_port);
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Client source IP: %s:%d", client_src_ip, client_src_port);
    } else {
        strncpy(client_src_ip, "unknown", INET_ADDRSTRLEN-1);
        client_src_port = 0;
    }

    /* Pre-allocate packet buffers for batch sending with fallback */
    char *packet_buffers = aligned_alloc(64, BATCH_SIZE * packet_size);
    if (!packet_buffers) {
        packet_buffers = calloc(BATCH_SIZE, packet_size);
        if (!packet_buffers) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Packet buffer allocation failed: %s", strerror(errno));
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for packet buffers (aligned_alloc not available)");
    } else {
        memset(packet_buffers, 0, BATCH_SIZE * packet_size);
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned packet buffer allocation");
    }
    
    /* Messages allocation with fallback */
    struct mmsghdr *msgs = aligned_alloc(64, BATCH_SIZE * sizeof(struct mmsghdr));
    if (!msgs) {
        msgs = calloc(BATCH_SIZE, sizeof(struct mmsghdr));
        if (!msgs) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Message structure allocation failed (both aligned and standard allocation failed): %s", strerror(errno));
            free(packet_buffers);
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for msgs (aligned_alloc not available)");
    } else {
        memset(msgs, 0, BATCH_SIZE * sizeof(struct mmsghdr));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned msgs allocation");
    }
    
        /* IO vectors allocation with fallback */
        struct iovec *iovecs = aligned_alloc(64, BATCH_SIZE * sizeof(struct iovec));
        if (!iovecs) {
            iovecs = calloc(BATCH_SIZE, sizeof(struct iovec));
            if (!iovecs) {
                free(packet_buffers);
                free(msgs);
                close(sockfd);
                return EXIT_FAILURE;
            }
            HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for iovecs (aligned_alloc not available)");
        } else {
            memset(iovecs, 0, BATCH_SIZE * sizeof(struct iovec));
            HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned iovecs allocation");
        }

    /* Initialize message structures (no control messages - let kernel auto-assign correlation IDs) */
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovecs[i].iov_base = packet_buffers + (i * packet_size);
        iovecs[i].iov_len = packet_size;
        
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &dest_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(dest_addr);
        msgs[i].msg_hdr.msg_control = NULL;
        msgs[i].msg_hdr.msg_controllen = 0;
    }

    /* Timing control with CPU cycles */
    const uint64_t interval_cycles = (uint64_t)(cpu_freq_ghz * 1e9 / pps);
    const long long interval_ns = 1000000000LL / pps;
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Starting one-way packet transmission");
    printf("\n");
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Target PPS: %d, Interval: %lld ns, CPU cycles per packet: %llu", 
           pps, interval_ns, (unsigned long long)interval_cycles);
    
    long long start_time = monotonic_time_ns();
    long long end_time = (duration_seconds > 0) ? start_time + ((long long)duration_seconds * 1000000000LL) : 0;
    
    uint32_t seq_num = 0;
    uint64_t total_packets_sent = 0;
    
    /* Hybrid time checking with minimal hot path impact */
    uint64_t loop_counter = 0;
    uint64_t last_time_check_cycles = rdtsc();
    uint64_t max_cycles_between_checks = (uint64_t)(cpu_freq_ghz * 1e9 * 0.1);

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
            long long current_time = 0;
            if (end_time > 0) {
                current_time = monotonic_time_ns();
                if (current_time >= end_time) {
                    printf("\n");
                    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Finished run time");
                    printf("\n");
                    break;
                }
                last_time_check_cycles = rdtsc();
            }
             
            /* Display PPS stats if ready */
            display_stats_if_ready();
        }
        
        int batch_count = (pps >= BATCH_SIZE) ? BATCH_SIZE : 1;
        
        /* Store application TX timestamps for correlation */
        struct timespec batch_app_tx_timestamps[BATCH_SIZE];
        
        for (int i = 0; i < batch_count; i++) {
            char *packet = packet_buffers + (i * packet_size);
            uint32_t current_seq = seq_num + i;
            
            /* Insert sequence number (network byte order) */
            uint32_t net_seq = htonl(current_seq);
            memcpy(packet, &net_seq, sizeof(net_seq));
            
            /* Generate application TX timestamp immediately before send */
            struct timespec src_time = get_system_time();
            batch_app_tx_timestamps[i] = src_time;
        }

        /* Send batch */
        int sent = sendmmsg(sockfd, msgs, batch_count, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "sendmmsg failed: %s", strerror(errno));
            break;
        }

        /* Application TX timestamp processing */
        for (int i = 0; i < sent; i++) {
            uint32_t current_seq = seq_num + i;
            struct timespec *app_tx_ts = &batch_app_tx_timestamps[i];
            
            /* Stats processing - create minimal entry and update TX timestamp */
            if (stats_config.enabled && global_stats_collector) {
                create_minimal_stats_entry(current_seq, client_src_port, client_src_ip,
                    TIMESTAMP_MODE_CLIENT_ONEWAY, STATS_CLIENT_ONEWAY);
                update_stats_buffer_with_app_tx_timestamp(current_seq,
                    app_tx_ts->tv_sec * 1000000000ULL + app_tx_ts->tv_nsec,
                    0,
                    STATS_CLIENT_ONEWAY);
            }
            
            /* Conditional CSV processing */
            if (csv_config.csv_enabled && csv_buffer) {
                csv_entry_t csv_entry;
                create_csv_client_oneway_main(&csv_entry, current_seq, client_src_ip, 
                                             (uint16_t)client_src_port, 
                                             app_tx_ts->tv_sec * 1000000000ULL + app_tx_ts->tv_nsec);
                
                /* Enqueue to CSV buffer */
                csv_enqueue_fast(csv_buffer, &csv_entry);
            }
        }

        /* Direct array storage for async TX timestamp processing */
        for (int i = 0; i < sent; i++) {
            store_tx_timestamp(seq_num + i, batch_app_tx_timestamps[i]);
        }

        seq_num += sent;
        total_packets_sent += sent;
        
        /* Update atomic counter for signal-based PPS reporting */
        __atomic_fetch_add(&g_packets_sent, sent, __ATOMIC_RELAXED);


        /* Precise timing control */
        uint64_t target_delay_cycles = (batch_count > 1) ? 
            interval_cycles * batch_count : interval_cycles;
        
        precise_delay_cycles(target_delay_cycles);
    }

    /* Stop TX timestamp processing thread */
    stop_tx_timestamp_processing_thread(tx_timestamp_thread, tx_thread_data);
    
    /* Process any remaining TX timestamps in the error queue */
    if (sockfd >= 0) {
        int total_processed = 0;
        int batch_processed;
        do {
            batch_processed = process_tx_timestamps_in_main_thread(sockfd);
            total_processed += batch_processed;
        } while (batch_processed > 0);
    }
    
    /* Cleanup stats system */
    if (stats_config.enabled && global_stats_collector) {
        if (buffer_has_data(global_stats_collector)) {
            stats_analysis_result_t *result = calloc(1, sizeof(stats_analysis_result_t));
            if (result) {
                stats_mode_type_t mode = (stats_mode_type_t)global_stats_collector->program_mode;
                if (initialize_analysis_result(result, mode, &stats_config) == 0) {
                    process_buffer_for_analysis(global_stats_collector, result);
                    display_analysis_results(result, total_packets_sent, 0);
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
    long long total_time = monotonic_time_ns() - start_time;
    double actual_duration = total_time / 1e9;
    double achieved_pps = total_packets_sent / actual_duration;
    
    printf("\n");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "EXECUTION DETAILS");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "=================");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Total packets sent: %llu", (unsigned long long)total_packets_sent);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Target TX PPS: %d", pps);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Actual TX PPS: %.0f", achieved_pps);
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Timestamps CSV filename: %s", csv_config.csv_filename);
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX timestamps CSV filename: %s", csv_config.tx_csv_filename);
    } else {
    }

    /* Close TX socket */
    close(sockfd);
    
    /* Cleanup and flush CSV buffers */
    if (tx_csv_buffer) {
        csv_buffer_destroy(tx_csv_buffer);
    }
    if (csv_buffer) {
        csv_buffer_destroy(csv_buffer);
    }

    /* Cleanup signal-based PPS reporting */
    cleanup_stats_reporting_hotpath();

    /* Cleanup packet buffers */
    free(packet_buffers);
    free(msgs);
    free(iovecs);
    
    /* Cleanup TX correlation system */
    cleanup_tx_correlation();
    
    return 0;
}

/** Round-trip mode */
int run_client_roundtrip(const char *dest_ip, int dest_port, int pps, int packet_size, 
                        int duration_seconds, int tx_cpu, int rx_cpu, int rx_port,
                        const char *tx_interface, const char *rx_interface) {
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Client configuration:");
    if (tx_interface) {
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX Interface: %s", tx_interface);
    }
    if (rx_interface) {
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "RX Interface: %s", rx_interface);
    }
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Target: %s:%d, PPS: %d, Packet size: %d bytes", dest_ip, dest_port, pps, packet_size);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX CPU: %d, RX CPU: %d", tx_cpu, rx_cpu);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Round-trip mode");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "RX port: %d", rx_port);

    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX socket setup");
    
    /* Optimize process scheduling and CPU affinity */
    optimize_process_scheduling(tx_cpu);
    
    /* Calibrate CPU frequency for precise timing (on bound TX CPU) */
    calibrate_cpu_freq();
    
    /* Validate TSC before starting */
    g_tsc_reliable = validate_tsc_for_operation();
    if (g_tsc_reliable) {
    } else {
    }
    
    /* Create UDP socket with low latency options */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Socket creation failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    
    /* Apply socket optimizations (with zero-copy for TX) */
    if (optimize_socket_performance(sockfd, tx_cpu, 1) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    /* Bind TX socket to specific interface */
    if (tx_interface) {
        struct ifreq tx_if_opts;
        memset(&tx_if_opts, 0, sizeof(tx_if_opts));
        strncpy(tx_if_opts.ifr_name, tx_interface, IFNAMSIZ-1);
        
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &tx_if_opts, sizeof(tx_if_opts)) < 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "TX SO_BINDTODEVICE failed: %s", strerror(errno));
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX socket bound to interface %s", tx_interface);
    }
    
    /* Setup TX timestamping on socket */
    if (setup_tx_timestamping(sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to setup TX socket timestamping");
        close(sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX timestamping enabled on socket");
    
    /* Conditional TX CSV buffer creation */
    if (csv_config.csv_enabled) {
        tx_csv_buffer = csv_buffer_create(
            1048576,
            csv_config.tx_csv_filename,
            CSV_CLIENT_TX,
            10000,
            csv_config.log_cpu
        );
        if (!tx_csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to create high-performance TX CSV buffer");
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX CSV logging initialized: %s", csv_config.tx_csv_filename);
    } else {
        tx_csv_buffer = NULL;
    }
    
    /* Initialize simple array-based TX correlation system */ 
    if (init_tx_correlation() < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to initialize TX correlation system");
        csv_buffer_destroy(tx_csv_buffer);
        close(sockfd);
        return EXIT_FAILURE;
    }
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX array correlation system initialized");
    
    /* Store socket for background processing */
    tx_sockfd_global = sockfd;
    
    /* Start dedicated TX timestamp processing thread */
    pthread_t tx_timestamp_thread;
    tx_timestamp_thread_data_t *tx_thread_data = NULL;
    
    if (start_tx_timestamp_processing_thread(sockfd, tx_timestamp_cpu, &tx_timestamp_thread, &tx_thread_data) != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to start TX timestamp processing thread for round-trip mode");
        csv_buffer_destroy(tx_csv_buffer);
        cleanup_tx_correlation();
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "TX timestamp processing thread started");

    /* Setup signal-based PPS reporting */
    setup_stats_reporting_hotpath();

    /* Create RX socket for return packets */
    int rx_sockfd = -1;
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "RX socket setup");
    
    rx_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx_sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Socket creation failed: %s", strerror(errno));
        close(sockfd);
        return EXIT_FAILURE;
    }
    
    /* Apply optimizations to RX socket */
    if (optimize_socket_performance(rx_sockfd, rx_cpu, 0) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Failed to optimize RX socket");
        close(sockfd);
        close(rx_sockfd);
        return EXIT_FAILURE;
    }

    /* Configure hardware timestamping on the RX interface */
    const char *hw_if_name = rx_interface;
    struct ifreq hwtstamp_ifreq;
    struct hwtstamp_config hwconfig;
    memset(&hwtstamp_ifreq, 0, sizeof(hwtstamp_ifreq));
    memset(&hwconfig, 0, sizeof(hwconfig));
    strncpy(hwtstamp_ifreq.ifr_name, hw_if_name, IFNAMSIZ-1);
    hwconfig.tx_type = HWTSTAMP_TX_OFF;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
    hwtstamp_ifreq.ifr_data = (void*)&hwconfig;
    
    if (ioctl(rx_sockfd, SIOCSHWTSTAMP, &hwtstamp_ifreq) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Hardware timestamping not supported on %s: %s", 
                hw_if_name, strerror(errno));
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Continuing with software timestamping only");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "RX hardware timestamping enabled on %s", hw_if_name);
    }

    /* Set up RX socket timestamping */
    if (setup_timestamping(rx_sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to setup RX socket timestamping");
        close(sockfd);
        close(rx_sockfd);
        return EXIT_FAILURE;
    }
    
    /* Bind RX socket to specific interface */
    if (rx_interface) {
        struct ifreq rx_if_opts;
        memset(&rx_if_opts, 0, sizeof(rx_if_opts));
        strncpy(rx_if_opts.ifr_name, rx_interface, IFNAMSIZ-1);
        
        if (setsockopt(rx_sockfd, SOL_SOCKET, SO_BINDTODEVICE, &rx_if_opts, sizeof(rx_if_opts)) < 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "RX SO_BINDTODEVICE failed: %s", strerror(errno));
            close(sockfd);
            close(rx_sockfd);
            return EXIT_FAILURE;
        }
    }
    
    /* Bind RX socket to listen port */
    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    rx_addr.sin_port = htons(rx_port);
    
    if (bind(rx_sockfd, (struct sockaddr *)&rx_addr, sizeof(rx_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "RX socket bind failed: %s", strerror(errno));
        close(sockfd);
        close(rx_sockfd);
        return EXIT_FAILURE;
    }
        
    /* Conditional main CSV buffer creation */
    csv_ring_buffer_t *csv_buffer = NULL;
    if (csv_config.csv_enabled) {
        csv_type_t client_csv_type = CSV_CLIENT_MAIN_ROUNDTRIP;
        csv_buffer = csv_buffer_create(
            1048576,
            csv_config.csv_filename,
            client_csv_type,
            10000,
            csv_config.log_cpu
        );
        if (!csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to create high-performance CSV buffer for round-trip mode");
            close(sockfd);
            close(rx_sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "CSV logging initialized");
    } else {
    }
    
    /* Configure destination address */
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) <= 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Address conversion failed: %s", strerror(errno));
        close(rx_sockfd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Connect UDP socket */
    if (connect(sockfd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "UDP socket connect failed: %s", strerror(errno));
        close(rx_sockfd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Capture client source IP and port */
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sockfd, (struct sockaddr*)&local_addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &local_addr.sin_addr, client_src_ip, INET_ADDRSTRLEN);
        client_src_port = ntohs(local_addr.sin_port);
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Client source IP: %s:%d", client_src_ip, client_src_port);
    } else {
        strncpy(client_src_ip, "unknown", INET_ADDRSTRLEN-1);
        client_src_port = 0;
    }

    /* Pre-allocate packet buffers for batch sending with fallback */
    char *packet_buffers = aligned_alloc(64, BATCH_SIZE * packet_size);
    if (!packet_buffers) {
        packet_buffers = calloc(BATCH_SIZE, packet_size);
        if (!packet_buffers) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Packet buffer allocation failed: %s", strerror(errno));
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for packet buffers (aligned_alloc not available)");
    } else {
        memset(packet_buffers, 0, BATCH_SIZE * packet_size);
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned packet buffer allocation");
    }
    
    /* Messages allocation with fallback */
    struct mmsghdr *msgs = aligned_alloc(64, BATCH_SIZE * sizeof(struct mmsghdr));
    if (!msgs) {
        msgs = calloc(BATCH_SIZE, sizeof(struct mmsghdr));
        if (!msgs) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Message structure allocation failed (both aligned and standard allocation failed): %s", strerror(errno));
            free(packet_buffers);
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for msgs (aligned_alloc not available)");
    } else {
        memset(msgs, 0, BATCH_SIZE * sizeof(struct mmsghdr));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned msgs allocation");
    }
    
    /* IO vectors allocation with fallback */
    struct iovec *iovecs = aligned_alloc(64, BATCH_SIZE * sizeof(struct iovec));
    if (!iovecs) {
        iovecs = calloc(BATCH_SIZE, sizeof(struct iovec));
        if (!iovecs) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Message structure allocation failed (both aligned and standard allocation failed): %s", strerror(errno));
            free(packet_buffers);
            free(msgs);
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_CLIENT, "Using standard allocation for iovecs (aligned_alloc not available)");
    } else {
        memset(iovecs, 0, BATCH_SIZE * sizeof(struct iovec));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Using 64-byte aligned iovecs allocation");
    }

    /* Initialize message structures (no control messages - let kernel auto-assign correlation IDs) */
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovecs[i].iov_base = packet_buffers + (i * packet_size);
        iovecs[i].iov_len = packet_size;
        
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &dest_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(dest_addr);
        msgs[i].msg_hdr.msg_control = NULL;
        msgs[i].msg_hdr.msg_controllen = 0;
    }

    /* Timing control with CPU cycles */
    const uint64_t interval_cycles = (uint64_t)(cpu_freq_ghz * 1e9 / pps);
    const long long interval_ns = 1000000000LL / pps;
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Starting round-trip packet transmission");
    printf("\n");
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CLIENT, "Target PPS: %d, Interval: %lld ns, CPU cycles per packet: %llu", 
           pps, interval_ns, (unsigned long long)interval_cycles);
    
    long long start_time = monotonic_time_ns();
    long long end_time = (duration_seconds > 0) ? start_time + ((long long)duration_seconds * 1000000000LL) : 0;
    
    uint32_t seq_num = 0;
    uint64_t total_packets_sent = 0;
    volatile uint64_t total_packets_received = 0;
    
    /* Thread management for RX processing */
    pthread_t rx_thread = 0;
    volatile int rx_thread_running = 0;
    RxThreadData rx_thread_data = {0};
    
    /* Start dedicated RX thread */
    rx_thread_data.rx_sockfd = rx_sockfd;
    rx_thread_data.running = &rx_thread_running;
    rx_thread_data.csv_buffer = csv_buffer;
    
    rx_thread_running = 1;
    
    /* Set up RX thread attributes for proper scheduling */
    if (create_realtime_thread(&rx_thread, rx_thread_func, &rx_thread_data, rx_cpu, 99, "Client RX") != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to create realtime RX thread");
        rx_thread_running = 0;
    }

    /* Hybrid time checking with minimal hot path impact */
    uint64_t loop_counter_rt = 0;
    uint64_t last_time_check_cycles_rt = rdtsc();
    uint64_t max_cycles_between_checks_rt = (uint64_t)(cpu_freq_ghz * 1e9 * 0.1);

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
            if (end_time > 0) {
                long long current_time = monotonic_time_ns();
                if (current_time >= end_time) {
                    printf("\n");
                    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Finished run time");
                    printf("\n");
                    break;
                }
                last_time_check_cycles_rt = rdtsc();
            }
            
            /* Display stats if ready */
            display_stats_if_ready();
        }
        
        int batch_count = (pps >= BATCH_SIZE) ? BATCH_SIZE : 1;
        
        /* Store application TX timestamps for correlation */
        struct timespec batch_app_tx_timestamps[BATCH_SIZE];
        uint64_t batch_app_tx_tsc_values[BATCH_SIZE];
        
        for (int i = 0; i < batch_count; i++) {
            char *packet = packet_buffers + (i * packet_size);
            uint32_t current_seq = seq_num + i;
            
            /* Insert sequence number (network byte order) */
            uint32_t net_seq = htonl(current_seq);
            memcpy(packet, &net_seq, sizeof(net_seq));
            
            /* Generate application TX timestamp immediately before send with conditional TSC timestamp */
            uint64_t tx_tsc = g_tsc_reliable ? CAPTURE_TSC_TIMESTAMP() : 0;
            struct timespec src_time = get_system_time();
            batch_app_tx_timestamps[i] = src_time;
            batch_app_tx_tsc_values[i] = tx_tsc;
            
            /* Insert client RX port for return packets */
            uint32_t rx_port_net = htonl((uint32_t)rx_port);
            memcpy(packet + 4, &rx_port_net, 4);
        }

        /* Send batch of packets */
        int sent = sendmmsg(sockfd, msgs, batch_count, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "sendmmsg failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < sent; i++) {
            store_tx_timestamps(seq_num + i, batch_app_tx_timestamps[i], batch_app_tx_tsc_values[i]);
        }

        /* Stats processing for application TX timestamps */
        if (stats_config.enabled && global_stats_collector) {
            for (int i = 0; i < sent; i++) {
                uint32_t current_seq = seq_num + i;
                struct timespec *app_tx_ts = &batch_app_tx_timestamps[i];
                uint64_t tsc_tx = batch_app_tx_tsc_values[i];
                
                /* Create minimal entry for this sequence number */
                create_minimal_stats_entry(current_seq, client_src_port, client_src_ip,
                    TIMESTAMP_MODE_CLIENT_ROUNDTRIP, STATS_CLIENT_ROUNDTRIP);
                
                update_stats_buffer_with_app_tx_timestamp(current_seq,
                    app_tx_ts->tv_sec * 1000000000ULL + app_tx_ts->tv_nsec,
                    TSC_TO_TIMESPEC(tsc_tx).tv_sec * 1000000000ULL + TSC_TO_TIMESPEC(tsc_tx).tv_nsec,
                    STATS_CLIENT_ROUNDTRIP);
            }
        }

        seq_num += sent;
        total_packets_sent += sent;

        /* Update atomic counter for signal-based PPS reporting */
        __atomic_fetch_add(&g_packets_sent, sent, __ATOMIC_RELAXED);


        /* Timing control */
        uint64_t target_delay_cycles = (batch_count > 1) ? 
            interval_cycles * batch_count : interval_cycles;
        
        precise_delay_cycles(target_delay_cycles);
    }

    /* Stop TX timestamp processing */
    stop_tx_timestamp_processing_thread(tx_timestamp_thread, tx_thread_data);
    
    /* Process remaining TX timestamps in the error queue */
    if (graceful_shutdown) {
        if (sockfd >= 0) {
            int total_processed = 0;
            int batch_processed;
            do {
                batch_processed = process_tx_timestamps_in_main_thread(sockfd);
                total_processed += batch_processed;
            } while (batch_processed > 0);
        }
    }

    /* Stop RX thread and get final RX count */
    if (rx_thread_running) {
        rx_thread_running = 0;
        
        /* Close RX socket */
        if (rx_sockfd >= 0) {
            close(rx_sockfd);
        }
        
        /* Get final RX count from thread return value with timeout */
        void* rx_result = NULL;
        
        /* Try to join with a reasonable timeout */
        struct timespec timeout_spec;
        clock_gettime(CLOCK_REALTIME, &timeout_spec);
        timeout_spec.tv_sec += 1;
        
        int join_result = pthread_timedjoin_np(rx_thread, &rx_result, &timeout_spec);
        if (join_result == 0) {
            total_packets_received = (uint64_t)rx_result;
        } else if (join_result == ETIMEDOUT) {
            if (pthread_cancel(rx_thread) == 0) {
                pthread_join(rx_thread, &rx_result);
                total_packets_received = __atomic_load_n(&final_rx_count, __ATOMIC_RELAXED);
            }
        } else {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CLIENT, "Failed to join RX thread: %s", strerror(join_result));
        }
        
        /* Mark socket as closed after thread cleanup */
        rx_sockfd = -1;
    }
        
    /* Cleanup stats system */
    if (stats_config.enabled && global_stats_collector) {
        if (buffer_has_data(global_stats_collector)) {
            stats_analysis_result_t *result = calloc(1, sizeof(stats_analysis_result_t));
            if (result) {
                stats_mode_type_t mode = (stats_mode_type_t)global_stats_collector->program_mode;
                if (initialize_analysis_result(result, mode, &stats_config) == 0) {
                    process_buffer_for_analysis(global_stats_collector, result);
                    display_analysis_results(result, total_packets_sent, total_packets_received);
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
    long long total_time = monotonic_time_ns() - start_time;
    double actual_duration = total_time / 1e9;
    double achieved_pps = total_packets_sent / actual_duration;
    
    printf("\n");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "EXECUTION DETAILS");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "=================");
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Total packets sent: %llu", (unsigned long long)total_packets_sent);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Total return packets received: %llu", (unsigned long long)total_packets_received);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Target TX PPS: %d", pps);
    HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Actual TX PPS: %.0f", achieved_pps);
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "Timestamps CSV filename: %s", csv_config.csv_filename);
        HW_LOG_INFO(HW_LOG_COMPONENT_CLIENT, "TX timestamps CSV filename: %s", csv_config.tx_csv_filename);
    } else {
    }

    /* Close TX socket */
    close(sockfd);
    
    /* Cleanup and flush CSV buffers */
    if (tx_csv_buffer) {
        csv_buffer_destroy(tx_csv_buffer);
    }
    if (csv_buffer) {
        csv_buffer_destroy(csv_buffer);
    }

    /* Cleanup signal-based PPS reporting */
    cleanup_stats_reporting_hotpath();

    /* Cleanup packet buffers */
    free(packet_buffers);
    free(msgs);
    free(iovecs);
    
    /* Cleanup TX correlation system */
    cleanup_tx_correlation();
    
    return 0;
}
