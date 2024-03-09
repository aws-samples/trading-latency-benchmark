#include <iostream>
#include "ExchangeClient.h"


int main() {
    int apiToken = 123456;
    std::string uri = "wss://localhost:8888";
    ExchangeClient client(apiToken, uri);
    // Use the client object to interact with the exchange
    return 0;
}
