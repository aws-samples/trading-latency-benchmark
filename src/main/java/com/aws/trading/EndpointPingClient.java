package com.aws.trading;

import org.HdrHistogram.Histogram;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.io.PrintStream;
import java.net.InetAddress;
import java.util.LinkedHashMap;
import java.util.Timer;
import java.util.TimerTask;

import static com.aws.trading.Config.*;
import static com.aws.trading.LatencyTools.formatNanos;
import static com.aws.trading.RoundTripLatencyTester.getLogFile;
import static com.aws.trading.RoundTripLatencyTester.saveHistogramToFile;

public class EndpointPingClient {
    public static final Histogram HISTOGRAM = new Histogram(Long.MAX_VALUE, 2);
    private static final Logger LOGGER = LogManager.getLogger(EndpointPingClient.class);
    public static void main(String[] args) {
        Timer timer = new Timer();
        timer.scheduleAtFixedRate(new PingTask(HOST), 0, PING_INTERVAL);
    }

    private static class PingTask extends TimerTask {
        private final String host;
        private long count = 0;
        private final SingleWriterRecorder hdrRecorder = new SingleWriterRecorder(Long.MAX_VALUE, 2);
        public PingTask(String host) {
            this.host = host;
        }

        @Override
        public void run() {
            try {
                InetAddress address = InetAddress.getByName(host);
                long startTime = System.nanoTime();
                boolean isReachable = address.isReachable(5000); // Timeout after 5 seconds
                long endTime = System.nanoTime();
                long latency = endTime - startTime;
                count += 1;
                if (count < WARMUP_COUNT) {
                    LOGGER.info("warming up... - message count: {}", count);
                    return;
                }
                if (isReachable) {
                    LOGGER.info(" {} is reachable. Latency: {}",host, formatNanos(latency));
                    hdrRecorder.recordValue(latency);
                    if (count % TEST_SIZE == 0) {
                        HISTOGRAM.add(hdrRecorder.getIntervalHistogram());
                        LinkedHashMap<String, String> latencyReport = LatencyTools.createLatencyReport(HISTOGRAM);
                        try (PrintStream histogramLogFile = getLogFile()) {
                            saveHistogramToFile(HISTOGRAM, System.nanoTime(), histogramLogFile);
                        } catch (IOException e) {
                            LOGGER.error(e);
                        }
                        LOGGER.info("Percentiles: {} \n", LatencyTools.toJSON(latencyReport));
                    }
                } else {
                    LOGGER.error("{} is not reachable.", host);
                }
            } catch (IOException e) {
                LOGGER.error("Error: {}", e.getMessage());
            }
        }
    }
}
