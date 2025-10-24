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

#ifndef HFT_CLIENT_EXCHANGECLIENT_H
#define HFT_CLIENT_EXCHANGECLIENT_H
#include "ExchangeClientLatencyTestHandler.h"
#include "Config.h"
#include "Logger.h"

// Include WebSocket++ and OpenSSL headers
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <openssl/ssl.h>

class ExchangeClient {

public:

    ExchangeClient(int apiToken, const std::string& uri);
    ~ExchangeClient();
    void addBalances(const std::string& qt);
    void connect();
    void close();
    void disconnect();


private:
    using ssl_client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using non_ssl_client = websocketpp::client<websocketpp::config::asio_client>;
    int m_apiToken;
    std::string m_uri;
    ExchangeClientLatencyTestHandler m_handler;
    std::unique_ptr<ExchangeClientLatencyTestHandler::ssl_client> m_ssl_client;
    std::unique_ptr<ExchangeClientLatencyTestHandler::non_ssl_client> m_non_ssl_client;


    std::thread m_thread;
    // Other private members and methods, if needed

    std::shared_ptr<asio::ssl::context> tls_init_handler();
};


#endif //HFT_CLIENT_EXCHANGECLIENT_H
