// ExchangeClientLatencyTestHandler.cpp
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
#include "libs/HdrHistogram_c/include/hdr/hdr_histogram.h"
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
            INT64_C(100000),  // Maximum value
            3,  // Number of significant figures
            &histogram);  // Pointer to initialise
}

ExchangeClientLatencyTestHandler::~ExchangeClientLatencyTestHandler() {
    if (histogram) {
        hdr_close(histogram);
    }
}

void ExchangeClientLatencyTestHandler::on_message(client* c, websocketpp::connection_hdl hdl, client::message_ptr msg) {
    auto eventReceiveTime = chrono::steady_clock::now();

    auto parsedObject = json::parse(msg->get_payload());
    string_view type = parsedObject["type"].get<string_view>();

    if (type == "BOOKED" || type == "DONE") {
        //LOGGER.info("eventTime: {}, received ACK: {}",eventReceiveTime, parsedObject);
        string_view clientId = parsedObject["client_id"].get<string_view>();
        if (type == "BOOKED") {
            if (calculateRoundTrip(eventReceiveTime, clientId, orderSentTimeMap)) return;
            string_view pair = parsedObject["instrument_code"].get<string_view>();
            sendCancelOrder(c, clientId, pair);
        } else {
            if (calculateRoundTrip(eventReceiveTime, clientId, cancelSentTimeMap)) return;
            sendOrder(c, hdl);
        }
        if (orderResponseCount % Config::TEST_SIZE == 0) {
            hdrPrint();
        }
    } else if (type == "AUTHENTICATED") {
        logger(parsedObject.dump());
        websocketpp::lib::error_code ec;
        c->send(hdl, ExchangeProtocol::SUBSCRIBE_MSG,websocketpp::frame::opcode::text, ec);
    } else if (type == "SUBSCRIPTIONS") {
        logger(parsedObject.dump());
        this->testStartTime = chrono::steady_clock::now();
        sendOrder(c, hdl);
    } else {
        logger("Unhandled object " + parsedObject.dump());
    }
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

    int rc = RAND_bytes(uuid.__rnd, sizeof(uuid));
    // Refer Section 4.2 of RFC-4122
    // https://tools.ietf.org/html/rfc4122#section-4.2
    uuid.clk_seq_hi_res = (uint8_t) ((uuid.clk_seq_hi_res & 0x3F) | 0x80);
    uuid.time_hi_and_version = (uint16_t) ((uuid.time_hi_and_version & 0x0FFF) | 0x4000);

    char buffer[38];
    snprintf(buffer, 38, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid.time_low, uuid.time_mid, uuid.time_hi_and_version,
             uuid.clk_seq_hi_res, uuid.clk_seq_low,
             uuid.node[0], uuid.node[1], uuid.node[2],
             uuid.node[3], uuid.node[4], uuid.node[5]);

    return std::string(buffer);
}
void ExchangeClientLatencyTestHandler::sendOrder(client* c, websocketpp::connection_hdl hdl) {
//    static random_device rd;
//    static mt19937 gen(rd());
//    static uniform_int_distribution<> dis(0, Config::COIN_PAIRS.size() - 1);
//    string pair = Config::COIN_PAIRS[dis(gen)];
    string_view pair = Config::COIN_PAIRS[0];
    auto clientId = uuid_v4_gen();
    string order = protocol->createBuyOrder(pair, clientId);
    websocketpp::lib::error_code ec;
    c->send(hdl, order,websocketpp::frame::opcode::text, ec);
    auto time = chrono::steady_clock::now();
    orderSentTimeMap.insert(std::make_pair(string(clientId), time));
    orderResponseCount += 1;
}

bool ExchangeClientLatencyTestHandler::calculateRoundTrip(chrono::steady_clock::time_point eventReceiveTime, string_view clientId, unordered_map<string, chrono::steady_clock::time_point>& sentTimeMap) {
    auto it = sentTimeMap.find(string(clientId));
    if (it == sentTimeMap.end() || eventReceiveTime < it->second) {
        logger("no order sent time found for order " + std::string(clientId));
        return true;
    }
    auto roundTripTime = eventReceiveTime - it->second;
    if (roundTripTime.count() > 0) {
        if (orderResponseCount % Config::TEST_SIZE == 0) {
            hdrPrint();
           // logger("RTT:" + std::to_string(roundTripTimeInMicroseconds) + "μs");
        }

        long long roundTripTimeInMicroseconds = chrono::duration_cast<chrono::microseconds>(roundTripTime).count();
        //logger("RTT:" + std::to_string(roundTripTimeInMicroseconds));
        hdr_record_value(
                histogram,  // Histogram to record to
                roundTripTimeInMicroseconds);
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
    ss << "        \"50.0%\":\"" << value50 << "µs\"," << std::endl;
    ss << "        \"90.0%\":\"" << value90 << "µs\"," << std::endl;
    ss << "        \"95.0%\":\"" << value95 << "µs\"," << std::endl;
    ss << "        \"99.0%\":\"" << value99 << "µs\"," << std::endl;
    ss << "        \"99.9%\":\"" << value99_9 << "µs\"," << std::endl;
    ss << "        \"99.99%\":\"" << value99_99 << "µs\"," << std::endl;
    ss << "        \"W\":\"" << valueW << "µs\"" << std::endl;
    ss << "}";
    std::cout << ss.str() << std::endl;
}
websocketpp::connection_hdl ExchangeClientLatencyTestHandler::get_hdl() const{
    return this->hdl;
}

void ExchangeClientLatencyTestHandler::on_open(client* c, websocketpp::connection_hdl hdl) {
    std::cout << "WebSocket client is connected" << std::endl;
    std::cout << "WebSocket client is authenticating for " << m_apiToken << std::endl;
    this->hdl = hdl;
    websocketpp::lib::error_code ec;
    c->send(hdl, authMessage(), websocketpp::frame::opcode::text, ec);
}

void ExchangeClientLatencyTestHandler::on_close(client* c, websocketpp::connection_hdl hdl) {
    std::cout << "connection closed" << std::endl;
}

string ExchangeClientLatencyTestHandler::authMessage() {
    // Replace with your authentication message implementation
    return ExchangeProtocol::AUTH_MSG_HEADER + std::to_string(m_apiToken) + ExchangeProtocol::MSG_END;
}

void ExchangeClientLatencyTestHandler::sendCancelOrder(client* c, string_view clientId, string_view pair) {
    string cancelOrder = protocol->createCancelOrder(pair, clientId);
    // std::cout << cancelOrder << std::endl;
    try {
        websocketpp::lib::error_code ec;
        c->send(hdl, cancelOrder, websocketpp::frame::opcode::text, ec);
    } catch (const exception& e) {
        logger(e.what());
    }
    auto cancelSentTime = chrono::steady_clock::now();
    //LOGGER.info("cancel sent time for clientId: {} - {}",clientId, cancelSentTime);
    cancelSentTimeMap.insert(std::make_pair(string(clientId), cancelSentTime));
    orderResponseCount += 1;
}