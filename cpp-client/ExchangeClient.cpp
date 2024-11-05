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
        m_ssl_client = std::make_unique<ExchangeClientLatencyTestHandler::ssl_client>();
        m_ssl_client->set_tls_init_handler(std::bind(&ExchangeClient::tls_init_handler, this));
        m_ssl_client->init_asio();
        m_ssl_client->set_open_handler(bind(&ExchangeClientLatencyTestHandler::on_open<ExchangeClientLatencyTestHandler::ssl_client>, &m_handler, m_ssl_client.get(), ::_1));
        m_ssl_client->set_close_handler(bind(&ExchangeClientLatencyTestHandler::on_close<ExchangeClientLatencyTestHandler::ssl_client>, &m_handler, m_ssl_client.get(), ::_1));
        m_ssl_client->set_message_handler(bind(&ExchangeClientLatencyTestHandler::on_message<ExchangeClientLatencyTestHandler::ssl_client>, &m_handler, m_ssl_client.get(), ::_1, ::_2));

        m_ssl_client->get_alog().clear_channels(websocketpp::log::alevel::frame_header |
                                                websocketpp::log::alevel::frame_payload |
                                                websocketpp::log::alevel::control);
    } else {
        logger("Not using SSL");
        m_non_ssl_client = std::make_unique<ExchangeClientLatencyTestHandler::non_ssl_client>();
        m_non_ssl_client->init_asio();
        m_non_ssl_client->set_open_handler(bind(&ExchangeClientLatencyTestHandler::on_open<ExchangeClientLatencyTestHandler::non_ssl_client>, &m_handler, m_non_ssl_client.get(), ::_1));
        m_non_ssl_client->set_close_handler(bind(&ExchangeClientLatencyTestHandler::on_close<ExchangeClientLatencyTestHandler::non_ssl_client>, &m_handler, m_non_ssl_client.get(), ::_1));
        m_non_ssl_client->set_message_handler(bind(&ExchangeClientLatencyTestHandler::on_message<ExchangeClientLatencyTestHandler::non_ssl_client>, &m_handler, m_non_ssl_client.get(), ::_1, ::_2));
    
        m_non_ssl_client->get_alog().clear_channels(websocketpp::log::alevel::frame_header |
                                                    websocketpp::log::alevel::frame_payload |
                                                    websocketpp::log::alevel::control);
    }
}

ExchangeClient::~ExchangeClient() {
    if (Config::USE_SSL) {
        m_ssl_client->stop();
    } else {
        m_non_ssl_client->stop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ExchangeClient::connect() {
    websocketpp::lib::error_code ec;
    std::string uri = (Config::USE_SSL ? "wss://" : "ws://") + m_uri;
    
    if (Config::USE_SSL) {
        auto con = m_ssl_client->get_connection(uri, ec);
        if (ec) {
            logger("Failed to create SSL connection: " + ec.message());
            return;
        }
        m_ssl_client->connect(con);
        m_ssl_client->run();
    } else {
        auto con = m_non_ssl_client->get_connection(uri, ec);
        if (ec) {
            logger("Failed to create non-SSL connection: " + ec.message());
            return;
        }
        m_non_ssl_client->connect(con);
        m_non_ssl_client->run();
    }
}

void ExchangeClient::close() {
    logger("WebSocket Client sending close");
    if (Config::USE_SSL) {
        m_ssl_client->close(m_handler.get_hdl(), websocketpp::close::status::going_away, "");
    } else {
        m_non_ssl_client->close(m_handler.get_hdl(), websocketpp::close::status::going_away, "");
    }
}

void ExchangeClient::disconnect() {
    logger("Disconnecting...");
    if (Config::USE_SSL) {
        if (m_ssl_client->get_con_from_hdl(m_handler.get_hdl())->get_state() == websocketpp::session::state::open) {
            m_ssl_client->close(m_handler.get_hdl(), websocketpp::close::status::normal, "");
        }
    } else {
        if (m_non_ssl_client->get_con_from_hdl(m_handler.get_hdl())->get_state() == websocketpp::session::state::open) {
            m_non_ssl_client->close(m_handler.get_hdl(), websocketpp::close::status::normal, "");
        }
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