//
// Created by Karaoglu, Huseyin Sercan on 05/03/2024.
//
#include <iostream>
#include <sstream>
#include "ExchangeClient.h"

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

ExchangeClient::ExchangeClient(int apiToken, const std::string& uri)
        : m_apiToken(apiToken), m_uri(uri), m_handler(apiToken, uri) {
    if (Config::USE_SSL) {
        logger("Using SSL");
        m_client.set_tls_init_handler(bind(&ExchangeClient::tls_init_handler, this));
    }
    m_client.init_asio();
    m_client.set_open_handler(bind(&ExchangeClientLatencyTestHandler::on_open, &m_handler, &m_client, ::_1));
    m_client.set_close_handler(bind(&ExchangeClientLatencyTestHandler::on_close, &m_handler, &m_client, ::_1));
    m_client.set_message_handler(bind(&ExchangeClientLatencyTestHandler::on_message, &m_handler, &m_client, ::_1, ::_2));
    m_client.get_alog().clear_channels(websocketpp::log::alevel::frame_header |
                                       websocketpp::log::alevel::frame_payload |
                                       websocketpp::log::alevel::control);
    websocketpp::lib::error_code ec;
    client::connection_ptr con = m_client.get_connection(m_uri, ec);
    if (ec) {
        logger("Failed to create connection: " + ec.message());
        return;
    }

    m_client.connect(con);
    m_client.run();
}

ExchangeClient::~ExchangeClient() {
    m_client.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}
std::shared_ptr<boost::asio::ssl::context> ExchangeClient::tls_init_handler() {
    std::shared_ptr<boost::asio::ssl::context> ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    try {
        boost::system::error_code ec;
        ctx->set_options(boost::asio::ssl::context::default_workarounds, ec);
        if (ec) {
            std::cout << "Error setting SSL options: " << ec.message() << std::endl;
        }
        std::cout << "Setting cipher list: " << Config::CIPHERS << std::endl;
        if (SSL_CTX_set_cipher_list(ctx->native_handle() , Config::CIPHERS.c_str()) != 1) {
            std::cout << "Error setting cipher list" << std::endl;
        }
    } catch (std::exception &e) {
        std::cout << "Error in context pointer: " << e.what() << std::endl;
    }
    return ctx;
}

void ExchangeClient::addBalances(const std::string& qt) {
    try {
        std::ostringstream endpoint;
        endpoint << m_uri.substr(0, m_uri.find("://") + 3) << m_uri.substr(m_uri.find("://") + 3)
                 << "/private/account/user/balances/" << m_apiToken << "/" << qt << "/" << 100000000;

        // Create HTTP request
        // ...

        // Send request and handle response
        // ...
    } catch (const std::exception& e) {
        logger("Error: " + std::string(e.what()));
    }
}

void ExchangeClient::connect() {
    logger("ExchangeClient is connecting via websocket to " + m_uri);
    // No need to explicitly connect, as the connection is established in the constructor
}

void ExchangeClient::close() {
    logger("WebSocket Client sending close");
    m_client.close(m_handler.get_hdl(), websocketpp::close::status::going_away, "");
}

void ExchangeClient::disconnect() {
    logger("Disconnecting...");
    if (m_client.get_con_from_hdl(m_handler.get_hdl())->get_state() == websocketpp::session::state::open) {
        m_client.close(m_handler.get_hdl(), websocketpp::close::status::normal, "");
    }
}