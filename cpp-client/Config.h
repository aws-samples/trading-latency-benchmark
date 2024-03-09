#ifndef HFT_CLIENT_CONFIG_H
#define HFT_CLIENT_CONFIG_H

#include <string>
#include <vector>

class Config {
public:
    static const std::vector<std::string> COIN_PAIRS;
    static const int API_TOKEN;
    static const std::string HOST;
    static const int HTTP_PORT;
    static const int WEBSOCKET_PORT;
    static const int TEST_SIZE;
    static const long WARMUP_COUNT;
    static const bool USE_IOURING;
    static const int EXCHANGE_CLIENT_COUNT;
    static const bool USE_SSL;
    static const std::string KEY_STORE_PASSWORD;
    static const std::string KEY_STORE_PATH;
    static const std::string CIPHERS;

private:
    Config();
    static std::string getProperty(const std::string& key, const std::string& defaultValue);
    static std::vector<std::string> getListProperty(const std::string& key, const std::string& defaultValue);
    static int getIntegerProperty(const std::string& key, const std::string& defaultValue);
    static long getLongProperty(const std::string& key, const std::string& defaultValue);
    static bool getBooleanProperty(const std::string& key, const std::string& defaultValue);
};

#endif // HFT_CLIENT_CONFIG_H