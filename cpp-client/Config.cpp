#include "Config.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

const std::vector<std::string> Config::COIN_PAIRS = Config::getListProperty("COINPAIRS", "BTC_EUR");
const int Config::API_TOKEN = Config::getIntegerProperty("API_TOKEN", "3002");
const std::string Config::HOST = Config::getProperty("HOST", "localhost");
const int Config::HTTP_PORT = Config::getIntegerProperty("HTTP_PORT", "8888");
const int Config::WEBSOCKET_PORT = Config::getIntegerProperty("WEBSOCKET_PORT", "8888");
const int Config::TEST_SIZE = Config::getIntegerProperty("TEST_SIZE", "10000");
const long Config::WARMUP_COUNT = Config::getLongProperty("WARMUP_COUNT", "5");
const bool Config::USE_IOURING = Config::getBooleanProperty("USE_IOURING", "false");
const int Config::EXCHANGE_CLIENT_COUNT = Config::getIntegerProperty("EXCHANGE_CLIENT_COUNT", "16");
const bool Config::USE_SSL = Config::getBooleanProperty("USE_SSL", "true");
const std::string Config::KEY_STORE_PASSWORD = Config::getProperty("KEY_STORE_PASSWORD", "123456");
const std::string Config::KEY_STORE_PATH = Config::getProperty("KEY_STORE_PATH", "keystore.p12");
const std::string Config::CIPHERS = Config::getProperty("CIPHERS", "AES256-GCM-SHA384");

Config::Config() {}

std::string Config::getProperty(const std::string& key, const std::string& defaultValue) {
    std::string value;
    std::ifstream file("config.properties");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string configKey;
            if (std::getline(iss, configKey, '=')) {
                if (configKey == key) {
                    std::getline(iss, value);
                    break;
                }
            }
        }
        file.close();
    }
    if (value.empty()) {
        std::cout << key << " config doesn't exist, defaulting to " << defaultValue << std::endl;
        value = defaultValue;
    }
    std::cout << "Found property in the config " << key << ":" << value << std::endl;
    return value;
}

std::vector<std::string> Config::getListProperty(const std::string& key, const std::string& defaultValue) {
    std::vector<std::string> values;
    std::istringstream iss(getProperty(key, defaultValue));
    std::string token;
    while (std::getline(iss, token, ',')) {
        values.push_back(token);
    }
    return values;
}

int Config::getIntegerProperty(const std::string& key, const std::string& defaultValue) {
    return std::stoi(getProperty(key, defaultValue));
}

long Config::getLongProperty(const std::string& key, const std::string& defaultValue) {
    return std::stol(getProperty(key, defaultValue));
}

bool Config::getBooleanProperty(const std::string& key, const std::string& defaultValue) {
    std::string value = getProperty(key, defaultValue);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value == "true";
}