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
#include "ExchangeProtocol.h"
#include "ExchangeClientLatencyTestHandler.h"
#include "Logger.h"
#include "Config.h"
#include <string_view>
#include <memory>
#include <nlohmann/json.hpp>
#include <iostream>
#include <unordered_map>
#include <openssl/rand.h>
//#include <hdr_histogram.h>
#include "hdr/hdr_histogram.h"
#include "hdr/hdr_histogram_log.h"
#include <sys/stat.h>
#include <algorithm>
using namespace std;
using json = nlohmann::json;

ExchangeClientLatencyTestHandler::ExchangeClientLatencyTestHandler(int apiToken, const std::string& uri)
        : m_apiToken(apiToken),
        m_uri(uri),
        testSize(Config::TEST_SIZE),
        orderSentTimeMap(Config::TEST_SIZE),
        cancelSentTimeMap(Config::TEST_SIZE)
        {
    this->histogram = new hdr_histogram();
    hdr_init(
            1,  // Minimum value
            INT64_C(3600000000000),  // Maximum value (1 hour in nanoseconds)
            3,  // Number of significant figures
            &histogram);  // Pointer to initialise
}

ExchangeClientLatencyTestHandler::~ExchangeClientLatencyTestHandler() {
    if (histogram) {
        hdr_close(histogram);
    }
}

string ExchangeClientLatencyTestHandler::authMessage() {
    // Replace with your authentication message implementation
    return ExchangeProtocol::AUTH_MSG_HEADER + std::to_string(m_apiToken) + ExchangeProtocol::MSG_END;
}


std::string uuid_v4_gen()
{
    union
    {
        struct
        {
            uint32_t time_low;
            uint16_t time_mid;
            uint16_t time_hi_and_version;
            uint8_t  clk_seq_hi_res;
            uint8_t  clk_seq_low;
            uint8_t  node[6];
        };
        uint8_t __rnd[16];
    } uuid;

    RAND_bytes(uuid.__rnd, sizeof(uuid));
    // Refer Section 4.2 of RFC-4122
    // https://tools.ietf.org/html/rfc4122#section-4.2
    uuid.clk_seq_hi_res = (uint8_t) ((uuid.clk_seq_hi_res & 0x3F) | 0x80);
    uuid.time_hi_and_version = (uint16_t) ((uuid.time_hi_and_version & 0x0FFF) | 0x4000);

    char buffer[37];
    snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid.time_low, uuid.time_mid, uuid.time_hi_and_version,
             uuid.clk_seq_hi_res, uuid.clk_seq_low,
             uuid.node[0], uuid.node[1], uuid.node[2],
             uuid.node[3], uuid.node[4], uuid.node[5]);

    return std::string(buffer);
}

bool ExchangeClientLatencyTestHandler::calculateRoundTrip(chrono::steady_clock::time_point eventReceiveTime, string_view clientId, unordered_map<string, chrono::steady_clock::time_point>& sentTimeMap) {
    auto it = sentTimeMap.find(string(clientId));
    if (it == sentTimeMap.end() || eventReceiveTime < it->second) {
        logger("no order sent time found for order " + std::string(clientId));
        return true;
    }
    auto roundTripTime = eventReceiveTime - it->second;
    if (roundTripTime.count() > 0) {
        long long roundTripTimeInNanoseconds = chrono::duration_cast<chrono::nanoseconds>(roundTripTime).count();
        hdr_record_value(
                histogram,  // Histogram to record to
                roundTripTimeInNanoseconds);
    }
    sentTimeMap.erase(it);
    return false;
}


void ExchangeClientLatencyTestHandler::hdrPrint(){
    // Calculate and store the percentile values
    int64_t value50 = hdr_value_at_percentile(histogram, 50.0);
    int64_t value90 = hdr_value_at_percentile(histogram, 90.0);
    int64_t value95 = hdr_value_at_percentile(histogram, 95.0);
    int64_t value99 = hdr_value_at_percentile(histogram, 99.0);
    int64_t value99_9 = hdr_value_at_percentile(histogram, 99.9);
    int64_t value99_99 = hdr_value_at_percentile(histogram, 99.99);
    int64_t valueW = hdr_max(histogram);
    std::stringstream ss;
    ss << "Percentiles: {" << std::endl;
    ss << "        \"50.0%\":\"" << value50 << "ns\"," << std::endl;
    ss << "        \"90.0%\":\"" << value90 << "ns\"," << std::endl;
    ss << "        \"95.0%\":\"" << value95 << "ns\"," << std::endl;
    ss << "        \"99.0%\":\"" << value99 << "ns\"," << std::endl;
    ss << "        \"99.9%\":\"" << value99_9 << "ns\"," << std::endl;
    ss << "        \"99.99%\":\"" << value99_99 << "ns\"," << std::endl;
    ss << "        \"W\":\"" << valueW << "ns\"" << std::endl;
    ss << "}";
    std::cout << ss.str() << std::endl;

    // Save histogram to .hlog file in host-named directory
    saveHistogramToFile();
    
    // Reset histogram for next interval (like Java's hdr.reset())
    hdr_reset(histogram);
}
void ExchangeClientLatencyTestHandler::saveHistogramToFile() {
    // Create host-named directory (dots replaced with underscores)
    std::string folder = Config::HOST;
    std::replace(folder.begin(), folder.end(), '.', '_');
    mkdir(folder.c_str(), 0755);

    std::string path = folder + "/histogram_cpp.hlog";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) {
        logger("Failed to open histogram log file: " + path);
        return;
    }

    struct hdr_log_writer writer;
    hdr_log_writer_init(&writer);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    hdr_log_write_header(&writer, f, "C++ HFT Client 0.0.1", &now);
    hdr_log_write(&writer, f, &now, &now, histogram);
    fclose(f);

    logger("Histogram saved to " + path);
}

websocketpp::connection_hdl ExchangeClientLatencyTestHandler::get_hdl() const{
    return this->hdl;
}

template<typename T>
void ExchangeClientLatencyTestHandler::sendOrder(T* c, websocketpp::connection_hdl m_hdl) {
//    static random_device rd;
//    static mt19937 gen(rd());
//    static uniform_int_distribution<> dis(0, Config::COIN_PAIRS.size() - 1);
//    string pair = Config::COIN_PAIRS[dis(gen)];
    string_view pair = Config::COIN_PAIRS[0];
    auto clientId = uuid_v4_gen();
    string order = protocol->createBuyOrder(pair, clientId);
    // std::cout << order << std::endl;
    websocketpp::lib::error_code ec;
    c->send(m_hdl, order,websocketpp::frame::opcode::text, ec);
    auto time = chrono::steady_clock::now();
    orderSentTimeMap.insert(std::make_pair(string(clientId), time));
    orderResponseCount += 1;
}

template<typename T>
void ExchangeClientLatencyTestHandler::sendCancelOrder(T* c, websocketpp::connection_hdl m_hdl, string_view clientId, string_view pair) {
    string cancelOrder = protocol->createCancelOrder(pair, clientId);
    // std::cout << cancelOrder << std::endl;
    try {
        websocketpp::lib::error_code ec;
        c->send(m_hdl, cancelOrder, websocketpp::frame::opcode::text, ec);
        // std::cout << "cancel is sent" << std::endl;
    } catch (const exception& e) {
        logger(e.what());
    }
    auto cancelSentTime = chrono::steady_clock::now();
    //LOGGER.info("cancel sent time for clientId: {} - {}",clientId, cancelSentTime);
    cancelSentTimeMap.insert(std::make_pair(string(clientId), cancelSentTime));
    orderResponseCount += 1;
}

template<typename T>
void ExchangeClientLatencyTestHandler::on_message(T* c, websocketpp::connection_hdl m_hdl, typename T::message_ptr msg) {
    auto eventReceiveTime = chrono::steady_clock::now();

    auto parsedObject = json::parse(msg->get_payload());
    string_view type = parsedObject["type"].template get<string_view>();


    if (type == "BOOKED" || type == "DONE") {
        // logger("Order ack received");
        string_view clientId = parsedObject["client_id"].template get<string_view>();
        if (type == "BOOKED") {
            if (calculateRoundTrip(eventReceiveTime, clientId, orderSentTimeMap)) return;
            string_view pair = parsedObject["instrument_code"].template get<string_view>();
            sendCancelOrder(c, m_hdl, clientId, pair);
        } else {
            if (calculateRoundTrip(eventReceiveTime, clientId, cancelSentTimeMap)) return;
            sendOrder(c, m_hdl);
        }
        if (orderResponseCount % Config::TEST_SIZE == 0) {
            hdrPrint();

            // Test complete — close connection to exit gracefully
            logger("Test completed. Reached TEST_SIZE: " + std::to_string(Config::TEST_SIZE) + ". Exiting gracefully.");
            websocketpp::lib::error_code ec;
            c->close(m_hdl, websocketpp::close::status::normal, "test complete", ec);
            return;
        }
    } else if (type == "AUTHENTICATED") {
        logger(parsedObject.dump());
        websocketpp::lib::error_code ec;
        c->send(m_hdl, ExchangeProtocol::SUBSCRIBE_MSG,websocketpp::frame::opcode::text, ec);
    } else if (type == "SUBSCRIPTIONS") {
        logger(parsedObject.dump());
        this->testStartTime = chrono::steady_clock::now();
        sendOrder(c, m_hdl);
    } else {
        logger("Unhandled object " + parsedObject.dump());
    }
}

template<typename T>
void ExchangeClientLatencyTestHandler::on_open(T* c, websocketpp::connection_hdl m_hdl) {
    logger("WebSocket client is connected");
    websocketpp::lib::error_code ec;
    c->send(m_hdl, authMessage(), websocketpp::frame::opcode::text, ec);
    if (ec) {
        logger("Error sending message: " + ec.message());
    } else {
        logger("WebSocket client is authenticating for " + std::to_string(m_apiToken));
    }
}

template<typename T>
void ExchangeClientLatencyTestHandler::on_close(T* c, websocketpp::connection_hdl hdl) {
    logger("Connection closed");
}

template void ExchangeClientLatencyTestHandler::sendOrder<ExchangeClientLatencyTestHandler::ssl_client>(ssl_client* c, websocketpp::connection_hdl hdl);
template void ExchangeClientLatencyTestHandler::sendOrder<ExchangeClientLatencyTestHandler::non_ssl_client>(non_ssl_client* c, websocketpp::connection_hdl hdl);
template void ExchangeClientLatencyTestHandler::sendCancelOrder<ExchangeClientLatencyTestHandler::ssl_client>(ssl_client* c, websocketpp::connection_hdl hdl, string_view clientId, string_view pair);
template void ExchangeClientLatencyTestHandler::sendCancelOrder<ExchangeClientLatencyTestHandler::non_ssl_client>(non_ssl_client* c, websocketpp::connection_hdl hdl, string_view clientId, string_view pair);
template void ExchangeClientLatencyTestHandler::on_open<ExchangeClientLatencyTestHandler::ssl_client>(ssl_client* c, websocketpp::connection_hdl hdl);
template void ExchangeClientLatencyTestHandler::on_open<ExchangeClientLatencyTestHandler::non_ssl_client>(non_ssl_client* c, websocketpp::connection_hdl hdl);
template void ExchangeClientLatencyTestHandler::on_message<ExchangeClientLatencyTestHandler::ssl_client>(ssl_client* c, websocketpp::connection_hdl hdl, ssl_client::message_ptr msg);
template void ExchangeClientLatencyTestHandler::on_message<ExchangeClientLatencyTestHandler::non_ssl_client>(non_ssl_client* c, websocketpp::connection_hdl hdl, non_ssl_client::message_ptr msg);
template void ExchangeClientLatencyTestHandler::on_close<ExchangeClientLatencyTestHandler::ssl_client>(ssl_client* c, websocketpp::connection_hdl hdl);
template void ExchangeClientLatencyTestHandler::on_close<ExchangeClientLatencyTestHandler::non_ssl_client>(non_ssl_client* c, websocketpp::connection_hdl hdl);
