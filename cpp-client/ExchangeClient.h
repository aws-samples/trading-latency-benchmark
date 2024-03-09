//
// Created by Karaoglu, Huseyin Sercan on 05/03/2024.
//

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

    int m_apiToken;
    std::string m_uri;
    ExchangeClientLatencyTestHandler m_handler;
    client m_client;
    std::thread m_thread;
    // Other private members and methods, if needed

    std::shared_ptr<boost::asio::ssl::context> tls_init_handler();
};


#endif //HFT_CLIENT_EXCHANGECLIENT_H
