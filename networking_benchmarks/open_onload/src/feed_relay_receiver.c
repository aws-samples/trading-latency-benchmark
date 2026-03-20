/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: feed_relay_receiver.c
 * 
 * Summary: UDP receiver for feed relay latency measurements using ENA HW RX timestamps.
 * 
 * Description: Listens for UDP packets from a Feed Relay Server, extracts hardware RX
 * timestamps via the ENA PTP Hardware Clock, parses embedded TX application timestamps
 * from the packet payload, and computes one-way latency (HW_RX - TX_App). Reports
 * configurable percentile statistics, logs per-packet CSV data via a background writer
 * thread, and displays real-time PPS rates. Designed to run under OpenOnload kernel
 * bypass using standard POSIX/Linux socket APIs.
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
#include <signal.h>
#include <endian.h>

/* Timing constants for optimal time checking across all PPS rates */
#define MAX_TIME_CHECK_INTERVAL_NS 100000000LL
#define MAX_ITERATION_CHECK_INTERVAL 50

/* Minimum feed relay packet size: 4-byte seq_num + 8-byte tx_app_timestamp */
#define FEED_RELAY_MIN_PACKET_SIZE 12

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

/* Network configuration */
static char rx_interface[IFNAMSIZ];
static int listen_port = 12345;
static int rx_cpu = 5;

/* Runtime control */
static int time_seconds = 0;
static volatile int graceful_shutdown = 0;

/*
 * SIGNAL & PROCESS MANAGEMENT
 */

/* Signal handler for graceful shutdown */
void signal_handler(int sig __attribute__((unused))) {
    graceful_shutdown = 1;
    HW_SIGNAL_LOG("Graceful shutdown initiated");
}

/*
 * LOGGING CONFIGURATION
 */

/* Parse log level from string */
static hw_log_level_t parse_log_level(const char *level_str) {
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
static void parse_log_components(const char *comp_str) {
    if (!comp_str) return;

    hw_log_disable_component(HW_LOG_COMPONENT_MAIN);
    hw_log_disable_component(HW_LOG_COMPONENT_CLIENT);
    hw_log_disable_component(HW_LOG_COMPONENT_SERVER);
    hw_log_disable_component(HW_LOG_COMPONENT_STATS);
    hw_log_disable_component(HW_LOG_COMPONENT_CSV);
    hw_log_disable_component(HW_LOG_COMPONENT_NETWORK);
    hw_log_disable_component(HW_LOG_COMPONENT_SIGNAL);

    char *comp_copy = strdup(comp_str);
    if (!comp_copy) return;

    char *token = strtok(comp_copy, ",");
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (strcmp(token, "MAIN") == 0) hw_log_enable_component(HW_LOG_COMPONENT_MAIN);
        else if (strcmp(token, "CLIENT") == 0) hw_log_enable_component(HW_LOG_COMPONENT_CLIENT);
        else if (strcmp(token, "SERVER") == 0) hw_log_enable_component(HW_LOG_COMPONENT_SERVER);
        else if (strcmp(token, "STATS") == 0) hw_log_enable_component(HW_LOG_COMPONENT_STATS);
        else if (strcmp(token, "CSV") == 0) hw_log_enable_component(HW_LOG_COMPONENT_CSV);
        else if (strcmp(token, "NETWORK") == 0) hw_log_enable_component(HW_LOG_COMPONENT_NETWORK);
        else if (strcmp(token, "SIGNAL") == 0) hw_log_enable_component(HW_LOG_COMPONENT_SIGNAL);
        else {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Unknown log component '%s'", token);
            free(comp_copy);
            exit(EXIT_FAILURE);
        }
        token = strtok(NULL, ",");
    }
    free(comp_copy);
}

/*
 * CONFIGURATION & ARGUMENT PROCESSING
 */

/* Stats argument parsing function */
static void parse_stats_argument(const char *arg, stats_config_t *config) {
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

    /* Parse optional bin width and bin count */
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

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s --rx-interface <interface> [OPTIONS]\n\n", prog_name);

    fprintf(stderr, "Required arguments:\n");
    fprintf(stderr, "  --rx-interface <interface>   Network interface name for receiving packets\n\n");

    fprintf(stderr, "Optional arguments:\n");
    fprintf(stderr, "  --port <port>                UDP port to listen on (default: 12345)\n");
    fprintf(stderr, "  --rx-cpu <cpu>               CPU core number for receive operations (default: 5)\n");
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

void parse_arguments(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"rx-interface", required_argument, 0, 'i'},
        {"port",         required_argument, 0, 'p'},
        {"rx-cpu",       required_argument, 0, 'x'},
        {"time",         required_argument, 0, 'T'},
        {"stats",        optional_argument, 0, 'S'},
        {"output-csv",   optional_argument, 0, 'C'},
        {"log-cpu",      required_argument, 0, 'L'},
        {"log-level",    required_argument, 0, 'l'},
        {"log-component", required_argument, 0, 'c'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    int rx_interface_set = 0;

    while ((c = getopt_long(argc, argv, "i:p:x:T:S::C::L:l:c:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                strncpy(rx_interface, optarg, IFNAMSIZ - 1);
                rx_interface[IFNAMSIZ - 1] = '\0';
                rx_interface_set = 1;
                break;
            case 'p':
                listen_port = atoi(optarg);
                break;
            case 'x':
                rx_cpu = atoi(optarg);
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
            case 'C':
                csv_config.csv_enabled = 1;
                if (optarg) {
                    strncpy(csv_config.csv_filename, optarg, sizeof(csv_config.csv_filename) - 1);
                    csv_config.csv_filename[sizeof(csv_config.csv_filename) - 1] = '\0';
                } else {
                    snprintf(csv_config.csv_filename, sizeof(csv_config.csv_filename),
                             "feed_relay_timestamps_%d.csv", getpid());
                }
                break;
            case 'L':
                csv_config.log_cpu = atoi(optarg);
                if (csv_config.log_cpu < 0) {
                    fprintf(stderr, "Error: --log-cpu must be >= 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'l':
                hw_log_set_level(parse_log_level(optarg));
                break;
            case 'c':
                parse_log_components(optarg);
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

    /* --rx-interface is required */
    if (!rx_interface_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--rx-interface is required");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (time_seconds < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "--time value must be positive");
        exit(EXIT_FAILURE);
    }
}

/*
 * MAIN ENTRY POINT
 */

int main(int argc, char *argv[]) {
    /* Parse command-line arguments */
    parse_arguments(argc, argv);

    /* Compute program duration in nanoseconds */
    long long program_duration_ns = 0;
    if (time_seconds > 0) {
        program_duration_ns = (long long)time_seconds * 1000000000LL;
    }

    /* Print configuration summary */
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Feed Relay Receiver configuration:");
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "RX Interface: %s", rx_interface);
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Port: %d", listen_port);
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "RX CPU: %d", rx_cpu);
    if (time_seconds > 0) {
        HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Run time: %d seconds", time_seconds);
    } else {
        HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Run time: unlimited (until SIGINT/SIGTERM)");
    }
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "CSV output: %s (log CPU: %d)", csv_config.csv_filename, csv_config.log_cpu);
    }
    if (stats_config.enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Stats: buffer=%u, bin_width=%uus, max_bins=%u",
                     stats_config.buffer_size, stats_config.bin_width_us, stats_config.max_bins);
    }

    /* Create UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Socket creation failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Apply performance optimizations to RX socket */
    if (optimize_socket_performance(sockfd, rx_cpu, 0) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to optimize socket performance");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Configure hardware timestamping on the RX interface */
    struct ifreq hwtstamp_ifreq;
    struct hwtstamp_config hwconfig;
    memset(&hwtstamp_ifreq, 0, sizeof(hwtstamp_ifreq));
    memset(&hwconfig, 0, sizeof(hwconfig));
    strncpy(hwtstamp_ifreq.ifr_name, rx_interface, IFNAMSIZ - 1);
    hwconfig.tx_type = HWTSTAMP_TX_OFF;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
    hwtstamp_ifreq.ifr_data = (void *)&hwconfig;

    if (ioctl(sockfd, SIOCSHWTSTAMP, &hwtstamp_ifreq) < 0) {
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Hardware timestamping not supported on %s: %s",
                     rx_interface, strerror(errno));
        HW_LOG_WARN(HW_LOG_COMPONENT_MAIN, "Continuing with software timestamping only");
    } else {
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "RX hardware timestamping enabled on %s", rx_interface);
    }

    /* Set up RX socket timestamping */
    if (setup_timestamping(sockfd) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to setup RX socket timestamping");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Bind RX socket to specific interface */
    struct ifreq if_opts;
    memset(&if_opts, 0, sizeof(if_opts));
    strncpy(if_opts.ifr_name, rx_interface, IFNAMSIZ - 1);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &if_opts, sizeof(if_opts)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "SO_BINDTODEVICE failed: %s", strerror(errno));
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Bind to port */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(listen_port);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Socket bind failed: %s", strerror(errno));
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* Register signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Conditional CSV buffer creation */
    csv_ring_buffer_t *csv_buffer = NULL;
    if (csv_config.csv_enabled) {
        csv_buffer = csv_buffer_create(
            1048576,
            csv_config.csv_filename,
            CSV_FEED_RELAY_RX,
            10000,
            csv_config.log_cpu
        );
        if (!csv_buffer) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to create CSV buffer");
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "CSV logging initialized: %s", csv_config.csv_filename);
    }

    /* Conditional stats collector creation */
    if (stats_config.enabled) {
        global_stats_collector = create_stats_collector(stats_config.buffer_size, STATS_FEED_RELAY_ONEWAY);
        if (!global_stats_collector) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "Failed to create stats collector");
            if (csv_buffer) csv_buffer_destroy(csv_buffer);
            close(sockfd);
            return EXIT_FAILURE;
        }
        HW_LOG_DEBUG(HW_LOG_COMPONENT_MAIN, "Stats collector initialized (buffer: %u)", stats_config.buffer_size);
    }

    /* Setup signal-based PPS reporting */
    setup_stats_reporting_hotpath();

    /* Optimize process scheduling and CPU affinity */
    optimize_process_scheduling(rx_cpu);

    /* Calibrate CPU frequency for precise timing (on bound RX CPU) */
    calibrate_cpu_freq();

    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Listening for feed relay packets on port %d", listen_port);
    printf("\n");

    /* === RX Hot Loop (Task 5.2) === */

    /* Set up recvmsg structures */
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

    /* Initialize performance tracking */
    long long start_time = monotonic_time_ns();
    uint64_t total_packets_received = 0;
    uint64_t malformed_packets = 0;

    /* Hybrid time checking with minimal hot path impact */
    uint64_t loop_counter = 0;
    uint64_t last_time_check_cycles = rdtsc();
    uint64_t max_cycles_between_checks = (uint64_t)(cpu_freq_ghz * 1e9 * 0.1);

    /* One-time warning flag for missing HW timestamps */
    int hw_ts_warning_logged = 0;

    while (1) {
        /* Check for graceful shutdown signal */
        if (graceful_shutdown) {
            break;
        }
        loop_counter++;

        /* Hybrid time check - whichever condition hits first */
        uint64_t cycles_since_check = rdtsc() - last_time_check_cycles;
        int should_check_time = (loop_counter % MAX_ITERATION_CHECK_INTERVAL == 0) ||
                                (cycles_since_check >= max_cycles_between_checks);

        if (should_check_time) {
            if (program_duration_ns > 0) {
                long long current_time = monotonic_time_ns();
                if ((current_time - start_time) >= program_duration_ns) {
                    printf("\n");
                    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Finished run time");
                    printf("\n");
                    break;
                }
                last_time_check_cycles = rdtsc();
            }

            /* Display stats if ready */
            display_stats_if_ready();
        }

        /* Reset msg_controllen before each recvmsg call */
        msg.msg_controllen = sizeof(ctrl_buffer);

        ssize_t num_bytes = recvmsg(sockfd, &msg, MSG_DONTWAIT);
        if (num_bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            HW_LOG_ERROR(HW_LOG_COMPONENT_MAIN, "recvmsg failed: %s", strerror(errno));
            break;
        }

        /* Capture application RX timestamp immediately after recvmsg() */
        struct __kernel_timespec app_rx_ts = get_app_timestamp();

        /* Extract HW and kernel timestamps from control messages */
        struct __kernel_timespec hw_rx_ts = {0, 0};
        struct __kernel_timespec ker_rx_ts = {0, 0};
        extract_rx_timestamps(&msg, &hw_rx_ts, &ker_rx_ts);

        /* Validate minimum packet size for feed relay format */
        if (num_bytes < FEED_RELAY_MIN_PACKET_SIZE) {
            malformed_packets++;
            continue;
        }

        /* Extract sequence number (4 bytes, network byte order) */
        uint32_t seq_num = ntohl(*(uint32_t *)packet_buffer);

        /* Extract TX application timestamp (8 bytes, network byte order) */
        uint64_t tx_app_ns = be64toh(*(uint64_t *)(packet_buffer + 4));

        /* Convert RX timestamps to nanoseconds */
        uint64_t hw_rx_ns = hw_rx_ts.tv_sec * 1000000000ULL + hw_rx_ts.tv_nsec;
        uint64_t ker_rx_ns = ker_rx_ts.tv_sec * 1000000000ULL + ker_rx_ts.tv_nsec;
        uint64_t app_rx_ns = app_rx_ts.tv_sec * 1000000000ULL + app_rx_ts.tv_nsec;

        /* Compute one-way latency */
        double one_way_latency_us = 0.0;
        if (tx_app_ns != 0) {
            if (hw_rx_ns != 0) {
                /* Primary: HW RX timestamp based latency */
                one_way_latency_us = (double)((int64_t)(hw_rx_ns - tx_app_ns)) / 1000.0;
            } else {
                /* Fallback: Kernel RX timestamp (loopback/same-host) */
                if (!hw_ts_warning_logged) {
                    HW_LOG_WARN(HW_LOG_COMPONENT_MAIN,
                        "HW RX timestamps unavailable (loopback/same-host?), falling back to kernel timestamps");
                    hw_ts_warning_logged = 1;
                }
                one_way_latency_us = (double)((int64_t)(ker_rx_ns - tx_app_ns)) / 1000.0;
            }
        }

        /* Extract source IP and port */
        char src_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip_str, INET_ADDRSTRLEN);
        int src_port = ntohs(src_addr.sin_port);

        /* Update local counter for final execution details */
        total_packets_received++;

        /* Update atomic counter for signal-based PPS reporting */
        __atomic_fetch_add(&g_packets_received, 1, __ATOMIC_RELAXED);

        /* Conditional CSV processing */
        if (csv_config.csv_enabled && csv_buffer) {
            csv_entry_t csv_entry;
            create_csv_feed_relay_rx(&csv_entry, seq_num, src_ip_str,
                                     (uint16_t)src_port, hw_rx_ns, ker_rx_ns,
                                     app_rx_ns, tx_app_ns, one_way_latency_us);
            csv_enqueue_fast(csv_buffer, &csv_entry);
        }

        /* Conditional stats processing */
        if (stats_config.enabled && global_stats_collector) {
            /* Build complete stats entry with all timestamps before enqueuing */
            stats_entry_t stats_entry;
            memset(&stats_entry, 0, sizeof(stats_entry));
            stats_entry.seq_num = seq_num;
            stats_entry.src_port = (uint16_t)src_port;
            stats_entry.entry_type = (uint8_t)TIMESTAMP_MODE_FEED_RELAY_ONEWAY;
            stats_entry.timestamp_mask = get_timestamp_mask_for_mode(TIMESTAMP_MODE_FEED_RELAY_ONEWAY);

            /* Set all timestamps in one shot.
             * Index 7 is used for the delta computation (ts[7] - ts[10]).
             * If HW RX timestamp is unavailable (zero), use kernel RX as fallback
             * so the one-way latency delta still computes. */
            stats_entry.timestamp_ns[7] = (hw_rx_ns != 0) ? hw_rx_ns : ker_rx_ns;
            stats_entry.timestamp_ns[8] = ker_rx_ns;    /* Kernel RX */
            stats_entry.timestamp_ns[9] = app_rx_ns;    /* App RX */
            stats_entry.timestamp_ns[10] = tx_app_ns;   /* TX App (from payload) */

            stats_enqueue(global_stats_collector, &stats_entry);
        }
    }

    /* === Shutdown and Cleanup (Task 5.3) === */

    /* Cleanup signal-based PPS reporting (stop SIGALRM first) */
    cleanup_stats_reporting_hotpath();

    /* Process stats buffer and display analysis if enabled */
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

    /* Display final execution details */
    printf("\n");
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "EXECUTION DETAILS");
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "=================");
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Total packets received: %llu", (unsigned long long)total_packets_received);
    HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Malformed packets: %llu", (unsigned long long)malformed_packets);
    if (csv_config.csv_enabled) {
        HW_LOG_INFO(HW_LOG_COMPONENT_MAIN, "Timestamps CSV filename: %s", csv_config.csv_filename);
    }

    /* Close RX socket */
    close(sockfd);

    /* Flush and destroy CSV buffer */
    if (csv_buffer) {
        csv_buffer_destroy(csv_buffer);
    }

    return 0;
}
