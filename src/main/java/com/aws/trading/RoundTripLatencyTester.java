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

import io.netty.channel.MultithreadEventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import net.openhft.affinity.AffinityStrategies;
import net.openhft.affinity.AffinityThreadFactory;
import org.HdrHistogram.Histogram;
import org.HdrHistogram.HistogramLogWriter;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.*;
import java.net.URI;
import java.net.URISyntaxException;
import java.security.KeyManagementException;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.UnrecoverableKeyException;
import java.security.cert.CertificateException;
import java.text.MessageFormat;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.LongAdder;

import static com.aws.trading.Config.*;
import static java.util.stream.Collectors.toList;
import static java.util.stream.Collectors.toSet;

public class RoundTripLatencyTester {
    private static final Logger LOGGER = LogManager.getLogger(RoundTripLatencyTester.class);
    private static final int NETTY_THREAD_COUNT = Runtime.getRuntime().availableProcessors() / 2;
    private final MultithreadEventLoopGroup workerGroup;
    private final ExchangeClient[] exchangeClients = new ExchangeClient[EXCHANGE_CLIENT_COUNT];
    private final static int REPORT_SIZE = EXCHANGE_CLIENT_COUNT * (TEST_SIZE / EXCHANGE_CLIENT_COUNT);
    private static final ThreadFactory NETTY_IO_THREAD_FACTORY = new AffinityThreadFactory("netty-io", AffinityStrategies.DIFFERENT_CORE);
    private static final ThreadFactory NETTY_WORKER_THREAD_FACTORY = new AffinityThreadFactory("netty-worker", AffinityStrategies.DIFFERENT_CORE);
    private final MultithreadEventLoopGroup nettyIOGroup;
    public static final Histogram HISTOGRAM = new Histogram(Long.MAX_VALUE, 4);
    public static final LongAdder MESSAGE_COUNTER = new LongAdder();
    private static long testStartTime;
    private static volatile long histogramStartTime;
    private final URI websocketURI;
    private final URI httpURI;


    public RoundTripLatencyTester() throws URISyntaxException, UnrecoverableKeyException, CertificateException, IOException, NoSuchAlgorithmException, KeyStoreException, KeyManagementException {
        this.websocketURI = new URI(MessageFormat.format("{0}://{1}:{2,number,#}", USE_SSL ? "wss" : "ws", HOST, WEBSOCKET_PORT));
        this.httpURI = new URI(MessageFormat.format("{0}://{1}:{2,number,#}", USE_SSL ? "https" : "http", HOST, HTTP_PORT));
        
        // Use NettyTransportFactory to detect and create the best available EventLoopGroup
        NettyTransportFactory.TransportType transport = NettyTransportFactory.detectTransport(USE_IOURING);
        this.nettyIOGroup = createEventLoopGroup(transport, NETTY_THREAD_COUNT, NETTY_IO_THREAD_FACTORY);
        this.workerGroup = createEventLoopGroup(transport, NETTY_THREAD_COUNT, NETTY_WORKER_THREAD_FACTORY);
        LOGGER.info("Using {} EventLoopGroup for networking", transport.getName());
        
        var apiToken1 = API_TOKEN;
        for (int i = 0; i < exchangeClients.length; i++) {
            LOGGER.info("Creating exchang client with api token {}", apiToken1);
            var handler = new ExchangeClientLatencyTestHandler(new ExchangeProtocolImpl(), websocketURI, apiToken1, TEST_SIZE / exchangeClients.length);
            var exchangeClient = new ExchangeClient(apiToken1, handler, nettyIOGroup, workerGroup);
            this.exchangeClients[i] = exchangeClient;
            COIN_PAIRS.stream().map(x ->
                    Arrays.stream(x.split("_"))
                            .collect(toList()))
                    .flatMap(Collection::stream)
                    .collect(toSet()).forEach(qt -> {
                exchangeClient.addBalances(httpURI, qt);
            });
            apiToken1 += 1;
        }
    }

    // 1) cancel orders on ack
    // 2) package send/listen/cancel into threads (thread affiniti optional)
    // 3) capture order-to-ack timestamp difference and write to log (logj4 or chronicle log is ok )
    // 4) script to sweep local logs into s3 (separate deamon)
    public void start() throws InterruptedException {
        for (ExchangeClient exchangeClient : exchangeClients) {
            exchangeClient.connect();
        }
        LOGGER.info("All exchange clients connected successfully");
        testStartTime = System.nanoTime();
        histogramStartTime = testStartTime;
    }

    public void stop() throws InterruptedException {
        LOGGER.info("shutting down netty io group");
        for (ExchangeClient exchangeClient : exchangeClients) {
            exchangeClient.disconnect();
        }
        this.nettyIOGroup.shutdownGracefully().await();
        LOGGER.info("shutting down netty worker group");
        this.workerGroup.shutdownGracefully().await();
    }

    public static synchronized void printResults(SingleWriterRecorder hdr, long orderResponseCount) {
        long currentTime = System.nanoTime();
        var executionTime = currentTime - testStartTime;
        MESSAGE_COUNTER.add(orderResponseCount);

        var messageCount = MESSAGE_COUNTER.longValue();
        if (messageCount < WARMUP_COUNT * TEST_SIZE) {
            LOGGER.info("warming up... - message count: {}", messageCount);
            return;
        }

        HISTOGRAM.add(hdr.getIntervalHistogram());
        if (messageCount % REPORT_SIZE == 0) {
            var executionTimeStr = LatencyTools.formatNanos(executionTime);
            var executionTimeInSeconds = TimeUnit.SECONDS.convert(executionTime, TimeUnit.NANOSECONDS);
            // Prevent division by zero during very fast execution or warmup
            var messagePerSecond = executionTimeInSeconds > 0 
                ? messageCount / executionTimeInSeconds 
                : 0;
            var logMsg = "\nTest Execution Time: {}s \n Number of messages: {} \n Message Per Second: {} \n Percentiles: {} \n";

            try (PrintStream histogramLogFile = getLogFile()) {
                saveHistogramToFile(HISTOGRAM, currentTime, histogramLogFile);
                histogramStartTime = currentTime;
            } catch (IOException e) {
                LOGGER.error(e);
            }

            LinkedHashMap<String, String> latencyReport = LatencyTools.createLatencyReport(HISTOGRAM);
            LOGGER.info(logMsg,
                    executionTimeStr, messageCount, messagePerSecond, LatencyTools.toJSON(latencyReport)
            );

            hdr.reset();
            HISTOGRAM.reset();
        }
    }

    public static void saveHistogramToFile(Histogram histogram, long currentTime, PrintStream log) {
        var histogramLogWriter = new HistogramLogWriter(log);
        histogramLogWriter.outputComment("[Logged with " + "Exchange Client 0.0.1" + "]");
        histogramLogWriter.outputLogFormatVersion();
        histogramLogWriter.outputStartTime(TimeUnit.MILLISECONDS.convert(currentTime, TimeUnit.NANOSECONDS));
        histogramLogWriter.setBaseTime(TimeUnit.MILLISECONDS.convert(histogramStartTime, TimeUnit.NANOSECONDS));
        histogramLogWriter.outputLegend();
        histogramLogWriter.outputIntervalHistogram(histogram);
    }

    public static PrintStream getLogFile() throws IOException {
        return getLogFile("./histogram.hlog");
    }
    public static PrintStream getLogFile(String path) throws IOException {
        return new PrintStream(new FileOutputStream(path, true), false);
    }
    /**
     * Creates an EventLoopGroup based on the detected transport type using reflection.
     * This avoids compile-time dependencies on platform-specific EventLoopGroup classes.
     *
     * @param transport The transport type to use
     * @param threadCount The number of threads for the EventLoopGroup
     * @param threadFactory The thread factory to use
     * @return The created EventLoopGroup
     */
    private static MultithreadEventLoopGroup createEventLoopGroup(
            NettyTransportFactory.TransportType transport,
            int threadCount,
            ThreadFactory threadFactory) {
        try {
            String className;
            switch (transport) {
                case IOURING:
                    className = "io.netty.channel.uring.IOUringEventLoopGroup";
                    break;
                case EPOLL:
                    className = "io.netty.channel.epoll.EpollEventLoopGroup";
                    break;
                case KQUEUE:
                    className = "io.netty.channel.kqueue.KQueueEventLoopGroup";
                    break;
                case NIO:
                default:
                    return new NioEventLoopGroup(threadCount, threadFactory);
            }
            
            // Load the class and create an instance using reflection
            Class<?> eventLoopGroupClass = Class.forName(className);
            return (MultithreadEventLoopGroup) eventLoopGroupClass
                    .getConstructor(int.class, ThreadFactory.class)
                    .newInstance(threadCount, threadFactory);
                    
        } catch (Exception e) {
            LOGGER.warn("Failed to create EventLoopGroup for {}, falling back to NioEventLoopGroup", transport, e);
            return new NioEventLoopGroup(threadCount, threadFactory);
        }
    }
    
    public static void main(String[] args) throws InterruptedException, IOException, URISyntaxException, UnrecoverableKeyException, CertificateException, NoSuchAlgorithmException, KeyStoreException, KeyManagementException {
        RoundTripLatencyTester latencyTester = new RoundTripLatencyTester();
        latencyTester.start();
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                latencyTester.stop();
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }));
        while (true) {
            BufferedReader console = new BufferedReader(new InputStreamReader(System.in));
            String msg = console.readLine();
            if ("exit".equals(msg) || "e".equals(msg)) {
                latencyTester.stop();
                System.exit(0);
            }
        }
    }
}
