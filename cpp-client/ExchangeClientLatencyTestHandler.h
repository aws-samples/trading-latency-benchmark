#ifndef HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H
#define HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H

#include <string>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include "ExchangeProtocol.h"

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

class ExchangeClientLatencyTestHandler {
public:
    ExchangeClientLatencyTestHandler(int apiToken, const std::string& uri);

    void on_open(client* c, websocketpp::connection_hdl hdl);
    void on_close(client* c, websocketpp::connection_hdl hdl);
    void on_message(client* c, websocketpp::connection_hdl hdl, client::message_ptr msg);
    void sendCancelOrder(client* c, string_view clientId, string_view pair);
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
    void hdrPrint();
    void sendOrder(client* c, websocketpp::connection_hdl hdl);
    bool calculateRoundTrip(chrono::steady_clock::time_point eventReceiveTime,
                            string_view clientId,
                            unordered_map<string, chrono::steady_clock::time_point>& sentTimeMap);
};

#endif //HFT_CLIENT_EXCHANGECLIENTLATENCYTESTHANDLER_H