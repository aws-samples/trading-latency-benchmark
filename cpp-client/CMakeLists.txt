cmake_minimum_required(VERSION 3.27)
project(hft_client)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent
include(FetchContent)

# Find Boost
find_package(Boost 1.74.0 REQUIRED COMPONENTS system)

# Fetch WebSocket++
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2
)
FetchContent_MakeAvailable(websocketpp)

# Fetch nlohmann_json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Fetch HdrHistogram_c
FetchContent_Declare(
    hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG 0.11.8
)
FetchContent_MakeAvailable(hdr_histogram)

# Find OpenSSL (system dependency)
find_package(OpenSSL REQUIRED)

# Add executable
add_executable(${PROJECT_NAME}
    main.cpp
    ExchangeProtocol.h
    ExchangeProtocol.cpp
    ExchangeClient.cpp
    ExchangeClient.h
    ExchangeClientLatencyTestHandler.cpp
    ExchangeClientLatencyTestHandler.h
    Config.h
    Config.cpp
    Logger.h
)

# Set include directories for the target
target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${websocketpp_SOURCE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${hdr_histogram_SOURCE_DIR}/src
    ${hdr_histogram_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    hdr_histogram_static
    nlohmann_json::nlohmann_json
    Boost::system
)

# Additional compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
