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
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.net.URL;
import java.util.Arrays;
import java.util.List;
import java.util.Properties;
import java.util.stream.Collectors;

/**
 * Configuration manager for the trading latency benchmark application.
 * Loads and provides access to configuration properties from config.properties file.
 */
public class Config {
    private static final Logger LOGGER = LogManager.getLogger(Config.class);
    private static final Properties cfg = new Properties();
    private static final String CONFIG_FILE_NAME = "config.properties";
    
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
     * The interval (in milliseconds) between consecutive ping requests to the endpoint. 
     * This is used to measure endpoint latencies.
     */
    public static final long PING_INTERVAL;

    /**
     * The number of significant figures to use for histogram precision.
     * Higher values provide finer granularity but use more memory.
     * - 2 sig figs: ~1% resolution (e.g., ~2.25ms buckets @ 225ms)
     * - 3 sig figs: ~0.1% resolution (e.g., ~225μs buckets @ 225ms)
     * - 4 sig figs: ~0.01% resolution (e.g., ~22.5μs buckets @ 225ms)
     * - 5 sig figs: ~0.001% resolution (e.g., ~2.25μs buckets @ 225ms)
     */
    public static final int HISTOGRAM_SIGNIFICANT_FIGURES;

    // Static initializer to load configuration when the class is loaded
    static {
        loadConfiguration();
        
        // Initialize configuration properties with defaults if not found
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
        PING_INTERVAL = getLongProperty("PING_INTERVAL", "1000"); // 1 second in milliseconds
        HISTOGRAM_SIGNIFICANT_FIGURES = getIntegerProperty("HISTOGRAM_SIGNIFICANT_FIGURES", "5");

        validateConfiguration();
    }

    /**
     * Loads the configuration from the config.properties file.
     * Tries to load from classpath first, then from the current directory.
     */
    private static void loadConfiguration() {
        boolean loaded = false;
        
        // Try loading from classpath first
        URL resource = Config.class.getClassLoader().getResource(CONFIG_FILE_NAME);
        if (resource != null) {
            try (FileInputStream fis = new FileInputStream(resource.getFile())) {
                cfg.load(fis);
                LOGGER.info("Configuration loaded from classpath: {}", resource.getFile());
                loaded = true;
            } catch (IOException e) {
                LOGGER.warn("Failed to load configuration from classpath: {}", e.getMessage());
            }
        }
        
        // If not loaded from classpath, try loading from current directory
        if (!loaded) {
            File configFile = new File(CONFIG_FILE_NAME);
            if (configFile.exists()) {
                try (FileInputStream fis = new FileInputStream(configFile)) {
                    cfg.load(fis);
                    LOGGER.info("Configuration loaded from file: {}", configFile.getAbsolutePath());
                    loaded = true;
                } catch (IOException e) {
                    LOGGER.error("Failed to load configuration from file: {}", e.getMessage(), e);
                }
            }
        }
        
        // If still not loaded, log error and exit
        if (!loaded) {
            LOGGER.error("Failed to load configuration from classpath or file system");
            LOGGER.error("Please ensure {} exists in classpath or current directory", CONFIG_FILE_NAME);
            throw new RuntimeException("Configuration file not found: " + CONFIG_FILE_NAME);
        }
    }

    /**
     * Validates the loaded configuration for required properties and valid values.
     */
    private static void validateConfiguration() {
        // Validate required properties
        if (HOST == null || HOST.trim().isEmpty()) {
            LOGGER.error("HOST is required but not specified in configuration");
            throw new IllegalStateException("HOST is required but not specified in configuration");
        }
        
        // Validate port ranges
        if (HTTP_PORT <= 0 || HTTP_PORT > 65535) {
            LOGGER.error("Invalid HTTP_PORT: {}. Must be between 1 and 65535", HTTP_PORT);
            throw new IllegalStateException("Invalid HTTP_PORT: " + HTTP_PORT);
        }
        
        if (WEBSOCKET_PORT <= 0 || WEBSOCKET_PORT > 65535) {
            LOGGER.error("Invalid WEBSOCKET_PORT: {}. Must be between 1 and 65535", WEBSOCKET_PORT);
            throw new IllegalStateException("Invalid WEBSOCKET_PORT: " + WEBSOCKET_PORT);
        }
        
        // Validate SSL configuration if enabled
        if (USE_SSL) {
            if (KEY_STORE_PATH == null || KEY_STORE_PATH.trim().isEmpty()) {
                LOGGER.error("KEY_STORE_PATH is required when USE_SSL is true");
                throw new IllegalStateException("KEY_STORE_PATH is required when USE_SSL is true");
            }
            
            if (KEY_STORE_PASSWORD == null || KEY_STORE_PASSWORD.trim().isEmpty()) {
                LOGGER.error("KEY_STORE_PASSWORD is required when USE_SSL is true");
                throw new IllegalStateException("KEY_STORE_PASSWORD is required when USE_SSL is true");
            }
            
            // Check if keystore file exists
            File keyStoreFile = new File(KEY_STORE_PATH);
            if (!keyStoreFile.exists() || !keyStoreFile.isFile()) {
                LOGGER.warn("Keystore file does not exist: {}. SSL may fail at runtime.", KEY_STORE_PATH);
            }
        }
        
        // Log configuration summary
        LOGGER.info("Configuration loaded successfully:");
        LOGGER.info("  HOST: {}", HOST);
        LOGGER.info("  HTTP_PORT: {}", HTTP_PORT);
        LOGGER.info("  WEBSOCKET_PORT: {}", WEBSOCKET_PORT);
        LOGGER.info("  USE_SSL: {}", USE_SSL);
        LOGGER.info("  USE_IOURING: {}", USE_IOURING);
        LOGGER.info("  TEST_SIZE: {}", TEST_SIZE);
        LOGGER.info("  EXCHANGE_CLIENT_COUNT: {}", EXCHANGE_CLIENT_COUNT);
    }

    /**
     * Gets a property value as a string, with a default value if not found.
     *
     * @param key The property key
     * @param defaultValue The default value to use if the property is not found
     * @return The property value as a string
     */
    private static String getProperty(String key, String defaultValue) {
        if (!cfg.containsKey(key)) {
            LOGGER.info("{} config doesn't exist, defaulting to {}", key, defaultValue);
        }
        String property = cfg.getProperty(key, defaultValue);
        LOGGER.debug("Found property in the config {}:{}", key, property);
        return property;
    }

    /**
     * Gets a property value as a list of strings, with a default value if not found.
     *
     * @param key The property key
     * @param defaultValue The default value to use if the property is not found
     * @return The property value as a list of strings
     */
    private static List<String> getListProperty(String key, String defaultValue) {
        return Arrays.stream(getProperty(key, defaultValue).split(","))
                .map(String::trim)
                .filter(s -> !s.isEmpty())
                .collect(Collectors.toList());
    }

    /**
     * Gets a property value as an integer, with a default value if not found.
     *
     * @param key The property key
     * @param defaultValue The default value to use if the property is not found
     * @return The property value as an integer
     */
    private static int getIntegerProperty(String key, String defaultValue) {
        try {
            return Integer.parseInt(getProperty(key, defaultValue));
        } catch (NumberFormatException e) {
            LOGGER.error("Invalid integer value for {}: {}", key, getProperty(key, defaultValue), e);
            throw new IllegalArgumentException("Invalid integer value for " + key, e);
        }
    }

    /**
     * Gets a property value as a long, with a default value if not found.
     *
     * @param key The property key
     * @param defaultValue The default value to use if the property is not found
     * @return The property value as a long
     */
    private static long getLongProperty(String key, String defaultValue) {
        try {
            return Long.parseLong(getProperty(key, defaultValue));
        } catch (NumberFormatException e) {
            LOGGER.error("Invalid long value for {}: {}", key, getProperty(key, defaultValue), e);
            throw new IllegalArgumentException("Invalid long value for " + key, e);
        }
    }

    /**
     * Gets a property value as a boolean, with a default value if not found.
     *
     * @param key The property key
     * @param defaultValue The default value to use if the property is not found
     * @return The property value as a boolean
     */
    private static boolean getBooleanProperty(String key, String defaultValue) {
        return Boolean.parseBoolean(getProperty(key, defaultValue));
    }
}
