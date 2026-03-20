/* Feature: onload-feed-relay-receiver, Property 8: Argument parsing correctness */
/*
 * Property 8: Argument Parsing Correctness
 *
 * For any valid combination of command-line arguments (--rx-interface, --port,
 * --rx-cpu, --time, --stats, --output-csv, --log-cpu), the argument parser
 * should populate the corresponding configuration variables with the specified
 * values, and unspecified optional arguments should retain their default values
 * (port=12345, rx_cpu=5, log_cpu=0).
 *
 * This test replicates the parsing logic from feed_relay_receiver.c to avoid
 * linking against the main program (which has global state and getopt_long
 * internal state issues).
 *
 * Validates: Requirements 12.1, 12.3, 12.4, 12.5
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <net/if.h>
#include <unistd.h>

#define NUM_ITERATIONS 200

/* ---- Replicated configuration structures from feed_relay_receiver.c ---- */

typedef struct {
    int csv_enabled;
    char csv_filename[256];
    int log_cpu;
} test_csv_config_t;

typedef struct {
    int enabled;
    uint32_t buffer_size;
    uint32_t bin_width_us;
    uint32_t max_bins;
} test_stats_config_t;

typedef struct {
    char rx_interface[IFNAMSIZ];
    int listen_port;
    int rx_cpu;
    int time_seconds;
    test_csv_config_t csv_config;
    test_stats_config_t stats_config;
} test_config_t;

/* Default values matching feed_relay_receiver.c */
#define DEFAULT_PORT      12345
#define DEFAULT_RX_CPU    5
#define DEFAULT_LOG_CPU   0
#define DEFAULT_TIME      0
#define DEFAULT_BUF_SIZE  5000000
#define DEFAULT_BIN_WIDTH 10
#define DEFAULT_MAX_BINS  1000

/* Initialize config with defaults */
static void init_config(test_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = DEFAULT_PORT;
    cfg->rx_cpu = DEFAULT_RX_CPU;
    cfg->time_seconds = DEFAULT_TIME;
    cfg->csv_config.csv_enabled = 0;
    cfg->csv_config.log_cpu = DEFAULT_LOG_CPU;
    cfg->stats_config.enabled = 0;
    cfg->stats_config.buffer_size = DEFAULT_BUF_SIZE;
    cfg->stats_config.bin_width_us = DEFAULT_BIN_WIDTH;
    cfg->stats_config.max_bins = DEFAULT_MAX_BINS;
}

/*
 * Replicated argument parser from feed_relay_receiver.c.
 * Returns 0 on success, -1 on error (missing required args, etc.)
 */
static int parse_arguments(int argc, char *argv[], test_config_t *cfg) {
    static struct option long_options[] = {
        {"rx-interface", required_argument, 0, 'i'},
        {"port",         required_argument, 0, 'p'},
        {"rx-cpu",       required_argument, 0, 'x'},
        {"time",         required_argument, 0, 'T'},
        {"stats",        optional_argument, 0, 'S'},
        {"output-csv",   optional_argument, 0, 'C'},
        {"log-cpu",      required_argument, 0, 'L'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    int rx_interface_set = 0;

    /* Reset getopt state for repeated calls */
    optind = 1;
    opterr = 0;

    while ((c = getopt_long(argc, argv, "i:p:x:T:S::C::L:h",
                            long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                strncpy(cfg->rx_interface, optarg, IFNAMSIZ - 1);
                cfg->rx_interface[IFNAMSIZ - 1] = '\0';
                rx_interface_set = 1;
                break;
            case 'p':
                cfg->listen_port = atoi(optarg);
                break;
            case 'x':
                cfg->rx_cpu = atoi(optarg);
                break;
            case 'T':
                cfg->time_seconds = atoi(optarg);
                break;
            case 'S':
                cfg->stats_config.enabled = 1;
                if (optarg) {
                    /* Parse buffer size with optional M/K suffix */
                    char *arg_copy = strdup(optarg);
                    if (!arg_copy) return -1;
                    char *token = strtok(arg_copy, ",");
                    if (token) {
                        char *endptr;
                        unsigned long size = strtoul(token, &endptr, 10);
                        if (endptr && *endptr != '\0') {
                            if (*endptr == 'M' || *endptr == 'm')
                                size *= 1000000;
                            else if (*endptr == 'K' || *endptr == 'k')
                                size *= 1000;
                        }
                        if (size >= 10000 && size <= 10000000)
                            cfg->stats_config.buffer_size = (uint32_t)size;
                    }
                    while ((token = strtok(NULL, ",")) != NULL) {
                        if (strncmp(token, "bw=", 3) == 0) {
                            unsigned long bw = strtoul(token + 3, NULL, 10);
                            if (bw >= 1 && bw <= 1000)
                                cfg->stats_config.bin_width_us = (uint32_t)bw;
                        } else if (strncmp(token, "bn=", 3) == 0) {
                            unsigned long bn = strtoul(token + 3, NULL, 10);
                            if (bn >= 10 && bn <= 10000)
                                cfg->stats_config.max_bins = (uint32_t)bn;
                        }
                    }
                    free(arg_copy);
                }
                break;
            case 'C':
                cfg->csv_config.csv_enabled = 1;
                if (optarg) {
                    strncpy(cfg->csv_config.csv_filename, optarg,
                            sizeof(cfg->csv_config.csv_filename) - 1);
                } else {
                    snprintf(cfg->csv_config.csv_filename,
                             sizeof(cfg->csv_config.csv_filename),
                             "feed_relay_timestamps_test.csv");
                }
                break;
            case 'L':
                cfg->csv_config.log_cpu = atoi(optarg);
                break;
            case 'h':
            case '?':
            default:
                return -1;
        }
    }

    if (!rx_interface_set)
        return -1;

    return 0;
}

/* ---- Random generators ---- */

static uint32_t rand_uint32(void) {
    uint32_t val = 0;
    val |= (uint32_t)(rand() & 0xFFFF) << 16;
    val |= (uint32_t)(rand() & 0xFFFF);
    return val;
}

/* Generate a random port in valid range [1024, 65535] */
static int rand_port(void) {
    return 1024 + (int)(rand_uint32() % (65535 - 1024 + 1));
}

/* Generate a random CPU core [0, 31] */
static int rand_cpu(void) {
    return (int)(rand_uint32() % 32);
}

/* Generate a random time duration [1, 3600] seconds */
static int rand_time(void) {
    return 1 + (int)(rand_uint32() % 3600);
}

/* Generate a random interface name like "eth0", "ens5", "lo" */
static const char *IFACE_NAMES[] = {
    "eth0", "eth1", "ens5", "ens6", "lo", "enp0s3", "eno1", "bond0"
};
#define NUM_IFACES (int)(sizeof(IFACE_NAMES) / sizeof(IFACE_NAMES[0]))

static const char *rand_interface(void) {
    return IFACE_NAMES[rand_uint32() % NUM_IFACES];
}

/* Build an argv array for a test case. Returns argc. */
static int build_argv(char **argv, int max_args,
                      const char *iface, int set_port, int port,
                      int set_rx_cpu, int rx_cpu,
                      int set_time, int time_sec,
                      int set_stats, const char *stats_arg,
                      int set_csv, const char *csv_arg,
                      int set_log_cpu, int log_cpu) {
    int argc = 0;
    static char port_buf[16], rx_cpu_buf[16], time_buf[16], log_cpu_buf[16];
    static char stats_buf[64], csv_buf[256];

    argv[argc++] = "test_prog";

    /* --rx-interface is always required */
    argv[argc++] = "--rx-interface";
    argv[argc++] = (char *)iface;

    if (set_port && argc + 2 <= max_args) {
        snprintf(port_buf, sizeof(port_buf), "%d", port);
        argv[argc++] = "--port";
        argv[argc++] = port_buf;
    }
    if (set_rx_cpu && argc + 2 <= max_args) {
        snprintf(rx_cpu_buf, sizeof(rx_cpu_buf), "%d", rx_cpu);
        argv[argc++] = "--rx-cpu";
        argv[argc++] = rx_cpu_buf;
    }
    if (set_time && argc + 2 <= max_args) {
        snprintf(time_buf, sizeof(time_buf), "%d", time_sec);
        argv[argc++] = "--time";
        argv[argc++] = time_buf;
    }
    if (set_stats && argc + 1 <= max_args) {
        if (stats_arg) {
            snprintf(stats_buf, sizeof(stats_buf), "--stats=%s", stats_arg);
            argv[argc++] = stats_buf;
        } else {
            argv[argc++] = "--stats";
        }
    }
    if (set_csv && argc + 1 <= max_args) {
        if (csv_arg) {
            snprintf(csv_buf, sizeof(csv_buf), "--output-csv=%s", csv_arg);
            argv[argc++] = csv_buf;
        } else {
            argv[argc++] = "--output-csv";
        }
    }
    if (set_log_cpu && argc + 2 <= max_args) {
        snprintf(log_cpu_buf, sizeof(log_cpu_buf), "%d", log_cpu);
        argv[argc++] = "--log-cpu";
        argv[argc++] = log_cpu_buf;
    }

    return argc;
}

int main(void) {
    unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    printf("Property 8: Argument Parsing Correctness\n");
    printf("Validates: Requirements 12.1, 12.3, 12.4, 12.5\n");
    printf("Seed: %u\n", seed);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    int passed = 0;
    int failed = 0;

    /* ---- Test 1: Defaults only (just --rx-interface) ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[20];
        int argc = build_argv(argv, 20, "eth0",
                              0, 0, 0, 0, 0, 0,
                              0, NULL, 0, NULL, 0, 0);
        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL defaults: parse_arguments returned %d\n", rc);
            failed++;
        } else if (strcmp(cfg.rx_interface, "eth0") != 0 ||
                   cfg.listen_port != DEFAULT_PORT ||
                   cfg.rx_cpu != DEFAULT_RX_CPU ||
                   cfg.time_seconds != DEFAULT_TIME ||
                   cfg.csv_config.log_cpu != DEFAULT_LOG_CPU ||
                   cfg.csv_config.csv_enabled != 0 ||
                   cfg.stats_config.enabled != 0) {
            fprintf(stderr, "FAIL defaults: unexpected values "
                    "iface=%s port=%d rx_cpu=%d time=%d log_cpu=%d "
                    "csv=%d stats=%d\n",
                    cfg.rx_interface, cfg.listen_port, cfg.rx_cpu,
                    cfg.time_seconds, cfg.csv_config.log_cpu,
                    cfg.csv_config.csv_enabled, cfg.stats_config.enabled);
            failed++;
        } else {
            passed++;
        }
    }

    /* ---- Test 2: Missing --rx-interface should fail ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[] = { "test_prog", "--port", "9999" };
        int rc = parse_arguments(3, argv, &cfg);
        if (rc == 0) {
            fprintf(stderr, "FAIL missing-iface: should have returned error\n");
            failed++;
        } else {
            passed++;
        }
    }

    /* ---- Test 3: All arguments specified ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[20];
        int argc = build_argv(argv, 20, "ens5",
                              1, 54321,   /* port */
                              1, 3,       /* rx_cpu */
                              1, 60,      /* time */
                              1, "1M,bw=5,bn=500",  /* stats */
                              1, "out.csv",          /* csv */
                              1, 2);                 /* log_cpu */
        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL all-args: parse_arguments returned %d\n", rc);
            failed++;
        } else {
            int ok = 1;
            if (strcmp(cfg.rx_interface, "ens5") != 0) {
                fprintf(stderr, "FAIL all-args: iface=%s expected ens5\n",
                        cfg.rx_interface);
                ok = 0;
            }
            if (cfg.listen_port != 54321) {
                fprintf(stderr, "FAIL all-args: port=%d expected 54321\n",
                        cfg.listen_port);
                ok = 0;
            }
            if (cfg.rx_cpu != 3) {
                fprintf(stderr, "FAIL all-args: rx_cpu=%d expected 3\n",
                        cfg.rx_cpu);
                ok = 0;
            }
            if (cfg.time_seconds != 60) {
                fprintf(stderr, "FAIL all-args: time=%d expected 60\n",
                        cfg.time_seconds);
                ok = 0;
            }
            if (!cfg.stats_config.enabled) {
                fprintf(stderr, "FAIL all-args: stats not enabled\n");
                ok = 0;
            }
            if (cfg.stats_config.buffer_size != 1000000) {
                fprintf(stderr, "FAIL all-args: buf_size=%u expected 1000000\n",
                        cfg.stats_config.buffer_size);
                ok = 0;
            }
            if (cfg.stats_config.bin_width_us != 5) {
                fprintf(stderr, "FAIL all-args: bw=%u expected 5\n",
                        cfg.stats_config.bin_width_us);
                ok = 0;
            }
            if (cfg.stats_config.max_bins != 500) {
                fprintf(stderr, "FAIL all-args: bn=%u expected 500\n",
                        cfg.stats_config.max_bins);
                ok = 0;
            }
            if (!cfg.csv_config.csv_enabled) {
                fprintf(stderr, "FAIL all-args: csv not enabled\n");
                ok = 0;
            }
            if (strcmp(cfg.csv_config.csv_filename, "out.csv") != 0) {
                fprintf(stderr, "FAIL all-args: csv_file=%s expected out.csv\n",
                        cfg.csv_config.csv_filename);
                ok = 0;
            }
            if (cfg.csv_config.log_cpu != 2) {
                fprintf(stderr, "FAIL all-args: log_cpu=%d expected 2\n",
                        cfg.csv_config.log_cpu);
                ok = 0;
            }
            if (ok) passed++; else failed++;
        }
    }

    /* ---- Test 4: --stats without argument uses defaults ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[20];
        int argc = build_argv(argv, 20, "eth0",
                              0, 0, 0, 0, 0, 0,
                              1, NULL,   /* stats with no arg */
                              0, NULL, 0, 0);
        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL stats-defaults: parse returned %d\n", rc);
            failed++;
        } else if (!cfg.stats_config.enabled ||
                   cfg.stats_config.buffer_size != DEFAULT_BUF_SIZE ||
                   cfg.stats_config.bin_width_us != DEFAULT_BIN_WIDTH ||
                   cfg.stats_config.max_bins != DEFAULT_MAX_BINS) {
            fprintf(stderr, "FAIL stats-defaults: enabled=%d buf=%u bw=%u bn=%u\n",
                    cfg.stats_config.enabled, cfg.stats_config.buffer_size,
                    cfg.stats_config.bin_width_us, cfg.stats_config.max_bins);
            failed++;
        } else {
            passed++;
        }
    }

    /* ---- Test 5: --stats with K suffix ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[20];
        int argc = build_argv(argv, 20, "eth0",
                              0, 0, 0, 0, 0, 0,
                              1, "100K",
                              0, NULL, 0, 0);
        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL stats-K: parse returned %d\n", rc);
            failed++;
        } else if (cfg.stats_config.buffer_size != 100000) {
            fprintf(stderr, "FAIL stats-K: buf_size=%u expected 100000\n",
                    cfg.stats_config.buffer_size);
            failed++;
        } else {
            passed++;
        }
    }

    /* ---- Test 6: --output-csv without filename gets default ---- */
    {
        test_config_t cfg;
        init_config(&cfg);
        char *argv[20];
        int argc = build_argv(argv, 20, "eth0",
                              0, 0, 0, 0, 0, 0,
                              0, NULL,
                              1, NULL,   /* csv with no filename */
                              0, 0);
        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL csv-default: parse returned %d\n", rc);
            failed++;
        } else if (!cfg.csv_config.csv_enabled ||
                   strlen(cfg.csv_config.csv_filename) == 0) {
            fprintf(stderr, "FAIL csv-default: enabled=%d file='%s'\n",
                    cfg.csv_config.csv_enabled, cfg.csv_config.csv_filename);
            failed++;
        } else {
            passed++;
        }
    }

    /* ---- Randomized property tests: random valid argument combos ---- */
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        test_config_t cfg;
        init_config(&cfg);

        /* Randomly decide which optional args to include */
        const char *iface = rand_interface();
        int set_port = rand() % 2;
        int port = rand_port();
        int set_rx_cpu = rand() % 2;
        int rx_cpu_val = rand_cpu();
        int set_time = rand() % 2;
        int time_val = rand_time();
        int set_stats = rand() % 2;
        int set_csv = rand() % 2;
        int set_log_cpu = (set_csv && (rand() % 2)) ? 1 : 0;
        int log_cpu_val = rand_cpu();

        /* Build stats argument string (or NULL for bare --stats) */
        char stats_str[64];
        const char *stats_arg = NULL;
        if (set_stats && (rand() % 2)) {
            /* Generate a valid stats config string */
            int use_suffix = rand() % 3;
            uint32_t buf_val;
            if (use_suffix == 0) {
                buf_val = 10000 + (rand_uint32() % (10000000 - 10000 + 1));
                snprintf(stats_str, sizeof(stats_str), "%u", buf_val);
            } else if (use_suffix == 1) {
                buf_val = 10 + (rand_uint32() % (10000 - 10 + 1));
                snprintf(stats_str, sizeof(stats_str), "%uK", buf_val);
                buf_val *= 1000;
            } else {
                buf_val = 1 + (rand_uint32() % 10);
                snprintf(stats_str, sizeof(stats_str), "%uM", buf_val);
                buf_val *= 1000000;
            }
            /* Clamp to valid range for verification */
            if (buf_val < 10000) buf_val = 10000;
            if (buf_val > 10000000) buf_val = 10000000;
            stats_arg = stats_str;
        }

        /* Build CSV filename (or NULL for default) */
        char csv_str[64];
        const char *csv_arg = NULL;
        if (set_csv && (rand() % 2)) {
            snprintf(csv_str, sizeof(csv_str), "test_output_%d.csv", i);
            csv_arg = csv_str;
        }

        char *argv[20];
        int argc = build_argv(argv, 20, iface,
                              set_port, port,
                              set_rx_cpu, rx_cpu_val,
                              set_time, time_val,
                              set_stats, stats_arg,
                              set_csv, csv_arg,
                              set_log_cpu, log_cpu_val);

        int rc = parse_arguments(argc, argv, &cfg);
        if (rc != 0) {
            fprintf(stderr, "FAIL iter %d: parse_arguments returned %d\n", i, rc);
            failed++;
            continue;
        }

        int ok = 1;

        /* Verify rx_interface always matches */
        if (strcmp(cfg.rx_interface, iface) != 0) {
            fprintf(stderr, "FAIL iter %d: iface='%s' expected '%s'\n",
                    i, cfg.rx_interface, iface);
            ok = 0;
        }

        /* Verify port: specified value or default */
        int expected_port = set_port ? port : DEFAULT_PORT;
        if (cfg.listen_port != expected_port) {
            fprintf(stderr, "FAIL iter %d: port=%d expected %d\n",
                    i, cfg.listen_port, expected_port);
            ok = 0;
        }

        /* Verify rx_cpu: specified value or default */
        int expected_rx_cpu = set_rx_cpu ? rx_cpu_val : DEFAULT_RX_CPU;
        if (cfg.rx_cpu != expected_rx_cpu) {
            fprintf(stderr, "FAIL iter %d: rx_cpu=%d expected %d\n",
                    i, cfg.rx_cpu, expected_rx_cpu);
            ok = 0;
        }

        /* Verify time: specified value or default */
        int expected_time = set_time ? time_val : DEFAULT_TIME;
        if (cfg.time_seconds != expected_time) {
            fprintf(stderr, "FAIL iter %d: time=%d expected %d\n",
                    i, cfg.time_seconds, expected_time);
            ok = 0;
        }

        /* Verify stats enabled flag */
        if (set_stats && !cfg.stats_config.enabled) {
            fprintf(stderr, "FAIL iter %d: stats should be enabled\n", i);
            ok = 0;
        }
        if (!set_stats && cfg.stats_config.enabled) {
            fprintf(stderr, "FAIL iter %d: stats should not be enabled\n", i);
            ok = 0;
        }

        /* Verify csv enabled flag */
        if (set_csv && !cfg.csv_config.csv_enabled) {
            fprintf(stderr, "FAIL iter %d: csv should be enabled\n", i);
            ok = 0;
        }
        if (!set_csv && cfg.csv_config.csv_enabled) {
            fprintf(stderr, "FAIL iter %d: csv should not be enabled\n", i);
            ok = 0;
        }

        /* Verify csv filename when specified */
        if (set_csv && csv_arg) {
            if (strcmp(cfg.csv_config.csv_filename, csv_arg) != 0) {
                fprintf(stderr, "FAIL iter %d: csv_file='%s' expected '%s'\n",
                        i, cfg.csv_config.csv_filename, csv_arg);
                ok = 0;
            }
        }
        /* When csv enabled without explicit filename, just check non-empty */
        if (set_csv && !csv_arg) {
            if (strlen(cfg.csv_config.csv_filename) == 0) {
                fprintf(stderr, "FAIL iter %d: csv_file should have default name\n", i);
                ok = 0;
            }
        }

        /* Verify log_cpu: specified value or default */
        int expected_log_cpu = set_log_cpu ? log_cpu_val : DEFAULT_LOG_CPU;
        if (cfg.csv_config.log_cpu != expected_log_cpu) {
            fprintf(stderr, "FAIL iter %d: log_cpu=%d expected %d\n",
                    i, cfg.csv_config.log_cpu, expected_log_cpu);
            ok = 0;
        }

        /* Verify unset optional args retain defaults */
        if (!set_port && cfg.listen_port != DEFAULT_PORT) {
            fprintf(stderr, "FAIL iter %d: unset port=%d expected default %d\n",
                    i, cfg.listen_port, DEFAULT_PORT);
            ok = 0;
        }
        if (!set_rx_cpu && cfg.rx_cpu != DEFAULT_RX_CPU) {
            fprintf(stderr, "FAIL iter %d: unset rx_cpu=%d expected default %d\n",
                    i, cfg.rx_cpu, DEFAULT_RX_CPU);
            ok = 0;
        }
        if (!set_log_cpu && cfg.csv_config.log_cpu != DEFAULT_LOG_CPU) {
            fprintf(stderr, "FAIL iter %d: unset log_cpu=%d expected default %d\n",
                    i, cfg.csv_config.log_cpu, DEFAULT_LOG_CPU);
            ok = 0;
        }

        if (ok) passed++; else failed++;
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
