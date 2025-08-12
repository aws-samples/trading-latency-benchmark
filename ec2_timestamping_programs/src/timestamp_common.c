/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_common.c
 * 
 * Summary: Implementation of high-performance UDP timestamp measurement infrastructure.
 * 
 * Description: Core implementation providing hardware/software timestamping, TSC-based timing,
 * socket optimization, statistical analysis with percentile calculations, lock-free CSV
 * logging, TX timestamp correlation and performance monitoring. Includes CPU calibration,
 * process scheduling optimization and comprehensive delta analysis framework for sub-microsecond
 * precision timestamp latency measurements.
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
#include <signal.h>
#include <math.h>
#include <stdbool.h>

/* 
 * GLOBAL STATE & CONFIGURATION MANAGEMENT 
 */

/* Global variable for CPU frequency calibration */
double cpu_freq_ghz = 0.0;

/* Global TSC reliability flag for hardware timestamp support */
volatile int g_tsc_reliable = 0;

/* Global atomic statistics counters for packet tracking */
volatile uint64_t g_packets_sent = 0;
volatile uint64_t g_packets_received = 0;
volatile uint64_t g_packets_tx_timestamp_processed = 0;

/* Global statistics state for PPS reporting */
stats_state_t g_stats_state = {0};
volatile time_t program_start_time = 0;

/* External references to stats configurations */
extern stats_config_t stats_config;
extern stats_collector_t *global_stats_collector;

/*
 * CORE UTILITY FUNCTIONS
*/

/* TSC to timespec conversion with zero-value handling */
struct __kernel_timespec tsc_to_timespec(uint64_t tsc_cycles) {
    /* Handle zero values for disabled TSC */
    if (tsc_cycles == 0) {
        return (struct __kernel_timespec){0, 0};
    }
    
    /* Get current system time reference point */
    struct timespec ref_time;
    clock_gettime(CLOCK_REALTIME, &ref_time);
    uint64_t ref_tsc = rdtsc();
    
    /* Calculate time difference from TSC difference */
    int64_t tsc_diff = (int64_t)(tsc_cycles - ref_tsc);
    double time_diff_ns = tsc_diff / cpu_freq_ghz;
    
    /* Apply difference to reference time */
    long long total_ns = ref_time.tv_sec * 1000000000LL + ref_time.tv_nsec + (long long)time_diff_ns;
    
    /* Handle negative values */
    if (total_ns < 0) {
        total_ns = 0;
    }
    
    struct __kernel_timespec result;
    result.tv_sec = total_ns / 1000000000LL;
    result.tv_nsec = total_ns % 1000000000LL;
    
    return result;
}

/* Calibrate CPU frequency for timing */
void calibrate_cpu_freq(void) {
    struct timespec start, end;
    uint64_t cycles_start, cycles_end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    cycles_start = rdtsc();
    
    usleep(100000);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    cycles_end = rdtsc();
    
    double time_diff = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    uint64_t cycles_diff = cycles_end - cycles_start;
    
    cpu_freq_ghz = (cycles_diff / time_diff) / 1e9;
    HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Calibrated CPU frequency: %.2f GHz", cpu_freq_ghz);
}

/*
 * PROCESS & SYSTEM OPTIMIZATION
 */

/* Set CPU affinity and real-time priority */
int optimize_process_scheduling(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Could not set CPU affinity");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Process optimized: CPU affinity set to core %d", cpu_id);
    }
    
    /* Set maximum real-time priority */
    struct sched_param param;
    param.sched_priority = 99;
    
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Could not set real-time priority (run as root for better performance)");
        /* Try lower priority */
        param.sched_priority = 50;
        if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
            HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Could not set any real-time priority");
        }
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Real-time priority set to maximum (99)");
    }
    
    /* Lock all memory pages to prevent swapping */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Could not lock memory pages");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Memory pages locked to prevent swapping");
    }
    
    /* Set process name */
    if (prctl(PR_SET_NAME, "hw_timestamp_proc", 0, 0, 0) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Could not set process name");
    }
    
    /* Disable address space randomization for consistent performance */
    if (prctl(PR_SET_SECCOMP, 0, 0, 0, 0) != 0) {
    }
    
    return 0;
}

/* Setup realtime thread attributes (common function) */
int setup_realtime_thread_attributes(pthread_attr_t *attr, int priority, const char *context) {
    int result = pthread_attr_init(attr);
    if (result != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "Failed to initialize %s thread attributes: %s", 
                    context, strerror(result));
        return -1;
    }
    
    /* Set scheduling policy to FIFO */
    result = pthread_attr_setschedpolicy(attr, SCHED_FIFO);
    if (result != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to set %s thread scheduling policy: %s", 
                   context, strerror(result));
        pthread_attr_destroy(attr);
        return -1;
    }
    
    /* Set priority */
    struct sched_param sched_param;
    sched_param.sched_priority = priority;
    result = pthread_attr_setschedparam(attr, &sched_param);
    if (result != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to set %s thread priority %d: %s", 
                   context, priority, strerror(result));
        pthread_attr_destroy(attr);
        return -1;
    }
    
    /* Set explicit scheduling inheritance */
    result = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
    if (result != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to set %s thread scheduling inheritance: %s", 
                   context, strerror(result));
        pthread_attr_destroy(attr);
        return -1;
    }
    
    return 0;
}

/* Create realtime thread with CPU affinity */
int create_realtime_thread(pthread_t *thread, void *(*thread_func)(void *), 
                          void *thread_data, int cpu_core, int priority, 
                          const char *thread_name) {
    pthread_attr_t attr;
    
    /* Setup realtime attributes */
    if (setup_realtime_thread_attributes(&attr, priority, thread_name) != 0) {
        return -1;
    }
    
    /* Create thread */
    int result = pthread_create(thread, &attr, thread_func, thread_data);
    
    /* Clean up attributes */
    pthread_attr_destroy(&attr);
    
    if (result != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "Failed to create %s thread: %s", 
                    thread_name, strerror(result));
        return -1;
    }
    
    /* Set CPU affinity after thread creation */
    if (cpu_core >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        
        if (pthread_setaffinity_np(*thread, sizeof(cpuset), &cpuset) == 0) {
            HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "%s thread bound to CPU core %d with priority %d", 
                        thread_name, cpu_core, priority);
        } else {
            HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to bind %s thread to CPU core %d", 
                       thread_name, cpu_core);
        }
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "%s thread created with priority %d", 
                    thread_name, priority);
    }
    
    return 0;
}

/*
 * NETWORK SOCKET OPTIMIZATION
 */

/* Optimize socket performance */
int optimize_socket_performance(int sockfd, int cpu_id, int is_tx_socket) {
    int opt = 1;
    
    /* Large socket buffers */
    int sndbuf_size = MAX_SOCKET_BUFFER;
    int rcvbuf_size = MAX_SOCKET_BUFFER;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set send buffer size");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "Send buffer set to %dMB", sndbuf_size / (1024*1024));
    }
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set receive buffer size");
    }
    
    /* Enable socket reuse and load balancing */
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set SO_REUSEADDR");
    }
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set SO_REUSEPORT");
    }
    
    /* Set maximum priority for network stack processing */
    int priority = 7;
    if (setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set socket priority");
    }
    
    /* Bind socket processing to specific CPU core */
    if (setsockopt(sockfd, SOL_SOCKET, SO_INCOMING_CPU, &cpu_id, sizeof(cpu_id)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set incoming CPU");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "Socket bound to CPU core %d", cpu_id);
    }
    
    /* Enable busy polling */
    int busy_poll_us = 50;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us)) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not enable busy polling");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "Busy polling enabled (%d us)", busy_poll_us);
    }
    
    /* Try to enable zero-copy transmission for TX */
    if (is_tx_socket) {
        if (setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &opt, sizeof(opt)) < 0) {
            HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not enable zero-copy (kernel may not support it)");
        } else {
            HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "Zero-copy transmission enabled");
        }
    }
    
    /* Set socket to non-blocking mode */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "Setting non-blocking failed");
        return -1;
    }
    
    /* Set socket to close-on-exec for cleaner process management */
    if (fcntl(sockfd, F_SETFD, FD_CLOEXEC) == -1) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set close-on-exec");
    }
    
    return 0;
}

/* Setup TX timestamping on socket */
int setup_tx_timestamping(int sockfd) {
    int timestamp_flags = 
        SOF_TIMESTAMPING_TX_SOFTWARE |
        SOF_TIMESTAMPING_SOFTWARE;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_TIMESTAMPING_NEW, &timestamp_flags, sizeof(timestamp_flags)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "TX timestamping setup failed");
        return -1;
    }
    return 0;
}

/* Setup RX timestamping on socket */
int setup_timestamping(int sockfd) {
    int timestamp_flags = 
        SOF_TIMESTAMPING_RX_HARDWARE |
        SOF_TIMESTAMPING_RX_SOFTWARE |
        SOF_TIMESTAMPING_RAW_HARDWARE |
        SOF_TIMESTAMPING_SOFTWARE;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_TIMESTAMPING_NEW, &timestamp_flags, sizeof(timestamp_flags)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "SO_TIMESTAMPING_NEW failed");
        return -1;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "Timestamping flags set: 0x%x", timestamp_flags);
    return 0;
}

/*
 * STATISTICS & PERFORMANCE MONITORING
 */

/* Hot path optimized PPS signal handler */
void stats_signal_handler_hotpath(int sig __attribute__((unused))) {
    
    /* All operations are atomic/lock-free */
    uint64_t current_sent = __atomic_load_n(&g_packets_sent, __ATOMIC_RELAXED);
    uint64_t current_received = __atomic_load_n(&g_packets_received, __ATOMIC_RELAXED);
    time_t current_time = time(NULL);
    
    time_t time_diff = current_time - g_stats_state.last_time;
    if (time_diff > 0 && g_stats_state.last_time > 0) {
        uint64_t sent_diff = current_sent - g_stats_state.last_sent;
        uint64_t received_diff = current_received - g_stats_state.last_received;
        
        /* Only calculate if there is activity */
        if (sent_diff > 0 || received_diff > 0) {
            g_stats_state.current_sent_pps = sent_diff / time_diff;
            g_stats_state.current_received_pps = received_diff / time_diff;
            
            /* Signal main thread that stats are ready (atomic flag) */
            __atomic_store_n(&g_stats_state.stats_ready, 1, __ATOMIC_RELEASE);
        }
    }
    
    /* Update state for next interval */
    g_stats_state.last_sent = current_sent;
    g_stats_state.last_received = current_received;
    g_stats_state.last_time = current_time;
    
    alarm(1); /* Schedule next signal */
}

/* Main thread PPS stats display function (called from hot path time checks) */
void display_stats_if_ready(void) {
    if (__atomic_load_n(&g_stats_state.stats_ready, __ATOMIC_ACQUIRE)) {
        uint64_t sent_pps = g_stats_state.current_sent_pps;
        uint64_t received_pps = g_stats_state.current_received_pps;
        
        if (sent_pps > 0 || received_pps > 0) {
            time_t elapsed = time(NULL) - program_start_time;
            
            /* Auto-detect mode based on active counters */
            if (sent_pps > 0 && received_pps > 0) {
                printf("Duration: [%ld] - TX PPS: %lu - RX PPS: %lu\n", 
                       elapsed, (unsigned long)sent_pps, (unsigned long)received_pps);
            } else if (sent_pps > 0) {
                printf("Duration: [%ld] - TX PPS: %lu\n", 
                       elapsed, (unsigned long)sent_pps);
            } else {
                printf("Duration: [%ld] - RX PPS: %lu\n", 
                       elapsed, (unsigned long)received_pps);
            }
            fflush(stdout); /* Immediate display */
        }
        
        /* Reset flag atomically */
        __atomic_store_n(&g_stats_state.stats_ready, 0, __ATOMIC_RELAXED);
    }
}

/* PPS stats setup function */
void setup_stats_reporting_hotpath(void) {
    program_start_time = time(NULL);
    memset((void*)&g_stats_state, 0, sizeof(g_stats_state));
    g_stats_state.last_time = program_start_time;
    
    signal(SIGALRM, stats_signal_handler_hotpath);
    alarm(1); /* Start the timer */
    HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Hot-path optimized PPS reporting enabled");
}

/* PPS stats cleanup function */
void cleanup_stats_reporting_hotpath(void) {
    alarm(0);
    signal(SIGALRM, SIG_DFL); /* Reset to default handler */
    HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Hot-path optimized PPS reporting disabled");
}

/*
 * TX TIMESTAMP PROCESSING SYSTEM
 */

/* Kernel error queue health monitoring */
int monitor_error_queue_health(int sockfd) {
    /* Check for socket errors, specifically error queue overflow */
    int socket_error = 0;
    socklen_t len = sizeof(socket_error);
    
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0) {
        if (socket_error == ENOBUFS) {
            HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Error queue buffer overflow detected - increase TX timestamp processing frequency");
            return -1; /* Error queue overflow detected */
        } else if (socket_error != 0) {
            HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Socket error detected: %s", strerror(socket_error));
            return -2; /* Other socket error */
        }
    }
    
    /* Try to peek at error queue without consuming */
    struct msghdr peek_msg;
    char dummy_data[64];
    char dummy_control[256];
    struct iovec peek_iov = {.iov_base = dummy_data, .iov_len = sizeof(dummy_data)};
    
    peek_msg.msg_iov = &peek_iov;
    peek_msg.msg_iovlen = 1;
    peek_msg.msg_control = dummy_control;
    peek_msg.msg_controllen = sizeof(dummy_control);
    peek_msg.msg_name = NULL;
    peek_msg.msg_namelen = 0;
    
    /* Peek at error queue (non-blocking, non-consuming) */
    int ret = recvmsg(sockfd, &peek_msg, MSG_DONTWAIT | MSG_ERRQUEUE | MSG_PEEK);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* Error queue is empty (normal) */
        } else if (errno == ENOBUFS) {
            HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Error queue overflow detected during peek");
            return -1; /* Error queue overflow */
        }
    }
    
    return 0;
}

/* TX timestamp processing thread implementation */
void* tx_timestamp_processing_thread(void* arg) {
    tx_timestamp_thread_data_t *data = (tx_timestamp_thread_data_t*)arg;
    
    if (!data) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "TX timestamp thread: data is NULL");
        return NULL;
    }
    
    /* Bind to CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(data->cpu_core, &cpuset);
    
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "TX timestamp thread bound to CPU core %d", data->cpu_core);
    } else {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to bind TX timestamp thread to CPU core %d", data->cpu_core);
    }
    
    /* Set thread name */
    if (prctl(PR_SET_NAME, "hw_ts_tx_proc", 0, 0, 0) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Could not set TX timestamp thread name");
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "TX timestamp processing thread started (%d us polling)", data->polling_interval_us);
    
    /* High-frequency processing loop */
    while (data->running) {
        /* Process TX timestamps from error queue */
        if (data->sockfd >= 0) {
            int processed = process_tx_timestamps_in_main_thread(data->sockfd);
            if (processed > 0) {
                __atomic_fetch_add(&g_packets_tx_timestamp_processed, processed, __ATOMIC_RELAXED);
            }
            
            /* Monitor error queue health during processing */
            int queue_health = monitor_error_queue_health(data->sockfd);
            if (queue_health < 0) {
                HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "TX timestamp thread detected error queue issues");
                /* Continue processing but log the issue */
            }
        }
        
        /* Sleep for busy poll interval */
        usleep(data->polling_interval_us);
    }
    
    /* Final processing before thread termination */
    if (data->sockfd >= 0) {
        int total_processed = 0;
        int batch_processed;
        do {
            batch_processed = process_tx_timestamps_in_main_thread(data->sockfd);
            total_processed += batch_processed;
        } while (batch_processed > 0);
        
    }
    return NULL;
}

/* Start TX timestamp processing thread */
int start_tx_timestamp_processing_thread(int sockfd, int cpu_core, pthread_t *thread, tx_timestamp_thread_data_t **thread_data) {
    if (!thread || !thread_data) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "Invalid thread or thread_data pointer");
        return -1;
    }
    
    *thread_data = malloc(sizeof(tx_timestamp_thread_data_t));
    if (!*thread_data) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_NETWORK, "Failed to allocate TX timestamp thread data");
        return -1;
    }
    
    (*thread_data)->sockfd = sockfd;
    (*thread_data)->running = 1;
    (*thread_data)->cpu_core = cpu_core;
    (*thread_data)->polling_interval_us = 500;
    
    /* Create TX timestamp processing thread with realtime attributes */
    int result = create_realtime_thread(thread, tx_timestamp_processing_thread, *thread_data, cpu_core, 99, "TX Timestamp");
    if (result != 0) {
        free(*thread_data);
        *thread_data = NULL;
        return -1;
    }
    
    /* Give the thread a moment to start */
    usleep(1000);
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "TX timestamp processing thread started successfully with realtime priority 99");
    return 0;
}

/* Stop TX timestamp processing thread */
void stop_tx_timestamp_processing_thread(pthread_t thread, tx_timestamp_thread_data_t *thread_data) {
    if (!thread_data) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "TX timestamp thread data is NULL");
        return;
    }
    
    thread_data->running = 0;
    
    /* Wait for thread to finish with timeout */
    struct timespec timeout_spec;
    clock_gettime(CLOCK_REALTIME, &timeout_spec);
    timeout_spec.tv_sec += 2;
    
    int join_result = pthread_timedjoin_np(thread, NULL, &timeout_spec);
    if (join_result == 0) {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_NETWORK, "TX timestamp processing thread joined successfully");
    } else if (join_result == ETIMEDOUT) {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "TX timestamp thread join timed out, canceling thread");
        pthread_cancel(thread);
        pthread_join(thread, NULL);
    } else {
        HW_LOG_WARN(HW_LOG_COMPONENT_NETWORK, "Failed to join TX timestamp thread: %s", strerror(join_result));
    }
    
    free(thread_data);
}

/*
 * CSV LOGGING INFRASTRUCTURE
 */

/* Mode-aware CSV functions implementation */
const char* get_csv_header(csv_type_t type) {
    switch (type) {
        case CSV_CLIENT_MAIN_ONEWAY:
            return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_ts\n";
        case CSV_CLIENT_MAIN_ROUNDTRIP:
            return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_tsc_ts,clt_app_tx_ts,clt_hw_rx_ts,clt_ker_rx_ts,clt_app_rx_tsc_ts,clt_app_rx_ts\n";
        case CSV_CLIENT_TX:
            return "clt_src_ip,clt_src_port,seq_num,clt_ker_tx_ts\n";
        case CSV_SERVER_MAIN_ONEWAY:
            return "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts\n";
        case CSV_SERVER_MAIN_ROUNDTRIP:
            return "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts,svr_app_tx_ts\n";
        case CSV_SERVER_TX:
            return "clt_src_ip,clt_src_port,seq_num,svr_ker_tx_ts\n";
        default:
            return "invalid_csv_type\n";
    }
}

/* Create CSV ring buffer with dedicated I/O thread */
csv_ring_buffer_t* csv_buffer_create(uint32_t size, const char *filename, csv_type_t csv_type, uint32_t batch_size, int log_cpu) {
    if (size == 0 || (size & (size - 1)) != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Buffer size must be power of 2");
        return NULL;
    }
    
    csv_ring_buffer_t *buffer = aligned_alloc(64, sizeof(csv_ring_buffer_t));
    if (!buffer) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to allocate CSV buffer");
        return NULL;
    }
    
    /* Allocate packet entries buffer (cache-aligned) */
    buffer->entries = aligned_alloc(64, size * sizeof(csv_entry_t));
    if (!buffer->entries) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to allocate CSV entries buffer");
        free(buffer);
        return NULL;
    }
    
    buffer->head = 0;
    buffer->tail = 0;
    buffer->size_mask = size - 1;
    buffer->batch_size = batch_size;
    buffer->running = 1;
    buffer->csv_type = csv_type;
    buffer->log_cpu = log_cpu;
    
    /* Allocate write buffer (for formatting batches) */
    buffer->write_buffer_size = batch_size * 256;
    buffer->write_buffer = malloc(buffer->write_buffer_size);
    if (!buffer->write_buffer) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to allocate write buffer");
        free(buffer->entries);
        free(buffer);
        return NULL;
    }
    
    buffer->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (buffer->fd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to open CSV file: %s", filename);
        free(buffer->write_buffer);
        free(buffer->entries);
        free(buffer);
        return NULL;
    }
    
    /* Write CSV header */
    const char *header = get_csv_header(csv_type);
    if (write(buffer->fd, header, strlen(header)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to write CSV header");
    }
    
    /* Start dedicated I/O thread */
    int thread_result = pthread_create(&buffer->writer_thread, NULL, csv_writer_thread, buffer);
    
    if (thread_result != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to create CSV writer thread: %s", strerror(thread_result));
        close(buffer->fd);
        free(buffer->write_buffer);
        free(buffer->entries);
        free(buffer);
        return NULL;
    }
    
    /* Give the thread a moment to start */
    usleep(1000);
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CSV, "CSV buffer created: size=%u, batch=%u, file=%s", size, batch_size, filename);
    return buffer;
}

/* Destroy CSV ring buffer */
void csv_buffer_destroy(csv_ring_buffer_t *buffer) {
    if (!buffer) return;
    
    buffer->running = 0;
    
    /* Wait for thread to finish */
    if (pthread_join(buffer->writer_thread, NULL) != 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_CSV, "Failed to join CSV writer thread");
    }
    
    if (buffer->fd >= 0) {
        fsync(buffer->fd);
        close(buffer->fd);
    }
    
    if (buffer->write_buffer) free(buffer->write_buffer);
    if (buffer->entries) free(buffer->entries);
    free(buffer);
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CSV, "CSV buffer destroyed");
}

/* Lock-free batch dequeue for I/O thread */
int csv_dequeue_batch(csv_ring_buffer_t *buffer, csv_entry_t *batch, int max_entries) {
    int count = 0;
    
    while (count < max_entries) {
        uint32_t current_tail = __atomic_load_n(&buffer->tail, __ATOMIC_RELAXED);
        uint32_t current_head = __atomic_load_n(&buffer->head, __ATOMIC_ACQUIRE);
        
        /* Check if buffer empty */
        if (current_tail == current_head) {
            break;
        }
        
        /* Copy entry */
        batch[count] = buffer->entries[current_tail];
        count++;
        
        /* Update tail pointer */
        __atomic_store_n(&buffer->tail, (current_tail + 1) & buffer->size_mask, __ATOMIC_RELAXED);
    }
    
    return count;
}

/* Format batch of CSV entries into write buffer */
void csv_format_batch(const csv_entry_t *batch, int count, char *buffer, size_t buffer_size, csv_type_t csv_type) {
    size_t offset = 0;
    
    buffer[0] = '\0';
    
    for (int i = 0; i < count && offset < buffer_size - 256; i++) {
        const csv_entry_t *entry = &batch[i];
        int len = 0;
        
        switch (csv_type) {
            case CSV_CLIENT_MAIN_ONEWAY:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[1] / 1000000000ULL, entry->timestamp_ns[1] % 1000000000ULL);
                break;
                
            case CSV_CLIENT_MAIN_ROUNDTRIP:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu,%llu.%09llu,%llu.%09llu,%llu.%09llu,%llu.%09llu,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[0] / 1000000000ULL, entry->timestamp_ns[0] % 1000000000ULL,
                    entry->timestamp_ns[1] / 1000000000ULL, entry->timestamp_ns[1] % 1000000000ULL,
                    entry->timestamp_ns[3] / 1000000000ULL, entry->timestamp_ns[3] % 1000000000ULL,
                    entry->timestamp_ns[4] / 1000000000ULL, entry->timestamp_ns[4] % 1000000000ULL,
                    entry->timestamp_ns[5] / 1000000000ULL, entry->timestamp_ns[5] % 1000000000ULL,
                    entry->timestamp_ns[6] / 1000000000ULL, entry->timestamp_ns[6] % 1000000000ULL);
                break;
                
            case CSV_SERVER_MAIN_ONEWAY:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu,%llu.%09llu,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[7] / 1000000000ULL, entry->timestamp_ns[7] % 1000000000ULL,
                    entry->timestamp_ns[8] / 1000000000ULL, entry->timestamp_ns[8] % 1000000000ULL,
                    entry->timestamp_ns[9] / 1000000000ULL, entry->timestamp_ns[9] % 1000000000ULL);
                break;
                
            case CSV_SERVER_MAIN_ROUNDTRIP:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu,%llu.%09llu,%llu.%09llu,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[7] / 1000000000ULL, entry->timestamp_ns[7] % 1000000000ULL,
                    entry->timestamp_ns[8] / 1000000000ULL, entry->timestamp_ns[8] % 1000000000ULL,
                    entry->timestamp_ns[9] / 1000000000ULL, entry->timestamp_ns[9] % 1000000000ULL,
                    entry->timestamp_ns[10] / 1000000000ULL, entry->timestamp_ns[10] % 1000000000ULL);
                break;
                
            case CSV_CLIENT_TX:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[2] / 1000000000ULL, entry->timestamp_ns[2] % 1000000000ULL);
                break;
                
            case CSV_SERVER_TX:
                len = snprintf(buffer + offset, buffer_size - offset,
                    "%s,%u,%u,%llu.%09llu\n",
                    entry->src_ip, entry->src_port, entry->seq_num,
                    entry->timestamp_ns[11] / 1000000000ULL, entry->timestamp_ns[11] % 1000000000ULL);
                break;
                
            default:
                len = snprintf(buffer + offset, buffer_size - offset, "invalid_csv_type\n");
                break;
        }
        
        if (len > 0) {
            offset += len;
        }
    }
    
    /* Ensure null termination (safety check) */
    if (offset < buffer_size) {
        buffer[offset] = '\0';
    } else {
        buffer[buffer_size - 1] = '\0';
    }
}

/* Dedicated CSV writer thread */
void* csv_writer_thread(void* arg) {
    csv_ring_buffer_t *buffer = (csv_ring_buffer_t*)arg;
    
    if (!buffer) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "CSV writer thread: buffer is NULL");
        return NULL;
    }
    
    /* Use dedicated CPU core */
    cpu_set_t csv_cpuset;
    CPU_ZERO(&csv_cpuset);
    CPU_SET(buffer->log_cpu, &csv_cpuset);
    
    if (pthread_setaffinity_np(pthread_self(), sizeof(csv_cpuset), &csv_cpuset) == 0) {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_CSV, "CSV writer thread bound to CPU core %d", buffer->log_cpu);
    } else {
        HW_LOG_WARN(HW_LOG_COMPONENT_CSV, "Failed to bind CSV writer thread to CPU core %d", buffer->log_cpu);
    }
    
    csv_entry_t *batch = malloc(buffer->batch_size * sizeof(csv_entry_t));
    
    if (!batch) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to allocate batch buffer in CSV writer thread");
        return NULL;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CSV, "CSV writer thread started (batch_size=%u)", buffer->batch_size);
    
    while (buffer->running) {
        /* Batch dequeue entries */
        int count = csv_dequeue_batch(buffer, batch, buffer->batch_size);
        
        if (count > 0) {
            /* Format entire batch */
            csv_format_batch(batch, count, buffer->write_buffer, buffer->write_buffer_size, buffer->csv_type);
            
            /* Single write call for entire batch */
            size_t data_len = strlen(buffer->write_buffer);
            if (write(buffer->fd, buffer->write_buffer, data_len) < 0) {
                HW_LOG_WARN(HW_LOG_COMPONENT_CSV, "CSV batch write failed");
            } else {
                fsync(buffer->fd);
            }
        } else {
            /* No data available - brief sleep to avoid busy waiting */
            usleep(10);
        }
    }
    
    /* Final flush - process any remaining entries */
    int final_count = csv_dequeue_batch(buffer, batch, buffer->batch_size);
    if (final_count > 0) {
        csv_format_batch(batch, final_count, buffer->write_buffer, buffer->write_buffer_size, buffer->csv_type);
        size_t data_len = strlen(buffer->write_buffer);
        write(buffer->fd, buffer->write_buffer, data_len);
        fsync(buffer->fd);
    }
    
    free(batch);
    HW_LOG_DEBUG(HW_LOG_COMPONENT_CSV, "CSV writer thread terminated");
    return NULL;
}

/*
 * STATISTICAL ANALYSIS ENGINE
 */

/* Create stats collector with memory management */
stats_collector_t* create_stats_collector(uint32_t buffer_size, stats_mode_type_t mode) {
    if (buffer_size == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Buffer size must be greater than 0");
        return NULL;
    }
    
    /* Allocate main control structure with 64-byte alignment */
    stats_collector_t *stats = aligned_alloc(64, sizeof(stats_collector_t));
    if (!stats) {
        stats = calloc(1, sizeof(stats_collector_t));
        if (!stats) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to allocate stats collector (both aligned and standard allocation failed)");
            return NULL;
        }
        HW_LOG_WARN(HW_LOG_COMPONENT_STATS, "Using standard allocation for stats collector (aligned_alloc not available)");
    } else {
        memset(stats, 0, sizeof(stats_collector_t));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Using 64-byte aligned stats collector allocation");
    }
    
    /* Ensure buffer size is power of 2 for efficient modulo operations */
    uint32_t actual_size = next_power_of_2(buffer_size);
    if (actual_size == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Buffer size too large, overflow detected");
        free(stats);
        return NULL;
    }
    
    if (actual_size != buffer_size) {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Buffer size adjusted from %u to %u (next power of 2)", buffer_size, actual_size);
    }
    
    stats->buffer = aligned_alloc(64, actual_size * sizeof(stats_entry_t));
    if (!stats->buffer) {
        stats->buffer = calloc(actual_size, sizeof(stats_entry_t));
        if (!stats->buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to allocate stats buffer (%u entries, %lu MB total)", 
                    actual_size, (actual_size * sizeof(stats_entry_t)) / (1024 * 1024));
            free(stats);
            return NULL;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Using standard allocation for stats buffer (aligned_alloc not available)");
    } else {
        memset(stats->buffer, 0, actual_size * sizeof(stats_entry_t));
        HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Using 64-byte aligned stats buffer allocation");
    }
    
    /* Initialize control structure */
    stats->head = 0;
    stats->tail = 0;
    stats->size_mask = actual_size - 1;
    stats->capacity = actual_size;
    stats->program_mode = (uint8_t)mode;
    stats->total_entries = 0;
    stats->dropped_entries = 0;
    stats->tx_correlation_context = NULL;
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Stats collector created: %u entries (%lu MB), mode=%d", 
           actual_size, (actual_size * sizeof(stats_entry_t)) / (1024 * 1024), mode);
    
    return stats;
}

/* Destroy statistics collector and cleanup memory */
void destroy_stats_collector(stats_collector_t *stats) {    
    if (stats->buffer) {
        free(stats->buffer);
        stats->buffer = NULL;
    }
    
    memset(stats, 0, sizeof(stats_collector_t));
    
    free(stats);
    
}

/* Update existing stats buffer entry with TX timestamp correlation */
void update_stats_buffer_with_tx_timestamp(uint32_t seq_num, uint64_t ker_tx_ts_ns, stats_mode_type_t mode) {    
    if (!stats_config.enabled || !global_stats_collector || !global_stats_collector->buffer) {
        return;
    }
    
    /* Search buffer for matching sequence number (linear search through active entries) */
    uint32_t current_tail = __atomic_load_n(&global_stats_collector->tail, __ATOMIC_ACQUIRE);
    uint32_t current_head = __atomic_load_n(&global_stats_collector->head, __ATOMIC_ACQUIRE);
    uint32_t buffer_count = (current_head - current_tail) & global_stats_collector->size_mask;
    
    /* Search backwards from head (most recent entries first) */
    for (uint32_t i = 0; i < buffer_count; i++) {
        uint32_t index = (current_head - 1 - i) & global_stats_collector->size_mask;
        stats_entry_t *entry = &global_stats_collector->buffer[index];
        
        if (entry->seq_num == seq_num) {
            /* Found matching entry - update with TX timestamp based on mode */
            switch (mode) {
                /* Client TX timestamp */
                case STATS_CLIENT_ONEWAY:
                case STATS_CLIENT_ROUNDTRIP:
                    entry->timestamp_ns[2] = ker_tx_ts_ns;
                    break;
                case STATS_SERVER_ONEWAY:
                /* No TX timestamps in server one-way mode */
                    break;    
                case STATS_SERVER_ROUNDTRIP:
                /* Server TX timestamp */
                    entry->timestamp_ns[11] = ker_tx_ts_ns;
                    break;
            }
            return;  /* Found and updated, exit search */
        }
    }
    /* Entry not found - this can happen and is normal depending on PPS rates or driver/kernel behavior */
}

/* Update existing stats buffer entry with application TX timestamp correlation */
void update_stats_buffer_with_app_tx_timestamp(uint32_t seq_num, uint64_t app_tx_ts_ns, 
                                              uint64_t tsc_tx_ts_ns, stats_mode_type_t mode) {
    
    if (!stats_config.enabled || !global_stats_collector || !global_stats_collector->buffer) {
        return;
    }
    
    /* Search buffer for matching sequence number (linear search through active entries) */
    uint32_t current_tail = __atomic_load_n(&global_stats_collector->tail, __ATOMIC_ACQUIRE);
    uint32_t current_head = __atomic_load_n(&global_stats_collector->head, __ATOMIC_ACQUIRE);
    uint32_t buffer_count = (current_head - current_tail) & global_stats_collector->size_mask;
    
    /* Search backwards from head (most recent entries first) */
    for (uint32_t i = 0; i < buffer_count; i++) {
        uint32_t index = (current_head - 1 - i) & global_stats_collector->size_mask;
        stats_entry_t *entry = &global_stats_collector->buffer[index];
        
        if (entry->seq_num == seq_num) {
            /* Found matching entry - update with app TX timestamp based on mode */
            switch (mode) {
                case STATS_CLIENT_ONEWAY:
                    entry->timestamp_ns[1] = app_tx_ts_ns;
                    break;
                case STATS_CLIENT_ROUNDTRIP:
                    if (tsc_tx_ts_ns > 0) entry->timestamp_ns[0] = tsc_tx_ts_ns;
                    entry->timestamp_ns[1] = app_tx_ts_ns;
                    break;
                case STATS_SERVER_ONEWAY:
                    /* No TX timestamps in server one-way mode */
                    break;    
                case STATS_SERVER_ROUNDTRIP:
                    /* Server app TX timestamp */
                    entry->timestamp_ns[10] = app_tx_ts_ns;
                    break;
            }
            return;  /* Found and updated, exit search */
        }
    }
}

/* Update existing stats buffer entry with RX timestamps correlation */
void update_stats_buffer_with_rx_timestamps(uint32_t seq_num, uint64_t hw_rx_ts_ns, uint64_t ker_rx_ts_ns, 
                                           uint64_t app_rx_ts_ns, uint64_t tsc_rx_ts_ns, stats_mode_type_t mode) {
    
    if (!stats_config.enabled || !global_stats_collector || !global_stats_collector->buffer) {
        return;
    }
    
    /* Search buffer for matching sequence number */
    uint32_t current_tail = __atomic_load_n(&global_stats_collector->tail, __ATOMIC_ACQUIRE);
    uint32_t current_head = __atomic_load_n(&global_stats_collector->head, __ATOMIC_ACQUIRE);
    uint32_t buffer_count = (current_head - current_tail) & global_stats_collector->size_mask;
    
    /* Search backwards from head (most recent entries first) */
    for (uint32_t i = 0; i < buffer_count; i++) {
        uint32_t index = (current_head - 1 - i) & global_stats_collector->size_mask;
        stats_entry_t *entry = &global_stats_collector->buffer[index];
        
        if (entry->seq_num == seq_num) {
            /* Found matching entry - update with RX timestamps based on mode */
            switch (mode) {
                case STATS_CLIENT_ONEWAY:
                    /* No RX timestamps in client one-way mode */
                    break;
                case STATS_CLIENT_ROUNDTRIP:
                    entry->timestamp_ns[3] = hw_rx_ts_ns;
                    entry->timestamp_ns[4] = ker_rx_ts_ns;
                    if (tsc_rx_ts_ns > 0) entry->timestamp_ns[5] = tsc_rx_ts_ns;
                    entry->timestamp_ns[6] = app_rx_ts_ns;
                    break;
                case STATS_SERVER_ONEWAY:
                case STATS_SERVER_ROUNDTRIP:
                    /* Server RX timestamps */
                    entry->timestamp_ns[7] = hw_rx_ts_ns;
                    entry->timestamp_ns[8] = ker_rx_ts_ns;
                    entry->timestamp_ns[9] = app_rx_ts_ns;
                    break;
            }
            return;  /* Found and updated, exit search */
        }
    }
}

/* Create minimal stats entry with only basic identification fields */
void create_minimal_stats_entry(uint32_t seq_num, uint16_t src_port, const char* src_ip __attribute__((unused)),
                               timestamp_mode_t entry_type, stats_mode_type_t mode __attribute__((unused))) {
    
    if (!stats_config.enabled || !global_stats_collector) {
        return;
    }
    
    /* Create stats entry directly */
    stats_entry_t stats_entry;
    stats_entry.seq_num = seq_num;
    stats_entry.src_port = src_port;
    stats_entry.entry_type = (uint8_t)entry_type;
    stats_entry.timestamp_mask = get_timestamp_mask_for_mode(entry_type);
    
    /* Initialize all timestamps to zero - will be updated separately */
    memset(stats_entry.timestamp_ns, 0, sizeof(stats_entry.timestamp_ns));
    memset(stats_entry.padding, 0, sizeof(stats_entry.padding));
    
    /* Send directly to stats system */
    stats_enqueue(global_stats_collector, &stats_entry);
}

/* All possible deltas and trip times across all modes */
const delta_definition_t ALL_DELTAS[12] = {
    /* Client deltas (indices 0-5) */
    {"D1: CAT (T1) -> CKT (T2)", "Client Application TX (T1) -> Client Kernel TX (T2)", 1, 2, 0x03},                  /* Client modes (one-way + round-trip) */
    {"D6: CHR (T8) -> CKR (T9)", "Client Hardware RX (T8) -> Client Kernel RX (T9)", 3, 4, 0x02},                     /* Client round-trip only */
    {"D7: CKR (T9) -> CAR (T10)", "Client Kernel RX (T9) -> Client Application RX (T10)", 4, 6, 0x02},                /* Client round-trip only */
    {"RTT D2: CAT (T1) -> CAR (T10)", "Client Application TX (T1) -> Client Application RX (T10)", 1, 6, 0x02},           /* Client round-trip only (end-to-end) */
    {"RTT D3: CATT (T1) -> CART (T10)", "Client Application TX TSC (T1) -> Client Application RX TSC (T10)", 0, 5, 0x02}, /* Client round-trip only (TSC end-to-end) */
    {"RTT D1: CAT (T1) -> CHR (T8)", "Client Application TX (T1) -> Client Hardware RX (T8)", 1, 3, 0x02},                /* Client round-trip only */
    
    /* Server deltas (indices 6-11) */
    {"D2: SHR (T3) -> SKR (T4)", "Server Hardware RX (T3) -> Server Kernel RX (T4)", 7, 8, 0x0C},             /* Server modes (one-way + round-trip) */
    {"D3: SKR (T4) -> SAR (T5)", "Server Kernel RX (T4) -> Server Application RX (T5)", 8, 9, 0x0C},          /* Server modes (one-way + round-trip) */
    {"D4: SAR (T5) -> SAT (T6)", "Server Application RX (T5) -> Server Application TX (T6)", 9, 10, 0x08},    /* Server round-trip only */
    {"D5: SAT (T6) -> SKT (T7)", "Server Application TX (T6) -> Server Kernel TX (T7)", 10, 11, 0x08},        /* Server round-trip only */
    {"TT D2: SHR (T3) -> SKT (T7)", "Server Hardware RX (T3) -> Server Kernel TX (T7)", 7, 11, 0x08},            /* Server round-trip only (end-to-end in server) */
    {"TT D1: SHR (T3) -> SAR (T5)", "Server Hardware RX (T3) -> Server Application RX (T5)", 7, 9, 0x0C}         /* Server modes (one-way + round-trip) */
};

/* Mode-specific delta index arrays */
const uint8_t CLIENT_ONEWAY_DELTAS[1] = {0};
const uint8_t CLIENT_ROUNDTRIP_DELTAS[6] = {0, 1, 2, 5, 3, 4};
const uint8_t SERVER_ONEWAY_DELTAS[3] = {6, 7, 11};
const uint8_t SERVER_ROUNDTRIP_DELTAS[6] = {6, 7, 8, 9, 11, 10};

/* Configure analysis for specific program mode */
void configure_analysis_for_mode(stats_analysis_result_t *result, stats_mode_type_t mode, stats_config_t *config) {
    if (!result || !config) {
        return;
    }
    
    /* Initialize result structure */
    memset(result, 0, sizeof(stats_analysis_result_t));
    result->mode = mode;
    result->config = *config;
    
    /* Configure active deltas based on mode */
    switch (mode) {
        case STATS_CLIENT_ONEWAY:
            result->delta_count = 1;
            for (int i = 0; i < result->delta_count; i++) {
                result->active_deltas[i] = CLIENT_ONEWAY_DELTAS[i];
            }
            break;
            
        case STATS_CLIENT_ROUNDTRIP:
            result->delta_count = 6;
            for (int i = 0; i < result->delta_count; i++) {
                result->active_deltas[i] = CLIENT_ROUNDTRIP_DELTAS[i];
            }
            break;
            
        case STATS_SERVER_ONEWAY:
            result->delta_count = 3;
            for (int i = 0; i < result->delta_count; i++) {
                result->active_deltas[i] = SERVER_ONEWAY_DELTAS[i];
            }
            break;
            
        case STATS_SERVER_ROUNDTRIP:
            result->delta_count = 6;
            for (int i = 0; i < result->delta_count; i++) {
                result->active_deltas[i] = SERVER_ROUNDTRIP_DELTAS[i];
            }
            break;
            
        default:
            /* No active deltas */
            result->delta_count = 0;
            break;
    }
    
    /* Initialize delta analysis structures */
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        
        /* Initialize analysis structure */
        analysis->packet_count = 0;
        analysis->histogram = NULL;  /* Allocated during processing */
        analysis->outlier_count = 0;
        analysis->used_bins = 0;
        
        /* Initialize percentiles calculation */
        if (exact_percentiles_init(analysis, config->buffer_size) != 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to initialize percentiles for delta %d", delta_idx);
        }
    }
}

/* Cleanup analysis result structure and free allocated memory */
void cleanup_analysis_result(stats_analysis_result_t *result) {
    if (!result) {
        return;
    }
    
    /* Free allocated memory for each active delta */
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        
        /* Free histogram memory */
        if (analysis->histogram) {
            free(analysis->histogram);
            analysis->histogram = NULL;
        }
        
        /* Reset analysis fields */
        analysis->packet_count = 0;
        analysis->outlier_count = 0;
        analysis->used_bins = 0;
        
        /* Clean up percentiles */
        exact_percentiles_cleanup(analysis);
    }
    
    /* Clear result structure */
    result->delta_count = 0;
    memset(result->active_deltas, 0, sizeof(result->active_deltas));
}

/* Allocate and initialize analysis structures for a specific delta */
static int allocate_analysis_structures_for_delta(delta_analysis_t *analysis, uint32_t max_bins, uint32_t buffer_capacity) {
    if (!analysis || max_bins == 0) {
        return -1;
    }
    
    /* Initialize percentiles calculation */
    exact_percentiles_init(analysis, buffer_capacity);
    
    /* Free existing histogram if already allocated */
    if (analysis->histogram) {
        free(analysis->histogram);
        analysis->histogram = NULL;
    }
    
    /* Allocate histogram array (zero-initialized) */
    analysis->histogram = calloc(max_bins, sizeof(uint32_t));
    if (!analysis->histogram) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to allocate histogram memory (%u bins, %lu bytes)", 
                max_bins, max_bins * sizeof(uint32_t));
        return -1;
    }
    
    /* Initialize counters */
    analysis->packet_count = 0;
    analysis->outlier_count = 0;
    analysis->used_bins = 0;
    
    return 0;
}

/* Allocate histogram analysis structures for all active deltas */
int allocate_analysis_histograms(stats_analysis_result_t *result) {
    if (!result) {
        return -1;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Initializing analysis structures for %d deltas", result->delta_count);
    
    /* Allocate and initialize structures for each active delta */
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        
        if (allocate_analysis_structures_for_delta(analysis, result->config.max_bins, result->config.buffer_size) != 0) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to initialize analysis structures for delta %d", delta_idx);
            
            /* Cleanup what has been allocated so far */
            for (int j = 0; j < i; j++) {
                uint8_t cleanup_delta_idx = result->active_deltas[j];
                delta_analysis_t *cleanup_analysis = &result->deltas[cleanup_delta_idx];
                if (cleanup_analysis->histogram) {
                    free(cleanup_analysis->histogram);
                    cleanup_analysis->histogram = NULL;
                }
            }
            return -1;
        }
    }
    return 0;
}

/* Validate analysis result structure and memory allocation */
int validate_analysis_result(stats_analysis_result_t *result, const char *context) {
    if (!result) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Analysis result is NULL in %s", context ? context : "unknown context");
        return -1;
    }
    
    if (result->delta_count == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "No active deltas configured in %s", context ? context : "unknown context");
        return -1;
    }
    
    if (result->delta_count > 10) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Too many active deltas (%d > 10) in %s", result->delta_count, context ? context : "unknown context");
        return -1;
    }
    
    if (result->config.max_bins == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Max bins is 0 in %s", context ? context : "unknown context");
        return -1;
    }
    
    if (result->config.bin_width_us == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Bin width is 0 in %s", context ? context : "unknown context");
        return -1;
    }
    
    /* Validate each active delta */
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        
        if (delta_idx >= 12) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Invalid delta index %d in %s", delta_idx, context ? context : "unknown context");
            return -1;
        }
        
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        
        /* Check if histogram is allocated when expected */
        if (!analysis->histogram) {
            continue;
        }
    }
    
    return 0;
}

/* Initialize analysis result with safe defaults */
int initialize_analysis_result(stats_analysis_result_t *result, stats_mode_type_t mode, stats_config_t *config) {
    if (!result || !config) {
        return -1;
    }
    
    configure_analysis_for_mode(result, mode, config);
    
    if (validate_analysis_result(result, "initialize_analysis_result") != 0) {
        return -1;
    }
    
    /* Allocate histograms for all active deltas */
    if (allocate_analysis_histograms(result) != 0) {
        cleanup_analysis_result(result);
        return -1;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Analysis result initialized: mode=%d, deltas=%d, bins=%u", 
           mode, result->delta_count, result->config.max_bins);
    
    return 0;
}

/* Reset analysis counters while preserving allocated memory */
void reset_analysis_counters(stats_analysis_result_t *result) {
    if (!result) {
        return;
    }
    
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        
        analysis->packet_count = 0;
        analysis->outlier_count = 0;
        analysis->used_bins = 0;
        
        /* Clear histogram bins if allocated */
        if (analysis->histogram) {
            memset(analysis->histogram, 0, result->config.max_bins * sizeof(uint32_t));
        }
    }
}

/* Process all buffer entries and compute deltas for analysis */
void process_buffer_for_analysis(stats_collector_t *stats, stats_analysis_result_t *result) {
    if (!stats || !result || !stats->buffer) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Invalid parameters for buffer analysis");
        return;
    }
    
    uint32_t entry_count = get_buffer_count(stats);
    if (entry_count == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "No buffer entries to process for analysis");
        return;
    }
    
    uint32_t tail = __atomic_load_n(&stats->tail, __ATOMIC_ACQUIRE);
    
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Starting stats analysis of %u packets and %d delta types", 
       entry_count, result->delta_count);
    printf("\n");
    
    if (allocate_analysis_histograms(result) != 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to allocate histograms for analysis\n");
        return;
    }
    
    /* Process each buffer entry */
    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t index = (tail + i) & stats->size_mask;
        stats_entry_t *entry = &stats->buffer[index];
        
        /* Process each active delta for this entry */
        for (int j = 0; j < result->delta_count; j++) {
            uint8_t delta_idx = result->active_deltas[j];
            process_entry_for_delta(entry, &result->deltas[delta_idx], delta_idx, &result->config);
        }
    }
    
    /* Display processing summary */
    uint32_t total_processed = 0;
    uint32_t total_outliers = 0;
    
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        delta_analysis_t *analysis = &result->deltas[delta_idx];
        total_processed += analysis->packet_count;
        total_outliers += analysis->outlier_count;
    }
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Analysis processing complete: %u total samples, %u outliers", 
           total_processed, total_outliers);
}

/* Process a single entry for a specific delta calculation */
void process_entry_for_delta(stats_entry_t *entry, delta_analysis_t *analysis, 
                           uint8_t delta_idx, stats_config_t *config) {
    if (!entry || !analysis || !config || delta_idx >= 12) {
        return;
    }
    
    const delta_definition_t *def = &ALL_DELTAS[delta_idx];
    
    /* Extract timestamps using delta definition indices */
    uint64_t ts_a = entry->timestamp_ns[def->timestamp_a_index];
    uint64_t ts_b = entry->timestamp_ns[def->timestamp_b_index];
    
    /* Validate timestamps (both must be non-zero) */
    if (ts_a == 0 || ts_b == 0) {
        return;
    }
    
    /* Calculate delta in nanoseconds */
    int64_t delta_ns = (int64_t)ts_b - (int64_t)ts_a;
    
    if (delta_ns < 0) {
        return;
    }
    
    /* Convert to microseconds with fractional precision */
    double delta_us = (double)delta_ns / 1000.0;
    
    /* Skip only truly invalid deltas (sub-nanosecond precision) */
    if (delta_us < 0.001) {
        return;
    }
    
    /* Prevent overflow for very large deltas */
    if (delta_us > 1000000.0) {
        analysis->outlier_count++;
        return;
    }
    
    /* Add delta value to percentiles collection */
    exact_percentiles_add_value(analysis, delta_us);
    
    /* Update histogram for display */
    uint32_t bin = (uint32_t)(delta_us / config->bin_width_us);
    if (bin < config->max_bins) {
        if (analysis->histogram) {
            analysis->histogram[bin]++;
            
            /* Update used_bins to track the highest bin with data */
            if (bin >= analysis->used_bins) {
                analysis->used_bins = bin + 1;
            }
        }
    } else {
        /* Delta is beyond histogram range - count as outlier */
        analysis->outlier_count++;
    }
}

/* Comparison function for qsort - percentile calculation */
static int compare_double_deltas(const void *a, const void *b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Initialize percentile calculation for a delta analysis */
int exact_percentiles_init(delta_analysis_t *analysis, uint32_t capacity) {
    if (!analysis || capacity == 0) return -1;
    
    analysis->delta_values = malloc(capacity * sizeof(double));
    if (!analysis->delta_values) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_STATS, "Failed to allocate %lu MB for delta analysis", 
                (capacity * sizeof(double)) / (1024 * 1024));
        analysis->delta_capacity = 0;
        return -1;
    }
    
    analysis->delta_capacity = capacity;
    analysis->packet_count = 0;
    analysis->percentiles_calculated = 0;
    memset(analysis->exact_percentiles, 0, sizeof(analysis->exact_percentiles));
    
    HW_LOG_DEBUG(HW_LOG_COMPONENT_STATS, "Allocated percentile array: %u entries (%lu MB)", 
           capacity, (capacity * sizeof(double)) / (1024 * 1024));
    return 0;
}

/* Add delta value to collection array */
void exact_percentiles_add_value(delta_analysis_t *analysis, double delta_us) {
    if (!analysis || !analysis->delta_values) return;
    
    if (analysis->packet_count < analysis->delta_capacity) {
        analysis->delta_values[analysis->packet_count] = delta_us;
        analysis->packet_count++;
        analysis->percentiles_calculated = 0;
    } else {
        analysis->outlier_count++;
    }
}

/* Calculate percentiles from collected delta values */
void exact_percentiles_calculate(delta_analysis_t *analysis) {
    if (!analysis || !analysis->delta_values || analysis->packet_count == 0) return;
    if (analysis->percentiles_calculated) return;
    
    /* Sort delta values for percentile calculation */
    qsort(analysis->delta_values, analysis->packet_count, sizeof(double), compare_double_deltas);
    
    /* Calculate percentiles using linear interpolation */
    const double percentile_positions[] = {25.0, 50.0, 75.0, 90.0, 95.0};
    
    for (int i = 0; i < 5; i++) {
        double pos = (percentile_positions[i] / 100.0) * (analysis->packet_count - 1);
        uint32_t lower_idx = (uint32_t)floor(pos);
        uint32_t upper_idx = (uint32_t)ceil(pos);
        
        if (lower_idx == upper_idx || upper_idx >= analysis->packet_count) {
            analysis->exact_percentiles[i] = analysis->delta_values[lower_idx];
        } else {
            double fraction = pos - lower_idx;
            double lower_val = analysis->delta_values[lower_idx];
            double upper_val = analysis->delta_values[upper_idx];
            analysis->exact_percentiles[i] = lower_val + fraction * (upper_val - lower_val);
        }
    }
    
    analysis->percentiles_calculated = 1;
}

/* Get percentile value */
double exact_percentiles_get(delta_analysis_t *analysis, uint8_t percentile) {
    if (!analysis) return 0.0;
    
    if (!analysis->percentiles_calculated) {
        exact_percentiles_calculate(analysis);
    }
    
    switch (percentile) {
        case 25: return analysis->exact_percentiles[0];
        case 50: return analysis->exact_percentiles[1]; 
        case 75: return analysis->exact_percentiles[2];
        case 90: return analysis->exact_percentiles[3];
        case 95: return analysis->exact_percentiles[4];
        default: return 0.0;
    }
}

/* Cleanup percentile resources */
void exact_percentiles_cleanup(delta_analysis_t *analysis) {
    if (!analysis) return;
    
    if (analysis->delta_values) {
        free(analysis->delta_values);
        analysis->delta_values = NULL;
    }
    analysis->delta_capacity = 0;
    analysis->packet_count = 0;
    analysis->percentiles_calculated = 0;
}

/* Display complete analysis results */
void display_analysis_results(stats_analysis_result_t *result, uint64_t packets_sent, uint64_t packets_received) {
    if (!result) {
        HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "No analysis results to display");
        return;
    }
    
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "STATS ANALYSIS RESULTS");
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "======================");
    
    stats_mode_type_t mode = result->mode;
    switch (mode) {
        case STATS_CLIENT_ONEWAY:
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total packets sent: %llu", (unsigned long long)packets_sent);
            printf("\n");
            break;
        case STATS_CLIENT_ROUNDTRIP:
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total packets sent: %llu", (unsigned long long)packets_sent);
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total return packets received: %llu", (unsigned long long)packets_received);
            printf("\n");
            break;
        case STATS_SERVER_ONEWAY:
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total packets received: %llu", (unsigned long long)packets_received);
            printf("\n");
            break;
        case STATS_SERVER_ROUNDTRIP:
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total packets received: %llu", (unsigned long long)packets_received);
            HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Total return packets sent: %llu", (unsigned long long)packets_sent);
            printf("\n");
            break;
        printf("\n");
    }
    
    /* Display each delta block in order */
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        display_delta_block(&result->deltas[delta_idx], delta_idx, &result->config);
        printf("\n");
        printf("\n");
    }
    
    /* Display deltas key section at the end */
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Key: Deltas and trip times:");
    for (int i = 0; i < result->delta_count; i++) {
        uint8_t delta_idx = result->active_deltas[i];
        const delta_definition_t *def = &ALL_DELTAS[delta_idx];
        HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "%s: %s", def->abbreviation, def->description);
    }
}

/* Display individual delta analysis block */
void display_delta_block(delta_analysis_t *analysis, uint8_t delta_idx, stats_config_t *config) {
    if (!analysis || !config || delta_idx >= 12) {
        return;
    }
    
    const delta_definition_t *def = &ALL_DELTAS[delta_idx];
    
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "%s:", def->abbreviation);
    
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Packets Evaluated: %u", analysis->packet_count);
    
    if (analysis->packet_count == 0) {
        return; /* No percentiles or histograms for zero count */
    }
    
    /* Percentiles from calculation */
    HW_LOG_INFO(HW_LOG_COMPONENT_STATS, "Percentiles (us): P25=%.3f,P50=%.3f,P75=%.3f,P90=%.3f,P95=%.3f",
           exact_percentiles_get(analysis, 25),
           exact_percentiles_get(analysis, 50),
           exact_percentiles_get(analysis, 75),
           exact_percentiles_get(analysis, 90),
           exact_percentiles_get(analysis, 95));
    
    /* Histograms (omit empty bins) */
    printf("[INFO] Histograms: (bin width=%uus) ", config->bin_width_us);
    
    bool first = true;
    for (uint32_t bin = 0; bin < analysis->used_bins; bin++) {
        if (analysis->histogram && analysis->histogram[bin] > 0) {
            if (!first) printf(",");
            printf("%u:%u", bin + 1, analysis->histogram[bin]);
            first = false;
        }
    }
    
    /* Outliers (if any) */
    if (analysis->outlier_count > 0) {
        if (!first) printf(",");
        printf("outliers:%u", analysis->outlier_count);
    }
}
