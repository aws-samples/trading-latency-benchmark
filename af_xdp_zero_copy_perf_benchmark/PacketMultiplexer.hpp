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

#ifndef PACKET_MULTIPLEXER_HPP
#define PACKET_MULTIPLEXER_HPP

#include "AFXDPSocket.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <set>
#include <array>
#include <netinet/in.h>

/**
 * High-performance packet multiplexer using AF_XDP zero copy
 * 
 * This multiplexer:
 * 1. Listens for incoming UDP packets to a specific IP and port
 * 2. Uses AF_XDP zero copy to receive packets with minimal latency
 * 3. Multiplexes received packets to multiple destination EC2 instances
 * 4. Provides control protocol for managing destination instances
 */
class PacketMultiplexer {
public:
    // Control protocol constants
    static constexpr int CONTROL_PORT = 12345;
    static constexpr uint8_t CTRL_ADD_DESTINATION = 1;
    static constexpr uint8_t CTRL_REMOVE_DESTINATION = 2;
    static constexpr uint8_t CTRL_LIST_DESTINATIONS = 3;

    // Destination instance information
    struct Destination {
        std::string ip_address;
        uint16_t port;
        struct sockaddr_in addr;
        
        Destination(const std::string& ip, uint16_t p);
        bool operator<(const Destination& other) const;
    };

private:
    std::string listen_interface_;
    std::string listen_ip_;
    uint16_t listen_port_;
    int num_queues_;
    
    std::vector<std::unique_ptr<AFXDPSocket>> xdp_sockets_;
    std::unique_ptr<AFXDPSocket> output_xdp_socket_;  // Zero-copy output socket
    int control_socket_;
    int output_socket_;  // Fallback regular socket
    
    std::atomic<bool> running_;
    std::vector<std::unique_ptr<std::thread>> packet_processor_threads_;
    std::unique_ptr<std::thread> control_thread_;
    
    mutable std::mutex destinations_mutex_;
    std::set<Destination> destinations_;
    
    // Statistics (per-queue and total)
    static constexpr int MAX_QUEUES = 8;  // Support up to 8 queues
    std::array<std::atomic<uint64_t>, MAX_QUEUES> packets_received_per_queue_;
    std::array<std::atomic<uint64_t>, MAX_QUEUES> packets_sent_per_queue_;
    std::atomic<uint64_t> packets_received_;
    std::atomic<uint64_t> packets_sent_;
    std::atomic<uint64_t> bytes_received_;
    std::atomic<uint64_t> bytes_sent_;

public:
    /**
     * Creates a new PacketMultiplexer
     * 
     * @param interface Network interface to bind to (e.g., "eth0")
     * @param listenIp  IP address to listen on
     * @param listenPort Port to listen on
     * @throws std::runtime_error If initialization fails
     */
    PacketMultiplexer(const std::string& interface, const std::string& listenIp, uint16_t listenPort);

    /**
     * Destructor
     */
    ~PacketMultiplexer();

    // Copy constructor and assignment operator are deleted
    PacketMultiplexer(const PacketMultiplexer&) = delete;
    PacketMultiplexer& operator=(const PacketMultiplexer&) = delete;

    // Move constructor and assignment operator
    PacketMultiplexer(PacketMultiplexer&& other) noexcept;
    PacketMultiplexer& operator=(PacketMultiplexer&& other) noexcept;

    /**
     * Initialize AF_XDP socket and XDP program
     * 
     * @param useZeroCopy Whether to use zero-copy mode (requires driver support)
     * @throws std::runtime_error If initialization fails
     */
    void initialize(bool useZeroCopy = true);

    /**
     * Add a destination EC2 instance
     * 
     * @param ipAddress IP address of the destination
     * @param port      Port of the destination
     * @throws std::runtime_error If adding destination fails
     */
    void addDestination(const std::string& ipAddress, uint16_t port);

    /**
     * Remove a destination EC2 instance
     * 
     * @param ipAddress IP address of the destination
     * @param port      Port of the destination
     * @throws std::runtime_error If removing destination fails
     */
    void removeDestination(const std::string& ipAddress, uint16_t port);

    /**
     * Get list of current destinations
     * 
     * @return Vector of current destinations
     */
    std::vector<Destination> getDestinations() const;

    /**
     * Start the packet multiplexer
     * This will start packet processing and control protocol handling
     */
    void start();

    /**
     * Stop the packet multiplexer
     */
    void stop();

    /**
     * Check if the multiplexer is running
     * 
     * @return True if running, false otherwise
     */
    bool isRunning() const;

    /**
     * Get statistics
     */
    struct Statistics {
        uint64_t packets_received;
        uint64_t packets_sent;
        uint64_t bytes_received;
        uint64_t bytes_sent;
        size_t destinations_count;
    };

    Statistics getStatistics() const;

    /**
     * Print current statistics
     */
    void printStatistics() const;

private:
    /**
     * Configure the XDP program with target IP and port
     */
    void configureXdpProgram();

    /**
     * Packet processing loop for a specific queue
     * 
     * @param queueId Queue ID to process packets for
     */
    void processPacketsForQueue(int queueId);

    /**
     * Handle control protocol messages
     */
    void handleControlProtocol();

    /**
     * Multiplex a single packet to all destinations
     * 
     * @param packetData Pointer to packet data in UMEM
     * @param packetLen  Length of the packet
     * @param queueId    Queue ID for the socket to use
     * @return Number of destinations the packet was sent to
     */
    int multiplexPacket(const uint8_t* packetData, size_t packetLen, int queueId);

    /**
     * Extract UDP payload from a packet
     * 
     * @param packetData Pointer to packet data
     * @param packetLen  Length of the packet
     * @param payloadData Output pointer to payload data
     * @param payloadLen  Output length of payload
     * @return True if UDP payload was successfully extracted
     */
    bool extractUdpPayload(const uint8_t* packetData, size_t packetLen,
                          const uint8_t*& payloadData, size_t& payloadLen);

    /**
     * Send UDP packet to a destination
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @return True if sent successfully
     */
    bool sendToDestination(const Destination& destination, const uint8_t* data, size_t length);

    /**
     * Send UDP packet to destination using zero-copy AF_XDP
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @return True if sent successfully
     */
    bool sendToDestinationZeroCopy(const Destination& destination, const uint8_t* data, size_t length);

    /**
     * Fallback method using regular socket when zero-copy fails
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @return True if sent successfully
     */
    bool sendToDestinationFallback(const Destination& destination, const uint8_t* data, size_t length);

    /**
     * Send UDP packet to destination using specific queue's socket
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @param queueId     Queue ID to use for transmission
     * @return True if sent successfully
     */
    bool sendToDestinationWithQueue(const Destination& destination, const uint8_t* data, size_t length, int queueId);

    /**
     * Send single packet directly (ena-xdp approach, no batching)
     * 
     * @param destination Target destination
     * @param data        Data to send
     * @param length      Length of data
     * @param queueId     Queue ID to use for transmission
     * @return True if sent successfully
     */
    bool sendSinglePacketDirect(const Destination& destination, const uint8_t* data, size_t length, int queueId);

    /**
     * Create UDP packet with headers for zero-copy transmission
     * 
     * @param destination Target destination
     * @param payload     UDP payload data
     * @param payloadLen  Length of payload
     * @param buffer      Output buffer for complete packet
     * @param bufferSize  Size of output buffer
     * @return Length of created packet, or 0 on error
     */
    size_t createUdpPacket(const Destination& destination, const uint8_t* payload, size_t payloadLen,
                          uint8_t* buffer, size_t bufferSize);

    /**
     * Initialize the zero-copy output socket
     * Following ena-xdp best practices for TX frame management
     */
    void initializeOutputSocket();

    /**
     * Pre-populate TX frames with packet templates for better performance
     * Following ena-xdp approach of preparing frames during initialization
     */
    void prePopulateTxFrames();

    /**
     * Create a base packet template that can be modified per destination
     * 
     * @param buffer Buffer to write the template to
     * @return Size of the template packet
     */
    size_t createBasePacketTemplate(uint8_t* buffer);

    /**
     * Update a packet template with specific destination and payload
     * 
     * @param buffer      Buffer containing the packet template
     * @param destination Target destination
     * @param payload     UDP payload data
     * @param payloadLen  Length of payload
     * @return Updated packet size
     */
    size_t updatePacketForDestination(uint8_t* buffer, const Destination& destination, 
                                     const uint8_t* payload, size_t payloadLen);

    /**
     * Calculate total packet size including headers
     * 
     * @param payloadLen UDP payload length
     * @return Total packet size
     */
    size_t calculatePacketSize(size_t payloadLen) const;

    /**
     * Process control message
     * 
     * @param message    Control message data
     * @param messageLen Length of control message
     * @param clientAddr Address of client that sent the message
     * @return Response message (empty if no response needed)
     */
    std::vector<uint8_t> processControlMessage(const uint8_t* message, size_t messageLen, 
                                               const struct sockaddr_in& clientAddr);

    /**
     * Helper method to parse IP address string to network byte order
     */
    uint32_t parseIpAddress(const std::string& ipStr);

    /**
     * Helper method to format IP address from network byte order to string
     */
    std::string formatIpAddress(uint32_t ipAddr);

    /**
     * Get interface IP address
     * 
     * @param interface Interface name
     * @param ip_address Output IP address string
     * @return True if successful
     */
    bool getInterfaceIp(const std::string& interface, std::string& ip_address);

    /**
     * Get interface MAC address
     * 
     * @param interface Interface name
     * @param mac_address Output MAC address (6 bytes)
     * @return True if successful
     */
    bool getInterfaceMac(const std::string& interface, uint8_t* mac_address);

    /**
     * Get destination MAC address via ARP lookup
     * 
     * @param ip_address Destination IP address string
     * @param mac_address Output MAC address (6 bytes)
     * @return True if successful
     */
    bool getDestinationMac(const std::string& ip_address, uint8_t* mac_address);

    /**
     * Trigger ARP resolution for a destination IP
     * 
     * @param ip_address Destination IP address to resolve
     */
    void triggerArpResolution(const std::string& ip_address);
};

#endif // PACKET_MULTIPLEXER_HPP
