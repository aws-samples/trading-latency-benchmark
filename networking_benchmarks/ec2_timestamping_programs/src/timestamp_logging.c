/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_logging.c
 * 
 * Summary: Implementation of thread-safe logging system for high-performance timestamp measurement.
 * 
 * Description: Core logging system providing thread-safe, component-based logging with atomic
 * performance counters, signal-safe logging capabilities and runtime configuration. Features
 * multi-level logging (TRACE/DEBUG/INFO/WARN/ERROR/FATAL), component filtering, environment
 * variable configuration, and signal-safe logging using write() syscalls. Includes timestamp
 * formatting, output stream routing and lock-free atomic operations.
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

#include "timestamp_logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

/*
 * GLOBAL STATE & CONFIGURATION MANAGEMENT
 */

/* Log configuration state */
static hw_log_level_t g_log_level = HW_LOG_LEVEL_INFO;
static uint64_t g_component_mask = 0xFFFFFFFFFFFFFFFFULL; /* All components enabled by default */

/* Lock-free performance counters */
_Atomic uint64_t hw_hotpath_error_count = 0;
_Atomic uint64_t hw_hotpath_warn_count = 0;

/* Display data - component names */
static const char* component_names[] = {
    "MAIN",
    "CLIENT", 
    "SERVER",
    "STATS",
    "CSV",
    "NETWORK",
    "SIGNAL"
};

/* Display data - level names */
static const char* level_names[] = {
    "TRACE",
    "DEBUG", 
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

/*
 * SYSTEM INITIALIZATION & CONFIGURATION
 */

/* Initialize logging system */
void hw_log_init(void) {
    atomic_store(&hw_hotpath_error_count, 0);
    atomic_store(&hw_hotpath_warn_count, 0);
    
    const char* env_level = getenv("HW_LOG_LEVEL");
    if (env_level) {
        if (strcmp(env_level, "TRACE") == 0) g_log_level = HW_LOG_LEVEL_TRACE;
        else if (strcmp(env_level, "DEBUG") == 0) g_log_level = HW_LOG_LEVEL_DEBUG;
        else if (strcmp(env_level, "INFO") == 0) g_log_level = HW_LOG_LEVEL_INFO;
        else if (strcmp(env_level, "WARN") == 0) g_log_level = HW_LOG_LEVEL_WARN;
        else if (strcmp(env_level, "ERROR") == 0) g_log_level = HW_LOG_LEVEL_ERROR;
        else if (strcmp(env_level, "FATAL") == 0) g_log_level = HW_LOG_LEVEL_FATAL;
        else if (strcmp(env_level, "OFF") == 0) g_log_level = HW_LOG_LEVEL_OFF;
    }
}

/* Cleanup logging system */
void hw_log_cleanup(void) {
    /* If needed in future */
}

/* Runtime log level control */
void hw_log_set_level(hw_log_level_t level) {
    if (level <= HW_LOG_LEVEL_OFF) {
        g_log_level = level;
    }
}

hw_log_level_t hw_log_get_level(void) {
    return g_log_level;
}

/* Component filtering - enable specific component */
void hw_log_enable_component(hw_log_component_t component) {
    if (component < 64) {
        g_component_mask |= (1ULL << component);
    }
}

/* Component filtering - disable specific component */
void hw_log_disable_component(hw_log_component_t component) {
    if (component < 64) {
        g_component_mask &= ~(1ULL << component);
    }
}

/*
 * UTILITY FUNCTIONS & HELPERS
 */

/* Check if component is enabled */
static inline int is_component_enabled(hw_log_component_t component) {
    return (component < 64) && (g_component_mask & (1ULL << component));
}

/* Get current timestamp for logging (static utility function) */
static void get_timestamp_string(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/*
 * CORE LOGGING ENGINE
 */

/* Core logging function */
void hw_log_internal(hw_log_level_t level, hw_log_component_t component, 
                     const char* function, int line, const char* fmt, ...) {
    
    if (level < g_log_level || level > HW_LOG_LEVEL_FATAL) {
        return;
    }
    
    if (!is_component_enabled(component)) {
        return;
    }
    
    char timestamp[32];
    get_timestamp_string(timestamp, sizeof(timestamp));
    
    const char* level_name = (level < 6) ? level_names[level] : "UNKNOWN";
    const char* comp_name = (component < 7) ? component_names[component] : "UNKNOWN";
    
    FILE* output = (level >= HW_LOG_LEVEL_WARN) ? stderr : stdout;
    
    if (level == HW_LOG_LEVEL_INFO) {
        /* Only show level for INFO output */
        fprintf(output, "[%s] ", level_name);
    } else {
        if (level >= HW_LOG_LEVEL_ERROR && function && line > 0) {
            fprintf(output, "[%s] [%s] [%s] [%s:%d] ", timestamp, level_name, comp_name, function, line);
        } else {
            fprintf(output, "[%s] [%s] [%s] ", timestamp, level_name, comp_name);
        }
    }
    
    va_list args;
    va_start(args, fmt);
    vfprintf(output, fmt, args);
    va_end(args);
    
    fprintf(output, "\n");
    fflush(output);
}

/*
 * SPECIALIZED LOGGING FUNCTIONS
 */

/* Signal-safe logging using write() syscall */
void hw_signal_safe_log(const char* message) {
    if (!message) return;
    
    char simple_timestamp[16];
    time_t now = time(NULL);
    struct tm tm_buf;
    if (gmtime_r(&now, &tm_buf)) {
        snprintf(simple_timestamp, sizeof(simple_timestamp), "[%02d:%02d:%02d] ", 
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        strcpy(simple_timestamp, "[??:??:??] ");
    }

    /* Write timestamp */
    size_t ts_len = strlen(simple_timestamp);
    if (write(STDERR_FILENO, simple_timestamp, ts_len) != (ssize_t)ts_len) {
        /* Ignore write errors in signal handler */
    }
    
    /* Write message */
    size_t msg_len = strlen(message);
    if (write(STDERR_FILENO, message, msg_len) != (ssize_t)msg_len) {
        /* Ignore write errors in signal handler */
    }
    
    /*  Write newline */
    if (write(STDERR_FILENO, "\n", 1) != 1) {
        /* Ignore write errors in signal handler */
    }
}

/*
 * STATISTICS & PERFORMANCE MONITORING  
 */

/* Get hot path error and warning statistics - atomic read operation */
uint64_t hw_get_hotpath_error_count(void) {
    return atomic_load(&hw_hotpath_error_count);
}
uint64_t hw_get_hotpath_warn_count(void) {
    return atomic_load(&hw_hotpath_warn_count);
}

/* Reset hot path counters - atomic reset operation */
void hw_reset_hotpath_counters(void) {
    atomic_store(&hw_hotpath_error_count, 0);
    atomic_store(&hw_hotpath_warn_count, 0);
}