package com.aws.trading;

import org.HdrHistogram.Histogram;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Timer;
import java.util.TimerTask;
import java.util.concurrent.ConcurrentHashMap;

import static com.aws.trading.Config.*;
import static com.aws.trading.LatencyTools.formatNanos;
import static com.aws.trading.RoundTripLatencyTester.getLogFile;
import static com.aws.trading.RoundTripLatencyTester.saveHistogramToFile;

/**
 * Enhanced ping client that measures latency impact of different payload sizes.
 * Tests TCP connect + data send latency for various payload sizes to understand
 * how packet size affects network latency without requiring server cooperation.
 * 
 * This is useful for understanding if the latency difference between ping tests
 * and actual trading traffic is due to packet size differences.
 */
public class PayloadSizePingClient {
    private static final Logger LOGGER = LogManager.getLogger(PayloadSizePingClient.class);
    private static final Map<String, Boolean> completedTasks = new ConcurrentHashMap<>();
    private static int totalTasks = 0;

    public static void main(String[] args) throws IOException {
        Timer timer = new Timer();
        String[] hosts = HOST.split(",");

        LOGGER.info("Starting Payload Size Ping Client");
        LOGGER.info("Testing payload sizes: {}", PAYLOAD_SIZES);
        LOGGER.info("Ping interval: {} ms", PING_INTERVAL);

        for (String host : hosts) {
            String[] arr = host.trim().split(":");
            String hostname = arr[0];
            int port = arr.length > 1 ? Integer.parseInt(arr[1]) : WEBSOCKET_PORT;

            LOGGER.info("Setting up tests for {}:{}", hostname, port);
            
            for (Integer payloadSize : PAYLOAD_SIZES) {
                String taskKey = String.format("%s:%d:%d", hostname, port, payloadSize);
                completedTasks.put(taskKey, false);
                totalTasks++;
                
                PayloadSizePingTask task = new PayloadSizePingTask(hostname, port, payloadSize, taskKey, timer);
                timer.scheduleAtFixedRate(task, 0, PING_INTERVAL);
                LOGGER.info("Scheduled payload size test: {} bytes", payloadSize);
            }
        }

        LOGGER.info("Total tasks scheduled: {}", totalTasks);

        // Add shutdown hook to generate summary report
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            LOGGER.info("Shutting down, generating summary report...");
            timer.cancel();
        }));
    }
    
    private static synchronized void checkAndExitIfAllCompleted() {
        long completedCount = completedTasks.values().stream().filter(v -> v).count();
        LOGGER.info("Completed tasks: {}/{}", completedCount, totalTasks);
        
        if (completedCount == totalTasks) {
            LOGGER.info("All payload size tests completed. Exiting gracefully.");
            System.exit(0);
        }
    }

    /**
     * Task that measures TCP connect + send latency for a specific payload size.
     */
    private static class PayloadSizePingTask extends TimerTask {
        private final String host;
        private final int port;
        private final int payloadSize;
        private final String taskKey;
        private final Timer timer;
        private final byte[] payload;
        private final Histogram histogram;
        private final SingleWriterRecorder hdrRecorder;
        private final PrintStream histogramLogFile;
        private long count = 0;
        private long successCount = 0;
        private long errorCount = 0;

        public PayloadSizePingTask(String host, int port, int payloadSize, String taskKey, Timer timer) throws IOException {
            this.host = host;
            this.port = port;
            this.payloadSize = payloadSize;
            this.taskKey = taskKey;
            this.timer = timer;
            
            // Pre-allocate payload buffer filled with test data
            this.payload = new byte[payloadSize];
            Arrays.fill(payload, (byte) 'A');
            
            this.histogram = new Histogram(Long.MAX_VALUE, HISTOGRAM_SIGNIFICANT_FIGURES);
            this.hdrRecorder = new SingleWriterRecorder(Long.MAX_VALUE, HISTOGRAM_SIGNIFICANT_FIGURES);
            
            // Create output directory for this payload size
            String folderPath = String.format("./%s_%d/%dbytes", 
                host.replace(".", "_"), port, payloadSize);
            File folder = new File(folderPath);
            if (!folder.exists()) {
                folder.mkdirs();
            }
            
            this.histogramLogFile = getLogFile(folderPath + "/histogram.hlog");
            LOGGER.info("Created histogram log file: {}/histogram.hlog", folderPath);
        }

        /**
         * Measures the time to:
         * 1. TCP connect to endpoint
         * 2. Send payload data
         * 3. Wait for TCP ACK (implicit with TCP_NODELAY and flush)
         * 
         * This measures network + TCP stack overhead without requiring server cooperation.
         */
        private long measureConnectAndSendLatency() throws IOException {
            Socket socket = null;
            try {
                socket = new Socket();
                
                // Enable TCP_NODELAY to disable Nagle's algorithm for immediate send
                socket.setTcpNoDelay(true);
                
                // Set reasonable timeout
                socket.setSoTimeout(5000);
                
                long startTime = System.nanoTime();
                
                // Connect to endpoint
                InetSocketAddress socketAddress = new InetSocketAddress(host, port);
                socket.connect(socketAddress, 5000);
                
                // Send payload if size > 0
                if (payloadSize > 0) {
                    OutputStream out = socket.getOutputStream();
                    out.write(payload);
                    out.flush(); // Force send and wait for TCP ACK
                }
                
                long endTime = System.nanoTime();
                
                return endTime - startTime;
            } finally {
                if (socket != null && !socket.isClosed()) {
                    try {
                        socket.close();
                    } catch (IOException e) {
                        // Ignore close errors
                    }
                }
            }
        }

        @Override
        public void run() {
            try {
                long latency = measureConnectAndSendLatency();
                count++;
                successCount++;
                
                if (count < WARMUP_COUNT) {
                    LOGGER.info("warming up... payload_size={} bytes, message count: {}", 
                        payloadSize, count);
                    return;
                }

                LOGGER.info("{}:{} reachable. Payload: {} bytes, Latency: {}", 
                    host, port, payloadSize, formatNanos(latency));
                
                hdrRecorder.recordValue(latency);
                
                if (count % TEST_SIZE == 0) {
                    histogram.add(hdrRecorder.getIntervalHistogram());
                    LinkedHashMap<String, String> latencyReport = LatencyTools.createLatencyReport(histogram);
                    saveHistogramToFile(histogram, System.nanoTime(), histogramLogFile);
                    
                    LOGGER.info("Percentiles for {}:{} with {} byte payload:\n{}\n", 
                        host, port, payloadSize, LatencyTools.toJSON(latencyReport));
                    LOGGER.info("Success rate: {}/{} ({:.2f}%)", 
                        successCount, count, (successCount * 100.0 / count));
                    
                    // Mark this task as completed
                    LOGGER.info("Test completed for {}:{} with {} byte payload. Reached TEST_SIZE: {}", 
                        host, port, payloadSize, TEST_SIZE);
                    histogramLogFile.close();
                    completedTasks.put(taskKey, true);
                    this.cancel(); // Cancel this timer task
                    
                    // Check if all tasks are completed
                    checkAndExitIfAllCompleted();
                }
            } catch (IOException e) {
                errorCount++;
                LOGGER.error("{}:{} is not reachable (payload size: {} bytes). Error: {}", 
                    host, port, payloadSize, e.getMessage());
                
                if (count % 10 == 0) {
                    LOGGER.warn("Error rate for {} byte payload: {}/{} ({:.2f}%)", 
                        payloadSize, errorCount, count, (errorCount * 100.0 / count));
                }
            }
        }
    }
}
