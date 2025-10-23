/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
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
package com.aws.trading;

/**
 * Application-wide constants for the trading latency benchmark.
 */
public final class Constants {
    
    // Prevent instantiation
    private Constants() {
        throw new AssertionError("Constants class should not be instantiated");
    }
    
    // WebSocket Protocol Constants
    public static final class WebSocket {
        private WebSocket() {}
        
        /** Maximum WebSocket frame payload size in bytes (1.28 MB) */
        public static final int MAX_FRAME_SIZE = 1280000;
        
        /** WebSocket message type field name */
        public static final String FIELD_TYPE = "type";
        
        /** WebSocket client_id field name */
        public static final String FIELD_CLIENT_ID = "client_id";
        
        /** WebSocket instrument_code field name */
        public static final String FIELD_INSTRUMENT_CODE = "instrument_code";
    }
    
    // Message Type Constants
    public static final class MessageType {
        private MessageType() {}
        
        /** Order booked confirmation message type */
        public static final String BOOKED = "BOOKED";
        
        /** Order done/cancelled confirmation message type */
        public static final String DONE = "DONE";
        
        /** Authentication success message type */
        public static final String AUTHENTICATED = "AUTHENTICATED";
        
        /** Subscription confirmation message type */
        public static final String SUBSCRIPTIONS = "SUBSCRIPTIONS";
    }
    
    // HDR Histogram Constants
    public static final class Histogram {
        private Histogram() {}
        
        /** Maximum value for HDR histogram recording (in nanoseconds) */
        public static final long MAX_VALUE = Long.MAX_VALUE;
        
        /** Number of significant digits for HDR histogram precision */
        public static final int SIGNIFICANT_DIGITS = 2;
    }
    
    // Latency Thresholds
    public static final class Latency {
        private Latency() {}
        
        /** Minimum valid round-trip time in nanoseconds (should be positive) */
        public static final long MIN_VALID_RTT_NANOS = 0L;
        
        /** Warning threshold for unusually high latency (10 seconds in nanoseconds) */
        public static final long WARNING_THRESHOLD_NANOS = 10_000_000_000L;
    }
    
    // Network Constants
    public static final class Network {
        private Network() {}
        
        /** Minimum valid port number */
        public static final int MIN_PORT = 1;
        
        /** Maximum valid port number */
        public static final int MAX_PORT = 65535;
        
        /** Default connection timeout in milliseconds */
        public static final int DEFAULT_CONNECTION_TIMEOUT_MS = 30000;
    }
}
