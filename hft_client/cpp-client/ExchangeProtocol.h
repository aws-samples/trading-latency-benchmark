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

#ifndef HFT_CLIENT_EXCHANGEPROTOCOL_H
#define HFT_CLIENT_EXCHANGEPROTOCOL_H
#include <string>
#include <vector>
#include <random>
using namespace std;

class ExchangeProtocol {
public:
    static const std::string AUTH_MSG_HEADER;
    static const std::string HEADER;
    static const std::string SYMBOL_END;
    static const std::string CLIENT_ID_END;
    static const std::string BUY_SIDE;
    static const std::string SELL_SIDE;
    static const std::string SIDE_END;
    static const std::string DUMMY_TYPE;
    static const std::string TYPE_END;
    static const std::string DUMMY_BUY_PRICE;
    static const std::string DUMMY_SELL_PRICE;
    static const std::string PRICE_END;
    static const std::string DUMMY_AMOUNT;
    static const std::string AMOUNT_END;
    static const std::string DUMMY_TIME_IN_FORCE;
    static const std::string TIME_IN_FORCE_END;
    static const std::string CANCEL_ORDER_HEADER;
    static const std::string CANCEL_ORDER_CLIENT_ID_END;
    static const std::string MSG_END;
    static const std::string SUBSCRIBE_MSG;

    string createSellOrder(const string& pair, const string& clientId);
    string createBuyOrder(string_view pair, string_view clientId);
    string createOrder(const string& pair, const string& type, const string& uuid, const string& side,
                                         const string& price, const string& qty);
    string createCancelOrder(string_view pair, string_view clientId);
};


#endif //HFT_CLIENT_EXCHANGEPROTOCOL_H
