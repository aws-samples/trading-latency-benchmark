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
 
#ifndef HFT_CLIENT_CONFIG_H
#define HFT_CLIENT_CONFIG_H

#include <string>
#include <vector>

class Config {
public:
    static const std::vector<std::string> COIN_PAIRS;
    static const int API_TOKEN;
    static const std::string HOST;
    static const int HTTP_PORT;
    static const int WEBSOCKET_PORT;
    static const int TEST_SIZE;
    static const long WARMUP_COUNT;
    static const bool USE_IOURING;
    static const int EXCHANGE_CLIENT_COUNT;
    static const bool USE_SSL;
    static const std::string KEY_STORE_PASSWORD;
    static const std::string KEY_STORE_PATH;
    static const std::string CIPHERS;

private:
    Config();
    static std::string getProperty(const std::string& key, const std::string& defaultValue);
    static std::vector<std::string> getListProperty(const std::string& key, const std::string& defaultValue);
    static int getIntegerProperty(const std::string& key, const std::string& defaultValue);
    static long getLongProperty(const std::string& key, const std::string& defaultValue);
    static bool getBooleanProperty(const std::string& key, const std::string& defaultValue);
};

#endif // HFT_CLIENT_CONFIG_H