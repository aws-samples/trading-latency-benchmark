cmake_minimum_required(VERSION 3.27)
project(hft_client)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set minimum policy version to allow older CMake syntax in dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Define ASIO_STANDALONE globally BEFORE fetching any dependencies
# This ensures WebSocket++ uses standalone ASIO instead of Boost.Asio
add_compile_definitions(ASIO_STANDALONE)

# Force WebSocket++ to use C++11 standard library instead of Boost
# This prevents WebSocket++ from trying to include boost headers
add_compile_definitions(_WEBSOCKETPP_CPP11_RANDOM_DEVICE_)
add_compile_definitions(_WEBSOCKETPP_CPP11_TYPE_TRAITS_)
add_compile_definitions(_WEBSOCKETPP_CPP11_FUNCTIONAL_)
add_compile_definitions(_WEBSOCKETPP_CPP11_SYSTEM_ERROR_)
add_compile_definitions(_WEBSOCKETPP_CPP11_MEMORY_)

# Windows-specific: Fix byte ambiguity between Windows SDK and std::byte
if(WIN32)
  add_compile_definitions(_HAS_STD_BYTE=0)
endif()

# Include FetchContent
include(FetchContent)

# Fetch standalone ASIO (replaces Boost.Asio to avoid version compatibility issues)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

# Fetch WebSocket++ (configured to use standalone ASIO via ASIO_STANDALONE definition)
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

# Add Windows resource file with version information
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE hft_client.rc)
endif()

# Set include directories for the target
target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${websocketpp_SOURCE_DIR}
    ${asio_SOURCE_DIR}/asio/include
    ${OPENSSL_INCLUDE_DIR}
    ${hdr_histogram_SOURCE_DIR}/src
    ${hdr_histogram_SOURCE_DIR}/include
)

# Define ASIO_STANDALONE to use standalone ASIO instead of Boost.Asio
# Force WebSocket++ to use C++11 standard library instead of Boost
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    ASIO_STANDALONE
    _WEBSOCKETPP_CPP11_RANDOM_DEVICE_
    _WEBSOCKETPP_CPP11_TYPE_TRAITS_
    _WEBSOCKETPP_CPP11_FUNCTIONAL_
    _WEBSOCKETPP_CPP11_SYSTEM_ERROR_
    _WEBSOCKETPP_CPP11_MEMORY_
)

# Link libraries (no Boost needed with standalone ASIO)
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    hdr_histogram_static
    nlohmann_json::nlohmann_json
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
