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
    public static final List<String> COIN_PAIRS;
    public static final Integer API_TOKEN;
    public static final String HOST;
    public static final int HTTP_PORT;

    public static final int WEBSOCKET_PORT;
    public static final int TEST_SIZE;
    public static final long WARMUP_COUNT;
    public static final boolean USE_IOURING;
    public static final int EXCHANGE_CLIENT_COUNT;
    public static final boolean USE_SSL;
    public static final String KEY_STORE_PASSWORD;
    public static final String KEY_STORE_PATH;

    public static final String CIPHERS;

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
