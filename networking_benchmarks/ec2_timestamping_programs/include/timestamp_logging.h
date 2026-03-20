/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_logging.h
 * 
 * Summary: Header definitions for thread-safe logging system with performance optimization.
 * 
 * Description: Multi-level logging (TRACE/DEBUG/INFO/WARN/ERROR/FATAL) with component-based 
 * filtering, compile-time optimization and signal handler safe logging using write() syscalls.
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

#ifndef HW_TIMESTAMP_LOGGING_H
#define HW_TIMESTAMP_LOGGING_H
#include <stdint.h>

/*
 * CORE TYPE DEFINITIONS & ENUMS
 */

/* Logging levels - TRACE (most verbose) through OFF (no logging) */
typedef enum {
    HW_LOG_LEVEL_TRACE = 0,
    HW_LOG_LEVEL_DEBUG = 1,
    HW_LOG_LEVEL_INFO = 2,
    HW_LOG_LEVEL_WARN = 3,
    HW_LOG_LEVEL_ERROR = 4,
    HW_LOG_LEVEL_FATAL = 5,
    HW_LOG_LEVEL_OFF = 6
} hw_log_level_t;

/* Component identification - system components for targeted logging control */
typedef enum {
    HW_LOG_COMPONENT_MAIN = 0,
    HW_LOG_COMPONENT_CLIENT = 1,
    HW_LOG_COMPONENT_SERVER = 2,
    HW_LOG_COMPONENT_STATS = 3,
    HW_LOG_COMPONENT_CSV = 4,
    HW_LOG_COMPONENT_NETWORK = 5,
    HW_LOG_COMPONENT_SIGNAL = 6
} hw_log_component_t;

/*
 * COMPILE-TIME CONFIGURATION & CONSTANTS
 */

/* Compile-time log level (can be overridden via -DHW_LOG_COMPILE_LEVEL=X) */
#ifndef HW_LOG_COMPILE_LEVEL
#define HW_LOG_COMPILE_LEVEL HW_LOG_LEVEL_INFO
#endif

/* Performance optimization hints */
#define HW_LOG_LIKELY(x) __builtin_expect(!!(x), 1)
#define HW_LOG_UNLIKELY(x) __builtin_expect(!!(x), 0)

/*
 * CORE FUNCTION DECLARATIONS
 */

/* Core logging function */
void hw_log_internal(hw_log_level_t level, hw_log_component_t component, 
                     const char* function, int line, const char* fmt, ...);

/* Signal-safe logging function */
void hw_signal_safe_log(const char* message);

/*
 * STANDARD LOGGING MACROS
 */

/* Logging macros with compile-time optimization */
#define HW_LOG_TRACE(comp, fmt, ...) \
    do { if (HW_LOG_UNLIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_TRACE)) \
         hw_log_internal(HW_LOG_LEVEL_TRACE, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define HW_LOG_DEBUG(comp, fmt, ...) \
    do { if (HW_LOG_UNLIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_DEBUG)) \
         hw_log_internal(HW_LOG_LEVEL_DEBUG, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define HW_LOG_INFO(comp, fmt, ...) \
    do { if (HW_LOG_LIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_INFO)) \
         hw_log_internal(HW_LOG_LEVEL_INFO, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define HW_LOG_WARN(comp, fmt, ...) \
    do { if (HW_LOG_LIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_WARN)) \
         hw_log_internal(HW_LOG_LEVEL_WARN, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define HW_LOG_ERROR(comp, fmt, ...) \
    do { if (HW_LOG_LIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_ERROR)) \
         hw_log_internal(HW_LOG_LEVEL_ERROR, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define HW_LOG_FATAL(comp, fmt, ...) \
    do { if (HW_LOG_LIKELY(HW_LOG_COMPILE_LEVEL <= HW_LOG_LEVEL_FATAL)) \
         hw_log_internal(HW_LOG_LEVEL_FATAL, comp, __func__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

/*
 * HIGH-PERFORMANCE & HOT PATH INFRASTRUCTURE
 */

/* Hot path protection - can be completely compiled out */
#ifdef HW_ENABLE_HOTPATH_LOGGING
#define HW_HOTPATH_LOG_ERROR(fmt, ...) \
    hw_signal_safe_log("HOTPATH_ERROR: " fmt)
#define HW_HOTPATH_LOG_WARN(fmt, ...) \
    hw_signal_safe_log("HOTPATH_WARN: " fmt)
#else
#define HW_HOTPATH_LOG_ERROR(fmt, ...) do { } while(0)
#define HW_HOTPATH_LOG_WARN(fmt, ...) do { } while(0)
#endif

/* Lock-free error counters for hot paths */
extern _Atomic uint64_t hw_hotpath_error_count;
extern _Atomic uint64_t hw_hotpath_warn_count;

#define HW_HOTPATH_COUNT_ERROR() \
    __atomic_fetch_add(&hw_hotpath_error_count, 1, __ATOMIC_RELAXED)
#define HW_HOTPATH_COUNT_WARN() \
    __atomic_fetch_add(&hw_hotpath_warn_count, 1, __ATOMIC_RELAXED)

/*
 * SIGNAL-SAFE LOGGING INFRASTRUCTURE
 */

/* Signal-safe macro for signal handlers */
#define HW_SIGNAL_LOG(msg) hw_signal_safe_log(msg)

/*
 * RUNTIME CONFIGURATION & LIFECYCLE MANAGEMENT
 */

/* Runtime configuration */
void hw_log_set_level(hw_log_level_t level);
hw_log_level_t hw_log_get_level(void);
void hw_log_enable_component(hw_log_component_t component);
void hw_log_disable_component(hw_log_component_t component);

/* Initialize/cleanup logging system */
void hw_log_init(void);
void hw_log_cleanup(void);

#endif /* HW_TIMESTAMP_LOGGING_H */