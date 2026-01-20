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

// ExchangeProtocolImpl.cpp

#include "ExchangeProtocol.h"
#include <websocketpp/config/asio_client.hpp>
#include <openssl/sha.h>
#include <cstring>
#include <sstream>
#include <iomanip>

using namespace std;

const std::string ExchangeProtocol::AUTH_MSG_HEADER = "{\"type\":\"AUTHENTICATE\",\"api_token\":\"";
const std::string ExchangeProtocol::HEADER = "{\"type\":\"CREATE_ORDER\",\"order\":{\"instrument_code\":\"";
const std::string ExchangeProtocol::SYMBOL_END = "\",\"client_id\":\"";
const std::string ExchangeProtocol::CLIENT_ID_END = "\",\"side\":\"";
const std::string ExchangeProtocol::BUY_SIDE = "BUY";
const std::string ExchangeProtocol::SELL_SIDE = "SELL";
const std::string ExchangeProtocol::SIDE_END = "\",\"type\":\"";
const std::string ExchangeProtocol::DUMMY_TYPE = "LIMIT";
const std::string ExchangeProtocol::TYPE_END = "\",\"price\":\"";
const std::string ExchangeProtocol::DUMMY_BUY_PRICE = "1";
const std::string ExchangeProtocol::DUMMY_SELL_PRICE = "2";
const std::string ExchangeProtocol::PRICE_END = "\",\"amount\":\"";
const std::string ExchangeProtocol::DUMMY_AMOUNT = "1";
const std::string ExchangeProtocol::AMOUNT_END = "\",\"time_in_force\":\"";
const std::string ExchangeProtocol::DUMMY_TIME_IN_FORCE = "GOOD_TILL_CANCELLED";
const std::string ExchangeProtocol::TIME_IN_FORCE_END = "\"}}";
const std::string ExchangeProtocol::CANCEL_ORDER_HEADER = "{\"type\":\"CANCEL_ORDER\",\"client_id\":\"";
const std::string ExchangeProtocol::CANCEL_ORDER_CLIENT_ID_END = "\",\"instrument_code\":\"";
const std::string ExchangeProtocol::MSG_END = "\"}";
const std::string ExchangeProtocol::SUBSCRIBE_MSG = "{\"type\":\"SUBSCRIBE\",\"channels\":[{\"name\":\"ORDERS\"}]}";
const size_t MAX_ORDER_SIZE = 256;
const size_t MAX_CANCEL_ORDER_SIZE = 128;

string ExchangeProtocol::createBuyOrder(string_view pair, string_view clientId) {
    char buffer[MAX_ORDER_SIZE];

    auto begin = buffer;
    auto end = begin;

    auto write = [&](std::string_view str) {
        end = std::copy(str.begin(), str.end(), end);
    };

    write(HEADER);
    write(pair);
    write(SYMBOL_END);
    write(clientId);
    write(CLIENT_ID_END);
    write(BUY_SIDE);
    write(SIDE_END);
    write(DUMMY_TYPE);
    write(TYPE_END);
    write(DUMMY_BUY_PRICE);
    write(PRICE_END);
    write(DUMMY_AMOUNT);
    write(AMOUNT_END);
    write(DUMMY_TIME_IN_FORCE);
    write(TIME_IN_FORCE_END);

    return {begin, end};
}

string ExchangeProtocol::createSellOrder(const string& pair, const string& clientId) {
    char buffer[MAX_ORDER_SIZE];

    auto begin = buffer;
    auto end = begin;

    auto write = [&](std::string_view str) {
        end = std::copy(str.begin(), str.end(), end);
    };
    write(HEADER);
    write(pair);
    write(SYMBOL_END);
    write(clientId);
    write(CLIENT_ID_END);
    write(SELL_SIDE);
    write(SIDE_END);
    write(DUMMY_TYPE);
    write(TYPE_END);
    write(DUMMY_SELL_PRICE);
    write(PRICE_END);
    write(DUMMY_AMOUNT);
    write(AMOUNT_END);
    write(DUMMY_TIME_IN_FORCE);
    write(TIME_IN_FORCE_END);

    return {begin, end};
}

string ExchangeProtocol::createOrder(const string& pair, const string& type, const string& uuid, const string& side,
                   const string& price, const string& qty) {
    char buffer[MAX_ORDER_SIZE];

    auto begin = buffer;
    auto end = begin;

    auto write = [&](std::string_view str) {
        end = std::copy(str.begin(), str.end(), end);
    };

    write(HEADER);
    write(pair);
    write(SYMBOL_END);
    write(uuid);
    write(CLIENT_ID_END);
    write(side);
    write(SIDE_END);
    write(type);
    write(TYPE_END);
    write(price);
    write(PRICE_END);
    write(qty);
    write(AMOUNT_END);
    write(DUMMY_TIME_IN_FORCE);
    write(TIME_IN_FORCE_END);

    return {begin, end};
}

string ExchangeProtocol::createCancelOrder(string_view pair, string_view clientId) {
    char buffer[MAX_CANCEL_ORDER_SIZE];

    auto begin = buffer;
    auto end = begin;

    auto write = [&](std::string_view str) {
        end = std::copy(str.begin(), str.end(), end);
    };

    write(CANCEL_ORDER_HEADER);
    write(clientId);
    write(CANCEL_ORDER_CLIENT_ID_END);
    write(pair);
    write(MSG_END);

    return {begin, end};
}