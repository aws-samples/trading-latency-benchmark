package com.aws.trading;

import org.HdrHistogram.Histogram;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.IOException;
import java.io.PrintStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
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
            var arr = host.trim().split(":");
            timer.scheduleAtFixedRate(arr.length > 1 ? new PingWithHostAndPort(arr[0], Integer.parseInt(arr[1])) : new PingWithHost(arr[0]), 0, PING_INTERVAL);
        }
    }

    private abstract static class PingTask extends TimerTask {
        final Histogram HISTOGRAM = new Histogram(Long.MAX_VALUE, 4);
        protected final String host;
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

        abstract long measureRTT() throws IOException;

        @Override
        public void run() {
            try {
                long latency = measureRTT();
                count += 1;
                if (count < WARMUP_COUNT) {
                    LOGGER.info("warming up... - message count: {}", count);
                    return;
                }

                LOGGER.info(" {} is reachable. Latency: {}", host, formatNanos(latency));
                hdrRecorder.recordValue(latency);
                if (count % TEST_SIZE == 0) {
                    HISTOGRAM.add(hdrRecorder.getIntervalHistogram());
                    LinkedHashMap<String, String> latencyReport = LatencyTools.createLatencyReport(HISTOGRAM);
                    saveHistogramToFile(HISTOGRAM, System.nanoTime(), histogramLogFile);
                    LOGGER.info("Percentiles for host: {}\n {}\n", host, LatencyTools.toJSON(latencyReport));
                }
            } catch (IOException e) {
                LOGGER.error("{} is not reachable.", host);
                throw new RuntimeException(e);
            }
        }
    }

    private static class PingWithHost extends PingTask {
        public PingWithHost(String host) throws IOException {
            super(host);
        }
        @Override
        long measureRTT() throws IOException {
            InetAddress address = InetAddress.getByName(host);
            long startTime = System.nanoTime();
            if(address.isReachable(5000)){
                long endTime = System.nanoTime();
                return endTime - startTime;
            } else {
                throw new IOException("address is not reachable");
            }
        }
    }

    private static class PingWithHostAndPort extends PingTask {
        private final Integer port;

        public PingWithHostAndPort(String host, Integer port) throws IOException {
            super(host);
            this.port = port;
        }

        @Override
        long measureRTT() throws IOException {
            InetSocketAddress socketAddress = new InetSocketAddress(host, port);
            Socket socket = new Socket();
            long startTime = System.nanoTime();
            socket.connect(socketAddress, 5000); // Timeout after 5 seconds
            long endTime = System.nanoTime();
            socket.close();
            return endTime - startTime;
        }
    }
}
