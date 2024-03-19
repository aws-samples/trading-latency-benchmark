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

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.net.URL;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;
import java.util.stream.Collectors;

public class Config {
    private static final Logger LOGGER = LogManager.getLogger(Config.class);
    private static final Properties cfg = new Properties();
    /**
     * A list of cryptocurrency trading pairs to be used in the exchange latency test application.
     */
    public static final List<String> COIN_PAIRS;

    /**
     * The API token required for authentication with the exchange.
     */
    public static final Integer API_TOKEN;

    /**
     * The hostname or IP address of the exchange to connect to.
     */
    public static final String HOST;

    /**
     * The HTTP port number to be used by exchange.
     */
    public static final int HTTP_PORT;

    /**
     * The WebSocket port number to be used for real-time communication with the exchange.
     */
    public static final int WEBSOCKET_PORT;

    /**
     * The size of the test data set to be used for performance evaluation.
     */
    public static final int TEST_SIZE;

    /**
     * The number of iterations to be performed as a warm-up before benchmarking.
     */
    public static final long WARMUP_COUNT;

    /**
     * A flag indicating whether to use the io_uring (I/O acceleration) feature on Linux systems.
     */
    public static final boolean USE_IOURING;

    /**
     * The number of exchange client instances to be created for parallel load testing.
     */
    public static final int EXCHANGE_CLIENT_COUNT;

    /**
     * A flag indicating whether to use SSL/TLS encryption for network communication.
     */
    public static final boolean USE_SSL;

    /**
     * The password for the Java keystore containing SSL/TLS certificates.
     */
    public static final String KEY_STORE_PASSWORD;

    /**
     * The path to the Java keystore file containing SSL/TLS certificates.
     */
    public static final String KEY_STORE_PATH;

    /**
     * A comma-separated list of cipher suites to be used for SSL/TLS encryption.
     */
    public static final String CIPHERS;

    /**
     * The interval (in milliseconds) between consecutive ping requests to the endpoint. This is used to measure endpoint latencies.
     */
    public static final long PING_INTERVAL;


    static {
        URL resource = Config.class.getClassLoader().getResource("config.properties");
        assert resource != null;
        try (final FileReader fr = new FileReader(resource.getFile())) {
            cfg.load(fr);
        } catch (IOException e) {
            var f = new File("config.properties");
            try (final FileReader fr = new FileReader(f)){
                cfg.load(fr);
            } catch (IOException ioException) {
                System.out.println("CONFIG ERROR");
                System.exit(101);
            }

        }
        COIN_PAIRS = getListProperty("COINPAIRS", "BTC_USDT");
        HOST = getProperty("HOST", "localhost");
        HTTP_PORT = getIntegerProperty("HTTP_PORT", "8888");
        WEBSOCKET_PORT = getIntegerProperty("WEBSOCKET_PORT", "8888");
        API_TOKEN = getIntegerProperty("API_TOKEN", "3002");
        TEST_SIZE = getIntegerProperty("TEST_SIZE", "1000");
        USE_IOURING = getBooleanProperty("USE_IOURING", "false");
        EXCHANGE_CLIENT_COUNT = getIntegerProperty("EXCHANGE_CLIENT_COUNT", "16");
        WARMUP_COUNT = getLongProperty("WARMUP_COUNT", "5");
        USE_SSL = getBooleanProperty("USE_SSL", "false");
        KEY_STORE_PATH = getProperty("KEY_STORE_PATH", "keystore.p12");
        KEY_STORE_PASSWORD = getProperty("KEY_STORE_PASSWORD", "123456");
        CIPHERS = getProperty("CIPHERS", "AES256-GCM-SHA384");
        PING_INTERVAL = getLongProperty("PING_INTERVAL", "1000" ); // 1 minute in milliseconds

    }


    private static String getProperty(String key, String defaultValue) {
        if(!cfg.containsKey(key)){
            LOGGER.info("{} config doesn't exist, defaulting to {}", key, defaultValue);
        }
        String property = cfg.getProperty(key, defaultValue);
        LOGGER.info("Found property in the config {}:{}", key, property);
        return property;
    }

    private static List<String> getListProperty(String key, String defaultValue){
        return Arrays.stream(getProperty(key,defaultValue).split(",")).collect(Collectors.toList());
    }

    private static int getIntegerProperty(String key, String defaultValue){
        return Integer.parseInt(getProperty(key,defaultValue));
    }

    private static long getLongProperty(String key, String defaultValue){
        return Long.parseLong(getProperty(key,defaultValue));
    }

    private static boolean getBooleanProperty(String key, String defaultValue){
        return Boolean.parseBoolean(getProperty(key, defaultValue));
    }
}
