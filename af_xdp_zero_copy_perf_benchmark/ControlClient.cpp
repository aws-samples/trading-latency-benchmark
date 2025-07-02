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
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

class ControlClient {
private:
    static constexpr int CONTROL_PORT = 12345;
    static constexpr uint8_t CTRL_ADD_DESTINATION = 1;
    static constexpr uint8_t CTRL_REMOVE_DESTINATION = 2;
    static constexpr uint8_t CTRL_LIST_DESTINATIONS = 3;

    int socket_fd_;
    std::string server_address_;

public:
    ControlClient(const std::string& serverAddress) : socket_fd_(-1), server_address_(serverAddress) {
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }
    }

    ~ControlClient() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
        }
    }

    bool addDestination(const std::string& ipAddress, uint16_t port) {
        std::vector<uint8_t> message(7);
        message[0] = CTRL_ADD_DESTINATION;

        // Convert IP address to network byte order
        struct in_addr addr;
        if (inet_aton(ipAddress.c_str(), &addr) == 0) {
            std::cerr << "Invalid IP address: " << ipAddress << std::endl;
            return false;
        }
        memcpy(&message[1], &addr.s_addr, 4);

        // Convert port to network byte order
        uint16_t port_net = htons(port);
        memcpy(&message[5], &port_net, 2);

        return sendMessage(message);
    }

    bool removeDestination(const std::string& ipAddress, uint16_t port) {
        std::vector<uint8_t> message(7);
        message[0] = CTRL_REMOVE_DESTINATION;

        // Convert IP address to network byte order
        struct in_addr addr;
        if (inet_aton(ipAddress.c_str(), &addr) == 0) {
            std::cerr << "Invalid IP address: " << ipAddress << std::endl;
            return false;
        }
        memcpy(&message[1], &addr.s_addr, 4);

        // Convert port to network byte order
        uint16_t port_net = htons(port);
        memcpy(&message[5], &port_net, 2);

        return sendMessage(message);
    }

    bool listDestinations() {
        std::vector<uint8_t> message(1);
        message[0] = CTRL_LIST_DESTINATIONS;

        if (!sendMessage(message)) {
            return false;
        }

        // Receive response
        std::vector<uint8_t> response(1024);
        struct sockaddr_in server_addr;
        socklen_t addr_len = sizeof(server_addr);

        ssize_t bytes_received = recvfrom(socket_fd_, response.data(), response.size(), 0,
                                         (struct sockaddr*)&server_addr, &addr_len);

        if (bytes_received < 0) {
            std::cerr << "Failed to receive response: " << strerror(errno) << std::endl;
            return false;
        }

        if (bytes_received == 0) {
            std::cout << "No destinations configured" << std::endl;
            return true;
        }

        // Parse response
        uint8_t count = response[0];
        std::cout << "Active destinations (" << static_cast<int>(count) << "):" << std::endl;

        size_t offset = 1;
        for (int i = 0; i < count && offset + 6 <= static_cast<size_t>(bytes_received); i++) {
            uint32_t ip_addr;
            uint16_t port;
            memcpy(&ip_addr, &response[offset], 4);
            memcpy(&port, &response[offset + 4], 2);

            struct in_addr addr;
            addr.s_addr = ip_addr;
            std::string ip_str = inet_ntoa(addr);
            uint16_t port_host = ntohs(port);

            std::cout << "  " << (i + 1) << ". " << ip_str << ":" << port_host << std::endl;
            offset += 6;
        }

        return true;
    }

private:
    bool sendMessage(const std::vector<uint8_t>& message) {
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(CONTROL_PORT);

        if (inet_aton(server_address_.c_str(), &server_addr.sin_addr) == 0) {
            std::cerr << "Invalid server address: " << server_address_ << std::endl;
            return false;
        }

        ssize_t sent = sendto(socket_fd_, message.data(), message.size(), 0,
                             (struct sockaddr*)&server_addr, sizeof(server_addr));

        if (sent < 0) {
            std::cerr << "Failed to send message: " << strerror(errno) << std::endl;
            return false;
        }

        // For add/remove operations, receive a simple response
        if (message[0] == CTRL_ADD_DESTINATION || message[0] == CTRL_REMOVE_DESTINATION) {
            uint8_t response;
            struct sockaddr_in resp_addr;
            socklen_t addr_len = sizeof(resp_addr);

            ssize_t bytes_received = recvfrom(socket_fd_, &response, 1, 0,
                                             (struct sockaddr*)&resp_addr, &addr_len);

            if (bytes_received < 0) {
                std::cerr << "Failed to receive response: " << strerror(errno) << std::endl;
                return false;
            }

            if (response == 1) {
                std::cout << "Operation successful" << std::endl;
                return true;
            } else {
                std::cout << "Operation failed" << std::endl;
                return false;
            }
        }

        return true;
    }
};

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " <server_ip> <command> [args...]" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  add <dest_ip> <dest_port>    - Add a destination" << std::endl;
    std::cout << "  remove <dest_ip> <dest_port> - Remove a destination" << std::endl;
    std::cout << "  list                         - List all destinations" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << progName << " 192.168.1.100 add 10.0.0.5 8080" << std::endl;
    std::cout << "  " << progName << " 192.168.1.100 remove 10.0.0.5 8080" << std::endl;
    std::cout << "  " << progName << " 192.168.1.100 list" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string server_ip = argv[1];
    std::string command = argv[2];

    try {
        ControlClient client(server_ip);

        if (command == "add") {
            if (argc != 5) {
                std::cerr << "Error: 'add' command requires destination IP and port" << std::endl;
                printUsage(argv[0]);
                return 1;
            }

            std::string dest_ip = argv[3];
            uint16_t dest_port = static_cast<uint16_t>(std::stoi(argv[4]));

            std::cout << "Adding destination: " << dest_ip << ":" << dest_port << std::endl;
            if (!client.addDestination(dest_ip, dest_port)) {
                return 1;
            }

        } else if (command == "remove") {
            if (argc != 5) {
                std::cerr << "Error: 'remove' command requires destination IP and port" << std::endl;
                printUsage(argv[0]);
                return 1;
            }

            std::string dest_ip = argv[3];
            uint16_t dest_port = static_cast<uint16_t>(std::stoi(argv[4]));

            std::cout << "Removing destination: " << dest_ip << ":" << dest_port << std::endl;
            if (!client.removeDestination(dest_ip, dest_port)) {
                return 1;
            }

        } else if (command == "list") {
            if (!client.listDestinations()) {
                return 1;
            }

        } else {
            std::cerr << "Error: Unknown command '" << command << "'" << std::endl;
            printUsage(argv[0]);
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
