// Logger.h
#ifndef HFT_CLIENT_LOGGER_H
#define HFT_CLIENT_LOGGER_H

#include <string>
#include <iostream>

inline void logger(const std::string& message) {
    std::cout << message << std::endl;
}

#endif // HFT_CLIENT_LOGGER_H