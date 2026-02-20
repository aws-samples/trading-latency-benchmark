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
#ifndef HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H
#define HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H

#include <string>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include "ExchangeProtocol.h"


class ExchangeClientLatencyTestHandler {
public:
    using ssl_client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using non_ssl_client = websocketpp::client<websocketpp::config::asio_client>;

    ExchangeClientLatencyTestHandler(int apiToken, const std::string& uri);
    
    template<typename T>
    void on_open(T* c, websocketpp::connection_hdl hdl);
    
    template<typename T>
    void on_message(T* c, websocketpp::connection_hdl hdl, typename T::message_ptr msg);

    template<typename T>
    void on_close(T* c, websocketpp::connection_hdl hdl);
    websocketpp::connection_hdl get_hdl() const;

    virtual ~ExchangeClientLatencyTestHandler();

private:
    int m_apiToken;
    ExchangeProtocol* protocol;
    string m_uri;
    string subscribeMessage();
    string authMessage();
    websocketpp::connection_hdl hdl;
    string uri;
    int testSize;
    unordered_map<string, chrono::steady_clock::time_point> orderSentTimeMap;
    unordered_map<string, chrono::steady_clock::time_point> cancelSentTimeMap;
    uint64_t orderResponseCount = 0;
    chrono::steady_clock::time_point testStartTime;
    random_device rd;
    mt19937 gen;
    struct hdr_histogram* histogram;
    struct timespec histogramStartTimeSpec;
    void hdrPrint();
    void saveHistogramToFile();
    template<typename T>
    void sendOrder(T* c, websocketpp::connection_hdl hdl);
    template<typename T>
    void sendCancelOrder(T* c, websocketpp::connection_hdl hdl, string_view clientId, string_view pair);
    bool calculateRoundTrip(chrono::steady_clock::time_point eventReceiveTime,
                            string_view clientId,
                            unordered_map<string, chrono::steady_clock::time_point>& sentTimeMap);
};

#endif //HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H