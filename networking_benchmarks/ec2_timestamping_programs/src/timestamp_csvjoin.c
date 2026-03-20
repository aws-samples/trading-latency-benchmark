/*
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 * SPDX-License-Identifier: MIT-0
 * 
 * File: timestamp_csvjoin.c
 *
 * Summary: Program used to join timestamp CSV log files created by client and server.
 *  
 * Description: Joins CSV files from timestamp_client.c and timestamp_server.c based on 
 * client IP and source port packet timestamp entries.
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
 * FILE HEADER, DEFINITIONS & CONSTANTS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include "timestamp_logging.h"

#define MAX_LINE_LENGTH 2048
#define MAX_FILES 10
#define MAX_TIMESTAMP_FIELDS 20
#define MAX_IP_LENGTH 16
#define MAX_FILENAME_LENGTH 256
#define MAX_CSV_FIELDS 20
#define MAX_DELTAS 15

/* Timestamp field indices for output formats */
#define FIELD_CLT_APP_TX_TSC_TS 0
#define FIELD_CLT_APP_TX_TS     1
#define FIELD_CLT_KER_TX_TS     2
#define FIELD_SVR_HW_RX_TS      3
#define FIELD_SVR_KER_RX_TS     4
#define FIELD_SVR_APP_RX_TS     5
#define FIELD_SVR_APP_TX_TS     6
#define FIELD_SVR_KER_TX_TS     7
#define FIELD_CLT_HW_RX_TS      8
#define FIELD_CLT_KER_RX_TS     9
#define FIELD_CLT_APP_RX_TSC_TS 10
#define FIELD_CLT_APP_RX_TS     11

/*
 * DATA STRUCTURES
 */

typedef enum {
    MODE_ONE_WAY,
    MODE_ROUND_TRIP
} operation_mode_t;

typedef enum {
    TARGET_CLIENT,
    TARGET_SERVER, 
    TARGET_CLIENT_SERVER
} target_type_t;

typedef enum {
    FILE_CLIENT_ONEWAY_MAIN,
    FILE_CLIENT_ONEWAY_TX,
    FILE_CLIENT_ROUNDTRIP_MAIN,
    FILE_CLIENT_ROUNDTRIP_TX,
    FILE_SERVER_ONEWAY_MAIN,
    FILE_SERVER_ROUNDTRIP_MAIN,
    FILE_SERVER_ROUNDTRIP_TX,
    FILE_TYPE_UNKNOWN
} file_type_t;

typedef struct {
    uint32_t seq_num;
    char timestamps[MAX_TIMESTAMP_FIELDS][32];
    uint16_t field_mask;
} record_t;

typedef struct {
    uint32_t *sequences;
    size_t count;
    size_t capacity;
} sequence_set_t;

typedef struct {
    char target_ip[MAX_IP_LENGTH];
    uint16_t target_port;
    operation_mode_t mode;
    target_type_t target;
    char input_files[MAX_FILES][MAX_FILENAME_LENGTH];
    int input_file_count;
    char output_file[MAX_FILENAME_LENGTH];
    file_type_t detected_types[MAX_FILES];
} config_t;

typedef struct {
    uint8_t ts_a_field;
    uint8_t ts_b_field;  
    const char* column_name;
    uint8_t mode_mask;
} delta_definition_t;

typedef struct {
    uint32_t delta_whole_us;
    uint32_t delta_frac_ns;  
    uint8_t valid;
} delta_result_t;

typedef struct {
    const uint8_t* delta_indices;
    uint8_t delta_count;
} mode_delta_config_t;

/*
 * UTILITY FUNCTIONS
 */

static char* trim_whitespace(char* str) {
    if (!str) return NULL;
    
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    end[1] = '\0';
    return str;
}

static int safe_strncpy(char* dest, const char* src, size_t n) {
    if (!dest || !src || n == 0) return -1;
    
    strncpy(dest, src, n - 1);
    dest[n - 1] = '\0';
    return 0;
}

static int compare_uint32(const void* a, const void* b) {
    uint32_t ua = *(const uint32_t*)a;
    uint32_t ub = *(const uint32_t*)b;
    
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static const char* get_file_type_name(file_type_t type) {
    switch (type) {
        case FILE_CLIENT_ONEWAY_MAIN: return "CLIENT_ONEWAY_MAIN";
        case FILE_CLIENT_ONEWAY_TX: return "CLIENT_ONEWAY_TX";
        case FILE_CLIENT_ROUNDTRIP_MAIN: return "CLIENT_ROUNDTRIP_MAIN";
        case FILE_CLIENT_ROUNDTRIP_TX: return "CLIENT_ROUNDTRIP_TX";
        case FILE_SERVER_ONEWAY_MAIN: return "SERVER_ONEWAY_MAIN";
        case FILE_SERVER_ROUNDTRIP_MAIN: return "SERVER_ROUNDTRIP_MAIN";
        case FILE_SERVER_ROUNDTRIP_TX: return "SERVER_ROUNDTRIP_TX";
        default: return "UNKNOWN";
    }
}

/*
 * DELTA CALCULATION ENGINE
 */
static const delta_definition_t ALL_DELTA_DEFINITIONS[15] = {
    {FIELD_CLT_APP_TX_TS, FIELD_CLT_KER_TX_TS, "delta_d1_clt_app_tx_to_ker_tx_us", 0x0F},
    {FIELD_CLT_HW_RX_TS, FIELD_CLT_KER_RX_TS, "delta_d6_clt_hw_rx_to_ker_rx_us", 0x02},
    {FIELD_CLT_KER_RX_TS, FIELD_CLT_APP_RX_TS, "delta_d7_clt_ker_rx_to_app_rx_us", 0x02},
    {FIELD_CLT_APP_TX_TS, FIELD_CLT_HW_RX_TS, "delta_rtt_d1_clt_app_tx_to_hw_rx_us", 0x02},
    {FIELD_CLT_APP_TX_TS, FIELD_CLT_APP_RX_TS, "delta_rtt_d2_clt_app_tx_to_app_rx_us", 0x02},
    {FIELD_CLT_APP_TX_TSC_TS, FIELD_CLT_APP_RX_TSC_TS, "delta_rtt_d3_clt_app_tx_tsc_to_app_rx_tsc_us", 0x02},
    {FIELD_SVR_HW_RX_TS, FIELD_SVR_KER_RX_TS, "delta_d2_svr_hw_rx_to_ker_rx_us", 0x0C},
    {FIELD_SVR_KER_RX_TS, FIELD_SVR_APP_RX_TS, "delta_d3_svr_ker_rx_to_app_rx_us", 0x0C},
    {FIELD_SVR_APP_RX_TS, FIELD_SVR_APP_TX_TS, "delta_d4_svr_app_rx_to_app_tx_us", 0x08},
    {FIELD_SVR_APP_TX_TS, FIELD_SVR_KER_TX_TS, "delta_d5_svr_app_tx_to_ker_tx_us", 0x08},
    {FIELD_SVR_HW_RX_TS, FIELD_SVR_APP_RX_TS, "delta_tt_d1_svr_hw_rx_to_app_rx_us", 0x0C},
    {FIELD_SVR_HW_RX_TS, FIELD_SVR_KER_TX_TS, "delta_tt_d2_svr_hw_rx_to_ker_tx_us", 0x08},
    {FIELD_CLT_KER_TX_TS, FIELD_SVR_HW_RX_TS, "delta_net_clt_ker_tx_to_svr_hw_rx_us", 0x0C},
    {FIELD_SVR_KER_TX_TS, FIELD_CLT_HW_RX_TS, "delta_net_svr_ker_tx_to_clt_hw_rx_us", 0x02}
};

static const uint8_t CLIENT_ONEWAY_DELTA_INDICES[1] = {0};
static const uint8_t CLIENT_ROUNDTRIP_DELTA_INDICES[6] = {0, 1, 2, 3, 4, 5};
static const uint8_t SERVER_ONEWAY_DELTA_INDICES[3] = {6, 7, 10};
static const uint8_t SERVER_ROUNDTRIP_DELTA_INDICES[6] = {6, 7, 8, 9, 10, 11};
static const uint8_t CLIENT_SERVER_ONEWAY_DELTA_INDICES[5] = {0, 6, 7, 10, 12};
static const uint8_t CLIENT_SERVER_ROUNDTRIP_DELTA_INDICES[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static const mode_delta_config_t MODE_DELTA_CONFIGS[6] = {
    {CLIENT_ONEWAY_DELTA_INDICES, 1},
    {CLIENT_ROUNDTRIP_DELTA_INDICES, 6},
    {SERVER_ONEWAY_DELTA_INDICES, 3},
    {SERVER_ROUNDTRIP_DELTA_INDICES, 6},
    {CLIENT_SERVER_ONEWAY_DELTA_INDICES, 5},
    {CLIENT_SERVER_ROUNDTRIP_DELTA_INDICES, 14}
};

static inline uint64_t parse_timestamp_ns(const char* ts_str) {
    if (!ts_str || strcmp(ts_str, "NULL") == 0) return 0;
    
    char* dot_pos = strchr(ts_str, '.');
    if (!dot_pos) {
        uint64_t seconds = (uint64_t)atoll(ts_str);
        return seconds * 1000000000ULL;
    }
    
    uint64_t seconds = (uint64_t)atoll(ts_str);
    
    char frac_str[16] = {0};
    strncpy(frac_str, dot_pos + 1, 15);
    
    size_t frac_len = strlen(frac_str);
    if (frac_len > 9) {
        frac_str[9] = '\0';
        frac_len = 9;
    }
    
    for (size_t i = frac_len; i < 9; i++) {
        frac_str[i] = '0';
    }
    frac_str[9] = '\0';
    
    uint64_t nanoseconds = (uint64_t)atoll(frac_str);
    return seconds * 1000000000ULL + nanoseconds;
}

static inline delta_result_t calculate_delta_fast(uint64_t ts_a_ns, uint64_t ts_b_ns) {
    delta_result_t result = {0, 0, 0};
    
    if (ts_a_ns == 0 || ts_b_ns == 0 || ts_b_ns <= ts_a_ns) {
        return result;
    }
    
    uint64_t delta_ns = ts_b_ns - ts_a_ns;
    result.delta_whole_us = (uint32_t)(delta_ns / 1000);
    result.delta_frac_ns = (uint32_t)(delta_ns % 1000);
    result.valid = 1;
    
    return result;
}

static int format_delta_to_buffer(delta_result_t* delta, char* buffer, size_t max_len) {
    if (!delta || !buffer || max_len < 8) return 0;
    
    if (!delta->valid) {
        strcpy(buffer, "NULL");
        return 4;
    }
    
    return snprintf(buffer, max_len, "%u.%03u", delta->delta_whole_us, delta->delta_frac_ns);
}

static const mode_delta_config_t* get_mode_delta_config(operation_mode_t mode, target_type_t target) {
    int config_index = -1;
    
    if (mode == MODE_ONE_WAY && target == TARGET_CLIENT) config_index = 0;
    else if (mode == MODE_ROUND_TRIP && target == TARGET_CLIENT) config_index = 1;
    else if (mode == MODE_ONE_WAY && target == TARGET_SERVER) config_index = 2;
    else if (mode == MODE_ROUND_TRIP && target == TARGET_SERVER) config_index = 3;
    else if (mode == MODE_ONE_WAY && target == TARGET_CLIENT_SERVER) config_index = 4;
    else if (mode == MODE_ROUND_TRIP && target == TARGET_CLIENT_SERVER) config_index = 5;
    
    return (config_index >= 0) ? &MODE_DELTA_CONFIGS[config_index] : NULL;
}

static int calculate_mode_deltas(record_t* record, operation_mode_t mode, 
                               target_type_t target, delta_result_t* results, uint8_t* delta_count) {
    const mode_delta_config_t* config = get_mode_delta_config(mode, target);
    if (!config || !config->delta_indices) {
        *delta_count = 0;
        return 0;
    }
    
    *delta_count = config->delta_count;
    
    for (uint8_t i = 0; i < config->delta_count; i++) {
        uint8_t delta_idx = config->delta_indices[i];
        const delta_definition_t* def = &ALL_DELTA_DEFINITIONS[delta_idx];
        
        uint64_t ts_a_ns = parse_timestamp_ns(record->timestamps[def->ts_a_field]);
        uint64_t ts_b_ns = parse_timestamp_ns(record->timestamps[def->ts_b_field]);
        
        results[i] = calculate_delta_fast(ts_a_ns, ts_b_ns);
    }
    
    return 0;
}

/*
 * CSV PARSING AND FILE DETECTION
 */

static int parse_csv_line(char* line, char** fields, int max_fields) {
    if (!line || !fields || max_fields <= 0) return 0;
    
    int field_count = 0;
    char* field_start = line;
    char* current = line;
    int in_quotes = 0;
    
    while (*current && field_count < max_fields) {
        if (*current == '"' && (current == line || current[-1] == ',')) {
            in_quotes = !in_quotes;
        } else if (*current == ',' && !in_quotes) {
            *current = '\0';
            fields[field_count++] = trim_whitespace(field_start);
            field_start = current + 1;
        }
        current++;
    }
    
    if (field_count < max_fields && field_start <= current) {
        fields[field_count++] = trim_whitespace(field_start);
    }
    
    return field_count;
}

static file_type_t detect_file_type(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Cannot open file '%s': %s", filename, strerror(errno));
        return FILE_TYPE_UNKNOWN;
    }
    
    char line[MAX_LINE_LENGTH];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return FILE_TYPE_UNKNOWN;
    }
    
    fclose(fp);
    
    line[strcspn(line, "\r\n")] = '\0';
    
    /* Check for known headers */
    if (strstr(line, "clt_src_ip,clt_src_port,seq_num,clt_app_tx_tsc_ts,clt_app_tx_ts,clt_hw_rx_ts,clt_ker_rx_ts,clt_app_rx_tsc_ts,clt_app_rx_ts")) {
        return FILE_CLIENT_ROUNDTRIP_MAIN;
    } else if (strstr(line, "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts,svr_app_tx_ts")) {
        return FILE_SERVER_ROUNDTRIP_MAIN;
    } else if (strstr(line, "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts")) {
        return FILE_SERVER_ONEWAY_MAIN;
    } else if (strstr(line, "clt_src_ip,clt_src_port,seq_num,clt_app_tx_ts")) {
        return FILE_CLIENT_ONEWAY_MAIN;
    } else if (strstr(line, "clt_src_ip,clt_src_port,seq_num,clt_ker_tx_ts")) {
        return FILE_CLIENT_ONEWAY_TX;
    } else if (strstr(line, "clt_src_ip,clt_src_port,seq_num,svr_ker_tx_ts")) {
        return FILE_SERVER_ROUNDTRIP_TX;
    }
    
    return FILE_TYPE_UNKNOWN;
}

static int validate_record_match(char** fields, int field_count, const char* target_ip, uint16_t target_port) {
    if (field_count < 3) return 0;
    
    /* Check IP match */
    if (strcmp(fields[0], target_ip) != 0) return 0;
    
    /* Check port match */
    uint16_t file_port = (uint16_t)atoi(fields[1]);
    if (file_port != target_port) return 0;
    
    return 1;
}

static int extract_sequence_number(char** fields, int field_count, uint32_t* seq_num) {
    if (field_count < 3) return -1;
    
    char* endptr;
    long seq = strtol(fields[2], &endptr, 10);
    
    if (*endptr != '\0' || seq < 0 || seq > UINT32_MAX) {
        return -1;
    }
    
    *seq_num = (uint32_t)seq;
    return 0;
}

/*
 * CORE JOINING LOGIC
*/

static int sequence_set_add(sequence_set_t* set, uint32_t seq_num) {
    if (!set) return -1;
    
    for (size_t i = 0; i < set->count; i++) {
        if (set->sequences[i] == seq_num) {
            return 0;
        }
    }
    
    if (set->count >= set->capacity) {
        size_t new_capacity = set->capacity ? set->capacity * 2 : 1024;
        uint32_t* new_sequences = realloc(set->sequences, new_capacity * sizeof(uint32_t));
        if (!new_sequences) return -1;
        
        set->sequences = new_sequences;
        set->capacity = new_capacity;
    }
    
    set->sequences[set->count++] = seq_num;
    return 0;
}

static int load_sequences_from_file(const char* filename, const char* target_ip, 
                                   uint16_t target_port, sequence_set_t* seq_set) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Cannot open file '%s': %s", filename, strerror(errno));
        return -1;
    }
    
    char line[MAX_LINE_LENGTH];
    int line_num = 0;
    int sequences_found = 0;
    
    if (fgets(line, sizeof(line), fp)) {
        line_num++;
    }
    
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        line[strcspn(line, "\r\n")] = '\0';
        
        if (strlen(line) == 0) continue;
        
        char* fields[MAX_CSV_FIELDS];
        int field_count = parse_csv_line(line, fields, MAX_CSV_FIELDS);
        
        if (field_count < 3) {
            HW_LOG_WARN(HW_LOG_COMPONENT_CSV, "Skipping malformed line %d in '%s'", line_num, filename);
            continue;
        }
        
        if (!validate_record_match(fields, field_count, target_ip, target_port)) {
            continue;
        }
        
        uint32_t seq_num;
        if (extract_sequence_number(fields, field_count, &seq_num) != 0) {
            HW_LOG_WARN(HW_LOG_COMPONENT_CSV,  "Invalid sequence number on line %d in '%s'", line_num, filename);
            continue;
        }
        
        if (sequence_set_add(seq_set, seq_num) == 0) {
            sequences_found++;
        }
    }
    
    fclose(fp);
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Loaded %d sequences from '%s'", sequences_found, filename);
    return 0;
}

static int find_common_sequences(config_t* config, sequence_set_t* common_seqs) {
    sequence_set_t file_sequences[MAX_FILES] = {0};
    
    /* Load sequences from all files */
    for (int i = 0; i < config->input_file_count; i++) {
        if (load_sequences_from_file(config->input_files[i], config->target_ip, 
                                   config->target_port, &file_sequences[i]) != 0) {
            for (int j = 0; j <= i; j++) {
                free(file_sequences[j].sequences);
            }
            return -1;
        }
    }
    
    if (file_sequences[0].count == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "No matching sequences found");
        for (int i = 0; i < config->input_file_count; i++) {
            free(file_sequences[i].sequences);
        }
        return 0;
    }
    
    /* Initialize common_seqs with first file's sequences */
    common_seqs->sequences = malloc(file_sequences[0].count * sizeof(uint32_t));
    if (!common_seqs->sequences) {
        for (int i = 0; i < config->input_file_count; i++) {
            free(file_sequences[i].sequences);
        }
        return -1;
    }
    
    /* Find intersection across all files */
    for (size_t i = 0; i < file_sequences[0].count; i++) {
        uint32_t seq = file_sequences[0].sequences[i];
        int found_in_all = 1;
        
        /* Check if this sequence exists in all other files */
        for (int f = 1; f < config->input_file_count; f++) {
            int found = 0;
            for (size_t j = 0; j < file_sequences[f].count; j++) {
                if (file_sequences[f].sequences[j] == seq) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                found_in_all = 0;
                break;
            }
        }
        
        if (found_in_all) {
            common_seqs->sequences[common_seqs->count++] = seq;
        }
    }
    
    /* Sort the common sequences */
    qsort(common_seqs->sequences, common_seqs->count, sizeof(uint32_t), compare_uint32);
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Found %zu common sequences across all files", common_seqs->count);
    
    /* Cleanup file sequences */
    for (int i = 0; i < config->input_file_count; i++) {
        free(file_sequences[i].sequences);
    }
    
    return 0;
}

static record_t* create_record_table(sequence_set_t* common_seqs) {
    if (!common_seqs || common_seqs->count == 0) return NULL;
    
    record_t* records = calloc(common_seqs->count, sizeof(record_t));
    if (!records) return NULL;
    
    for (size_t i = 0; i < common_seqs->count; i++) {
        records[i].seq_num = common_seqs->sequences[i];
        records[i].field_mask = 0;
        for (int j = 0; j < MAX_TIMESTAMP_FIELDS; j++) {
            strcpy(records[i].timestamps[j], "NULL");
        }
    }
    
    return records;
}

static int find_record_by_seq(record_t* records, size_t record_count, uint32_t seq_num) {
    /* Binary search since records are sorted by sequence number */
    int left = 0, right = (int)record_count - 1;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        
        if (records[mid].seq_num == seq_num) {
            return mid;
        } else if (records[mid].seq_num < seq_num) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    
    return -1;
}

static int populate_timestamps_from_file(const char* filename, file_type_t file_type,
                                        const char* target_ip, uint16_t target_port,
                                        record_t* records, size_t record_count) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Cannot open file '%s': %s", filename, strerror(errno));
        return -1;
    }
    
    char line[MAX_LINE_LENGTH];
    int line_num = 0;
    int records_populated = 0;
    
    if (fgets(line, sizeof(line), fp)) {
        line_num++;
    }
    
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        line[strcspn(line, "\r\n")] = '\0';
        
        if (strlen(line) == 0) continue;
        
        char* fields[MAX_CSV_FIELDS];
        int field_count = parse_csv_line(line, fields, MAX_CSV_FIELDS);
        
        if (field_count < 3) continue;
        
        if (!validate_record_match(fields, field_count, target_ip, target_port)) {
            continue;
        }
        
        uint32_t seq_num;
        if (extract_sequence_number(fields, field_count, &seq_num) != 0) {
            continue;
        }
        
        int record_idx = find_record_by_seq(records, record_count, seq_num);
        if (record_idx == -1) continue; /* Sequence not in common set */
        
        /* Populate timestamp fields based on file type */
        switch (file_type) {
            case FILE_CLIENT_ONEWAY_MAIN:
                if (field_count >= 4) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_APP_TX_TS], fields[3], 32);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_APP_TX_TS);
                }
                break;
                
            case FILE_CLIENT_ONEWAY_TX:
            case FILE_CLIENT_ROUNDTRIP_TX:
                if (field_count >= 4) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_KER_TX_TS], fields[3], 32);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_KER_TX_TS);
                }
                break;
                
            case FILE_CLIENT_ROUNDTRIP_MAIN:
                if (field_count >= 9) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_APP_TX_TSC_TS], fields[3], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_APP_TX_TS], fields[4], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_HW_RX_TS], fields[5], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_KER_RX_TS], fields[6], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_APP_RX_TSC_TS], fields[7], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_CLT_APP_RX_TS], fields[8], 32);
                    
                    records[record_idx].field_mask |= (1 << FIELD_CLT_APP_TX_TSC_TS);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_APP_TX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_HW_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_KER_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_APP_RX_TSC_TS);
                    records[record_idx].field_mask |= (1 << FIELD_CLT_APP_RX_TS);
                }
                break;
                
            case FILE_SERVER_ONEWAY_MAIN:
                if (field_count >= 6) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_HW_RX_TS], fields[3], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_KER_RX_TS], fields[4], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_APP_RX_TS], fields[5], 32);
                    
                    records[record_idx].field_mask |= (1 << FIELD_SVR_HW_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_KER_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_APP_RX_TS);
                }
                break;
                
            case FILE_SERVER_ROUNDTRIP_MAIN:
                if (field_count >= 7) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_HW_RX_TS], fields[3], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_KER_RX_TS], fields[4], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_APP_RX_TS], fields[5], 32);
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_APP_TX_TS], fields[6], 32);
                    
                    records[record_idx].field_mask |= (1 << FIELD_SVR_HW_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_KER_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_APP_RX_TS);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_APP_TX_TS);
                }
                break;
                
            case FILE_SERVER_ROUNDTRIP_TX:
                if (field_count >= 4) {
                    safe_strncpy(records[record_idx].timestamps[FIELD_SVR_KER_TX_TS], fields[3], 32);
                    records[record_idx].field_mask |= (1 << FIELD_SVR_KER_TX_TS);
                }
                break;
                
            default:
                break;
        }
        
        records_populated++;
    }
    
    fclose(fp);
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Populated %d records from '%s' (%s)", records_populated, filename, get_file_type_name(file_type));
    return 0;
}

/*
 * OUTPUT GENERATION
*/

static const char* get_output_header_with_deltas(operation_mode_t mode, target_type_t target) {
    switch (mode) {
        case MODE_ONE_WAY:
            switch (target) {
                case TARGET_CLIENT:
                    return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_ts,clt_ker_tx_ts,delta_d1_clt_app_tx_to_ker_tx_us";
                case TARGET_SERVER:
                    return "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts,delta_d2_svr_hw_rx_to_ker_rx_us,"
                    "delta_d3_svr_ker_rx_to_app_rx_us,delta_tt_d1_svr_hw_rx_to_app_rx_us";
                case TARGET_CLIENT_SERVER:
                    return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_ts,clt_ker_tx_ts,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts,"
                    "delta_d1_clt_app_tx_to_ker_tx_us,delta_d2_svr_hw_rx_to_ker_rx_us,delta_d3_svr_ker_rx_to_app_rx_us,"
                    "delta_tt_d1_svr_hw_rx_to_app_rx_us,delta_net_clt_ker_tx_to_svr_hw_rx_us";
            }
            break;
        case MODE_ROUND_TRIP:
            switch (target) {
                case TARGET_CLIENT:
                    return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_tsc_ts,clt_app_tx_ts,clt_ker_tx_ts,clt_hw_rx_ts,"
                    "clt_ker_rx_ts,clt_app_rx_tsc_ts,clt_app_rx_ts,delta_d1_clt_app_tx_to_ker_tx_us,delta_d6_clt_hw_rx_to_ker_rx_us,"
                    "delta_d7_clt_ker_rx_to_app_rx_us,delta_rtt_d1_clt_app_tx_to_hw_rx_us,delta_rtt_d2_clt_app_tx_to_app_rx_us,"
                    "delta_rtt_d3_clt_app_tx_tsc_to_app_rx_tsc_us";
                case TARGET_SERVER:
                    return "clt_src_ip,clt_src_port,seq_num,svr_hw_rx_ts,svr_ker_rx_ts,svr_app_rx_ts,svr_app_tx_ts,svr_ker_tx_ts,"
                    "delta_d2_svr_hw_rx_to_ker_rx_us,delta_d3_svr_ker_rx_to_app_rx_us,delta_d4_svr_app_rx_to_app_tx_us,"
                    "delta_d5_svr_app_tx_to_ker_tx_us,delta_tt_d1_svr_hw_rx_to_app_rx_us,delta_tt_d2_svr_hw_rx_to_ker_tx_us";
                case TARGET_CLIENT_SERVER:
                    return "clt_src_ip,clt_src_port,seq_num,clt_app_tx_tsc_ts,clt_app_tx_ts,clt_ker_tx_ts,svr_hw_rx_ts,"
                    "svr_ker_rx_ts,svr_app_rx_ts,svr_app_tx_ts,svr_ker_tx_ts,clt_hw_rx_ts,clt_ker_rx_ts,clt_app_rx_tsc_ts,"
                    "clt_app_rx_ts,delta_d1_clt_app_tx_to_ker_tx_us,delta_d6_clt_hw_rx_to_ker_rx_us,delta_d7_clt_ker_rx_to_app_rx_us,"
                    "delta_rtt_d1_clt_app_tx_to_hw_rx_us,delta_rtt_d2_clt_app_tx_to_app_rx_us,delta_rtt_d3_clt_app_tx_tsc_to_app_rx_tsc_us,"
                    "delta_d2_svr_hw_rx_to_ker_rx_us,delta_d3_svr_ker_rx_to_app_rx_us,delta_d4_svr_app_rx_to_app_tx_us,"
                    "delta_d5_svr_app_tx_to_ker_tx_us,delta_tt_d1_svr_hw_rx_to_app_rx_us,delta_tt_d2_svr_hw_rx_to_ker_tx_us,"
                    "delta_net_clt_ker_tx_to_svr_hw_rx_us,delta_net_svr_ker_tx_to_clt_hw_rx_us";
            }
            break;
    }
    return "";
}

static const char* get_output_header(operation_mode_t mode, target_type_t target) {
    return get_output_header_with_deltas(mode, target);
}

static int format_output_record(record_t* record, const char* target_ip, 
                               uint16_t target_port, operation_mode_t mode, 
                               target_type_t target, char* output_line, size_t max_len) {
    int written = 0;
    
    /* Always start with IP, port, and sequence number */
    written += snprintf(output_line, max_len, "%s,%u,%u", target_ip, target_port, record->seq_num);
    
    switch (mode) {
        case MODE_ONE_WAY:
            switch (target) {
                case TARGET_CLIENT:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s",
                        record->timestamps[FIELD_CLT_APP_TX_TS],
                        record->timestamps[FIELD_CLT_KER_TX_TS]);
                    break;
                case TARGET_SERVER:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s,%s",
                        record->timestamps[FIELD_SVR_HW_RX_TS],
                        record->timestamps[FIELD_SVR_KER_RX_TS],
                        record->timestamps[FIELD_SVR_APP_RX_TS]);
                    break;
                case TARGET_CLIENT_SERVER:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s,%s,%s,%s",
                        record->timestamps[FIELD_CLT_APP_TX_TS],
                        record->timestamps[FIELD_CLT_KER_TX_TS],
                        record->timestamps[FIELD_SVR_HW_RX_TS],
                        record->timestamps[FIELD_SVR_KER_RX_TS],
                        record->timestamps[FIELD_SVR_APP_RX_TS]);
                    break;
            }
            break;
        case MODE_ROUND_TRIP:
            switch (target) {
                case TARGET_CLIENT:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s,%s,%s,%s,%s,%s",
                        record->timestamps[FIELD_CLT_APP_TX_TSC_TS],
                        record->timestamps[FIELD_CLT_APP_TX_TS],
                        record->timestamps[FIELD_CLT_KER_TX_TS],
                        record->timestamps[FIELD_CLT_HW_RX_TS],
                        record->timestamps[FIELD_CLT_KER_RX_TS],
                        record->timestamps[FIELD_CLT_APP_RX_TSC_TS],
                        record->timestamps[FIELD_CLT_APP_RX_TS]);
                    break;
                case TARGET_SERVER:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s,%s,%s,%s",
                        record->timestamps[FIELD_SVR_HW_RX_TS],
                        record->timestamps[FIELD_SVR_KER_RX_TS],
                        record->timestamps[FIELD_SVR_APP_RX_TS],
                        record->timestamps[FIELD_SVR_APP_TX_TS],
                        record->timestamps[FIELD_SVR_KER_TX_TS]);
                    break;
                case TARGET_CLIENT_SERVER:
                    written += snprintf(output_line + written, max_len - written, ",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s",
                        record->timestamps[FIELD_CLT_APP_TX_TSC_TS],
                        record->timestamps[FIELD_CLT_APP_TX_TS],
                        record->timestamps[FIELD_CLT_KER_TX_TS],
                        record->timestamps[FIELD_SVR_HW_RX_TS],
                        record->timestamps[FIELD_SVR_KER_RX_TS],
                        record->timestamps[FIELD_SVR_APP_RX_TS],
                        record->timestamps[FIELD_SVR_APP_TX_TS],
                        record->timestamps[FIELD_SVR_KER_TX_TS],
                        record->timestamps[FIELD_CLT_HW_RX_TS],
                        record->timestamps[FIELD_CLT_KER_RX_TS],
                        record->timestamps[FIELD_CLT_APP_RX_TSC_TS],
                        record->timestamps[FIELD_CLT_APP_RX_TS]);
                    break;
            }
            break;
    }
    
    /* Calculate and append delta columns */
    delta_result_t deltas[MAX_DELTAS];
    uint8_t delta_count = 0;
    
    if (calculate_mode_deltas(record, mode, target, deltas, &delta_count) == 0) {
        for (uint8_t i = 0; i < delta_count; i++) {
            char delta_buffer[16];
            format_delta_to_buffer(&deltas[i], delta_buffer, sizeof(delta_buffer));
            written += snprintf(output_line + written, max_len - written, ",%s", delta_buffer);
        }
    }
    
    return written;
}

static int write_output_csv(const char* filename, record_t* records, 
                           size_t record_count, config_t* config) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Cannot create output file '%s': %s", filename, strerror(errno));
        return -1;
    }
    
    const char* header = get_output_header(config->mode, config->target);
    fprintf(fp, "%s\n", header);
    
    char output_line[MAX_LINE_LENGTH];
    int records_written = 0;
    
    for (size_t i = 0; i < record_count; i++) {
        if (format_output_record(&records[i], config->target_ip, config->target_port,
                                config->mode, config->target, output_line, sizeof(output_line)) > 0) {
            fprintf(fp, "%s\n", output_line);
            records_written++;
        }
    }
    
    fclose(fp);
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Wrote %d records to '%s'", records_written, filename);
    return 0;
}

static int process_join_operation(config_t* config) {
    sequence_set_t common_seqs = {0};
    
    /* Find common sequences across all input files */
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Finding common sequences");
    if (find_common_sequences(config, &common_seqs) != 0) {
        return -1;
    }
    
    if (common_seqs.count == 0) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "No common sequences found - nothing to join");
        return 0;
    }
    
    /* Create record table for common sequences */
    record_t* records = create_record_table(&common_seqs);
    if (!records) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to allocate memory for records");
        free(common_seqs.sequences);
        return -1;
    }
    
    /* Populate timestamp data from each file */
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Populating timestamp data");
    for (int i = 0; i < config->input_file_count; i++) {
        if (populate_timestamps_from_file(config->input_files[i], config->detected_types[i],
                                        config->target_ip, config->target_port,
                                        records, common_seqs.count) != 0) {
            free(records);
            free(common_seqs.sequences);
            return -1;
        }
    }
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Writing output");
    if (write_output_csv(config->output_file, records, common_seqs.count, config) != 0) {
        free(records);
        free(common_seqs.sequences);
        return -1;
    }
    
    /* Cleanup */
    free(records);
    free(common_seqs.sequences);
    
    return 0;
}

/*
 * COMMAND LINE PROCESSING
*/

static void print_usage(const char* progname) {
    fprintf(stderr, "Usage: %s [MODE] [TARGET] [OPTIONS]\n\n", progname);
    
    fprintf(stderr, "Mode (exactly one required):\n");
    fprintf(stderr, "  --one-way                    Operate on one-way timestamp CSV files\n");
    fprintf(stderr, "  --round-trip                 Operate on round-trip timestamp CSV files\n\n");
    
    fprintf(stderr, "Target (exactly one required):\n");
    fprintf(stderr, "  --client                     Join CSV files created by client only\n");
    fprintf(stderr, "  --server                     Join CSV files created by server only\n");
    fprintf(stderr, "  --client-server              Join CSV files created by both client and server\n\n");
    
    fprintf(stderr, "Required options:\n");
    fprintf(stderr, "  --clt-src-ip <ip>            Client source IP address to join on\n");
    fprintf(stderr, "  --clt-src-port <port>        Client source port to join on\n");
    fprintf(stderr, "  --input-files <file1,file2>  Comma-separated input CSV files\n\n");
    
    fprintf(stderr, "Optional options:\n");
    fprintf(stderr, "  --output-csv <filename>      Output CSV filename (default: joined_output.csv)\n");
    fprintf(stderr, "  --help                       Show this help message\n\n");
    
    fprintf(stderr, "Valid combinations:\n");
    fprintf(stderr, "  --one-way --client           (2 files): client main CSV + client TX CSV\n");
    fprintf(stderr, "  --round-trip --client        (2 files): client main CSV + client TX CSV\n");
    fprintf(stderr, "  --round-trip --server        (2 files): server main CSV + server TX CSV\n");
    fprintf(stderr, "  --one-way --client-server    (3 files): client main CSV + client TX CSV + server main CSV\n");
    fprintf(stderr, "  --round-trip --client-server (4 files): client main CSV + client TX CSV + server main CSV + server TX CSV\n\n");
    
}

static int parse_input_files(const char* files_str, config_t* config) {
    if (!files_str || !config) return -1;
    
    char* files_copy = strdup(files_str);
    if (!files_copy) return -1;
    
    config->input_file_count = 0;
    char* file = strtok(files_copy, ",");
    
    while (file && config->input_file_count < MAX_FILES) {
        file = trim_whitespace(file);
        if (strlen(file) > 0) {
            safe_strncpy(config->input_files[config->input_file_count], file, MAX_FILENAME_LENGTH);
            config->input_file_count++;
        }
        file = strtok(NULL, ",");
    }
    
    free(files_copy);
    return 0;
}

static int parse_arguments(int argc, char* argv[], config_t* config) {
    int mode_set = 0, target_set = 0;
    int clt_ip_set = 0, clt_port_set = 0, input_files_set = 0;
    
    static struct option long_options[] = {
        {"one-way", no_argument, 0, 'w'},
        {"round-trip", no_argument, 0, 'r'},
        {"client", no_argument, 0, 'c'},
        {"server", no_argument, 0, 's'},
        {"client-server", no_argument, 0, 'b'},
        {"clt-src-ip", required_argument, 0, 'i'},
        {"clt-src-port", required_argument, 0, 'p'},
        {"input-files", required_argument, 0, 'f'},
        {"output-csv", required_argument, 0, 'o'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;

    strcpy(config->output_file, "joined_output.csv");
    
    while ((c = getopt_long(argc, argv, "wrcsbhi:p:f:o:", long_options, &option_index)) != -1) {
        switch (c) {
            case 'w':
                if (mode_set) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Multiple modes specified");
                    return -1;
                }
                config->mode = MODE_ONE_WAY;
                mode_set = 1;
                break;
            case 'r':
                if (mode_set) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Multiple modes specified");
                    return -1;
                }
                config->mode = MODE_ROUND_TRIP;
                mode_set = 1;
                break;
            case 'c':
                if (target_set) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Multiple targets specified");
                    return -1;
                }
                config->target = TARGET_CLIENT;
                target_set = 1;
                break;
            case 's':
                if (target_set) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Multiple targets specified");
                    return -1;
                }
                config->target = TARGET_SERVER;
                target_set = 1;
                break;
            case 'b':
                if (target_set) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Multiple targets specified");
                    return -1;
                }
                config->target = TARGET_CLIENT_SERVER;
                target_set = 1;
                break;
            case 'i':
                safe_strncpy(config->target_ip, optarg, MAX_IP_LENGTH);
                clt_ip_set = 1;
                break;
            case 'p':
                config->target_port = (uint16_t)atoi(optarg);
                if (config->target_port == 0) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Invalid port number '%s'", optarg);
                    return -1;
                }
                clt_port_set = 1;
                break;
            case 'f':
                if (parse_input_files(optarg, config) != 0) {
                    HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Failed to parse input files");
                    return -1;
                }
                input_files_set = 1;
                break;
            case 'o':
                safe_strncpy(config->output_file, optarg, MAX_FILENAME_LENGTH);
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case '?':
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    if (!mode_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Mode not specified (use --one-way or --round-trip)");
        return -1;
    }
    if (!target_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Target not specified (use --client, --server, or --client-server)");
        return -1;
    }
    if (!clt_ip_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Client source IP not specified (use --clt-src-ip)");
        return -1;
    }
    if (!clt_port_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Client source port not specified (use --clt-src-port)");
        return -1;
    }
    if (!input_files_set) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Input files not specified (use --input-files)");
        return -1;
    }
    
    return 0;
}

static int validate_file_combination(config_t* config) {
    int expected_file_count = 0;
    
    switch (config->mode) {
        case MODE_ONE_WAY:
            switch (config->target) {
                case TARGET_CLIENT: expected_file_count = 2; break;
                case TARGET_SERVER: expected_file_count = 1; break;
                case TARGET_CLIENT_SERVER: expected_file_count = 3; break;
            }
            break;
        case MODE_ROUND_TRIP:
            switch (config->target) {
                case TARGET_CLIENT: expected_file_count = 2; break;
                case TARGET_SERVER: expected_file_count = 2; break;
                case TARGET_CLIENT_SERVER: expected_file_count = 4; break;
            }
            break;
    }
    
    if (config->input_file_count != expected_file_count) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Expected %d input files for this mode/target combination, got %d",
                expected_file_count, config->input_file_count);
        return -1;
    }
    
    /* Detect file types */
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Detecting file types:");
    for (int i = 0; i < config->input_file_count; i++) {
        config->detected_types[i] = detect_file_type(config->input_files[i]);
        if (config->detected_types[i] == FILE_TYPE_UNKNOWN) {
            HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Cannot determine type of file '%s'", config->input_files[i]);
            return -1;
        }
        printf("       %s: %s\n", config->input_files[i], get_file_type_name(config->detected_types[i]));
    }
    
    /* Simple validation that file types match the selected mode/target */
    int has_client_main = 0, has_client_tx = 0;
    int has_server_main = 0, has_server_tx = 0;
    
    for (int i = 0; i < config->input_file_count; i++) {
        switch (config->detected_types[i]) {
            case FILE_CLIENT_ONEWAY_MAIN:
            case FILE_CLIENT_ROUNDTRIP_MAIN:
                has_client_main = 1;
                break;
            case FILE_CLIENT_ONEWAY_TX:
            case FILE_CLIENT_ROUNDTRIP_TX:
                has_client_tx = 1;
                break;
            case FILE_SERVER_ONEWAY_MAIN:
            case FILE_SERVER_ROUNDTRIP_MAIN:
                has_server_main = 1;
                break;
            case FILE_SERVER_ROUNDTRIP_TX:
                has_server_tx = 1;
                break;
            default:
                break;
        }
    }
    
    if (config->target == TARGET_CLIENT && (has_server_main || has_server_tx)) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Server files provided for client-only operation");
        return -1;
    }
    if (config->target == TARGET_SERVER && (has_client_main || has_client_tx)) {
        HW_LOG_ERROR(HW_LOG_COMPONENT_CSV, "Client files provided for server-only operation");
        return -1;
    }
    
    return 0;
}

/*
 * MAIN PROGRAM
*/

int main(int argc, char* argv[]) {
    config_t config = {0};
    
    hw_log_init();
    
    if (parse_arguments(argc, argv, &config) != 0) {
        return EXIT_FAILURE;
    }
    
    if (validate_file_combination(&config) != 0) {
        return EXIT_FAILURE;
    }
    
    printf("\n");
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Processing join operation");
    if (process_join_operation(&config) != 0) {
        return EXIT_FAILURE;
    }

    hw_log_cleanup();
    
    HW_LOG_INFO(HW_LOG_COMPONENT_CSV, "Finished");
    return EXIT_SUCCESS;
}
