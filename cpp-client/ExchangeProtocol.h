//
// Created by Karaoglu, Huseyin Sercan on 08/03/2024.
//

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
