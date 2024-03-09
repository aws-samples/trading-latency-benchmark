cmake_minimum_required(VERSION 3.27)
project(hft_client)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(websocketpp REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)


file(GLOB HDR_HISTOGRAM_SOURCES "libs/HdrHistogram_c/src/*.c") 
add_library(HdrHistogram_c ${HDR_HISTOGRAM_SOURCES})
target_include_directories(HdrHistogram_c PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/HdrHistogram_c/include>
        $<INSTALL_INTERFACE:include>)
# Set the include directories
include_directories(
        ${CMAKE_SOURCE_DIR}/libs/HdrHistogram_c/include
        ${CMAKE_SOURCE_DIR}/include
        ${WEBSOCKETPP_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
        ${NHLOHMANN_JSON_INCLUDE_DIR}
)
# Set the source files
set(SOURCE_FILES
        src/main.cpp
        src/ExchangeClient.cpp
        src/ExchangeClientLatencyTestHandler.cpp
)
add_executable(hft_client main.cpp
        ExchangeProtocol.h
        ExchangeProtocol.cpp
        ExchangeClient.cpp
        ExchangeClient.h
        ExchangeClientLatencyTestHandler.cpp
        ExchangeClientLatencyTestHandler.h
        Config.h
        Config.cpp
        Logger.h)
# Link the required libraries

target_link_libraries(${PROJECT_NAME}
        ${WEBSOCKETPP_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        HdrHistogram_c
        nlohmann_json::nlohmann_json
)
# Include the HdrHistogram_c include directory for your target
target_include_directories(hft_client PRIVATE ${CMAKE_SOURCE_DIR}/libs/HdrHistogram_c/include)