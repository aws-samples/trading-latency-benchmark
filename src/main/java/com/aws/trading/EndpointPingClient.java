package com.aws.trading;

import org.HdrHistogram.Histogram;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
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
    private static final Logger LOGGER = LogManager.getLogger(EndpointPingClient.class);

    public static void main(String[] args) throws IOException {
        Timer timer = new Timer();
        String[] hosts = HOST.split(",");
        for (String host : hosts) {
            timer.scheduleAtFixedRate(new PingTask(host.trim()), 0, PING_INTERVAL);
        }
    }

    private static class PingTask extends TimerTask {
        final Histogram HISTOGRAM = new Histogram(Long.MAX_VALUE, 2);
        private final String host;
        private final PrintStream histogramLogFile;
        private long count = 0;
        private final SingleWriterRecorder hdrRecorder = new SingleWriterRecorder(Long.MAX_VALUE, 2);

        public PingTask(String host) throws IOException {
            this.host = host;
            String folderPath = "./" + host.replace(".", "_");
            File folder = new File(folderPath);
            if (!folder.exists()) {
                folder.mkdirs(); // Create the folder if it doesn't exist
            }
            this.histogramLogFile = getLogFile(folderPath + "/histogram.hlog");
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
                    LOGGER.info(" {} is reachable. Latency: {}", host, formatNanos(latency));
                    hdrRecorder.recordValue(latency);
                    if (count % TEST_SIZE == 0) {
                        HISTOGRAM.add(hdrRecorder.getIntervalHistogram());
                        LinkedHashMap<String, String> latencyReport = LatencyTools.createLatencyReport(HISTOGRAM);
                        saveHistogramToFile(HISTOGRAM, System.nanoTime(), histogramLogFile);
                        LOGGER.info("Percentiles for host: {}\n {}\n", host, LatencyTools.toJSON(latencyReport));
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
