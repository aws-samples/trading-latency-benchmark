/*
 * Market Data Provider Trade Feed Round-Trip Latency Benchmark Client
 * 
 * This client measures round-trip latency through the AF_XDP packet multiplexer by:
 * 1. Listening for UDP messages as a subscriber
 * 2. Adding itself as a subscriber to the packet multiplexer
 * 3. Sending Market Data Provider trade messages with sequential IDs
 * 4. Receiving echoed messages and calculating round-trip time
 * 5. Generating comprehensive latency and packet loss reports
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <sstream>
#include <mutex>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>

static volatile bool g_running = true;
static std::atomic<bool> g_receiving_started(false);

// Timing information per trade ID
struct TradeTimingInfo {
    std::chrono::high_resolution_clock::time_point send_time;
    bool received;
    uint64_t rtt_us;
};

// Global timing map (trade_id -> timing info)
static std::unordered_map<uint64_t, TradeTimingInfo> g_timing_map;
static std::mutex g_timing_mutex;

// Statistics
static std::atomic<uint64_t> g_total_sent(0);
static std::atomic<uint64_t> g_total_received(0);
static std::atomic<uint64_t> g_total_lost(0);
static std::vector<std::atomic<uint64_t>> g_latency_histogram(10000); // 0-9999 microseconds
static std::atomic<uint64_t> g_latency_over_10ms(0);

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", stopping..." << std::endl;
    g_running = false;
}

class MarketDataProviderTradeMessage {
private:
    // Pre-built static portion of the message (everything except trade_id)
    static std::string message_template;
    static const size_t trade_id_offset = 38; // Position where trade_id starts in the JSON
    
public:
    static void initializeTemplate() {
        // Build a template with placeholder for trade_id
        message_template = R"({"e":"trade","E":1234567890123,"s":"BTC-USDT","t":0000000000,"p":"45000","q":"1.5","b":1000000001,"a":1000000002,"T":1234567890000,"S":"1","X":"MARKET"})";
    }
    
    // Highly optimized message generation - only updates trade_id
    static void generateTradeMessage(uint64_t trade_id, char* buffer, size_t& length) {
        // Copy template
        memcpy(buffer, message_template.c_str(), message_template.length());
        
        // Write trade_id directly into the buffer (10 digits, zero-padded)
        char* id_pos = buffer + trade_id_offset;
        for (int i = 9; i >= 0; i--) {
            id_pos[i] = '0' + (trade_id % 10);
            trade_id /= 10;
        }
        
        length = message_template.length();
        buffer[length] = '\0';
    }
    
    // Extract trade_id from received message
    static uint64_t extractTradeId(const char* message, size_t length) {
        if (length < trade_id_offset + 10) return 0;
        
        uint64_t trade_id = 0;
        const char* id_pos = message + trade_id_offset;
        for (int i = 0; i < 10; i++) {
            if (id_pos[i] < '0' || id_pos[i] > '9') return 0;
            trade_id = trade_id * 10 + (id_pos[i] - '0');
        }
        return trade_id;
    }
};

// Static member initialization
std::string MarketDataProviderTradeMessage::message_template;

class RoundTripBenchmark {
private:
    std::string multiplexer_ip_;
    uint16_t multiplexer_port_;
    std::string local_ip_;
    uint16_t local_port_;
    
    int send_socket_;
    int recv_socket_;
    struct sockaddr_in multiplexer_addr_;
    
    std::thread receiver_thread_;
    
public:
    RoundTripBenchmark(const std::string& multiplexer_ip, uint16_t multiplexer_port,
                      const std::string& local_ip, uint16_t local_port)
        : multiplexer_ip_(multiplexer_ip), multiplexer_port_(multiplexer_port),
          local_ip_(local_ip), local_port_(local_port),
          send_socket_(-1), recv_socket_(-1) {}
    
    ~RoundTripBenchmark() {
        if (send_socket_ >= 0) close(send_socket_);
        if (recv_socket_ >= 0) close(recv_socket_);
    }
    
    bool initialize() {
        // Create send socket
        send_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (send_socket_ < 0) {
            std::cerr << "Failed to create send socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Setup multiplexer address
        memset(&multiplexer_addr_, 0, sizeof(multiplexer_addr_));
        multiplexer_addr_.sin_family = AF_INET;
        multiplexer_addr_.sin_port = htons(multiplexer_port_);
        if (inet_aton(multiplexer_ip_.c_str(), &multiplexer_addr_.sin_addr) == 0) {
            std::cerr << "Invalid multiplexer IP: " << multiplexer_ip_ << std::endl;
            return false;
        }
        
        // Create receive socket
        recv_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (recv_socket_ < 0) {
            std::cerr << "Failed to create receive socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Make receive socket non-blocking
        int flags = fcntl(recv_socket_, F_GETFL, 0);
        fcntl(recv_socket_, F_SETFL, flags | O_NONBLOCK);
        
        // Increase receive buffer size for high-speed reception
        int recv_buf_size = 4 * 1024 * 1024; // 4MB
        setsockopt(recv_socket_, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size));
        
        // Bind receive socket
        struct sockaddr_in recv_addr;
        memset(&recv_addr, 0, sizeof(recv_addr));
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_port = htons(local_port_);
        if (inet_aton(local_ip_.c_str(), &recv_addr.sin_addr) == 0) {
            std::cerr << "Invalid local IP: " << local_ip_ << std::endl;
            return false;
        }
        
        if (bind(recv_socket_, (struct sockaddr*)&recv_addr, sizeof(recv_addr)) < 0) {
            std::cerr << "Failed to bind receive socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        std::cout << "Listening on " << local_ip_ << ":" << local_port_ << std::endl;
        
        // Add self as subscriber to packet multiplexer
        return addSelfAsSubscriber();
    }
    
    bool addSelfAsSubscriber() {
        // Use control_client to add ourselves as a subscriber
        std::stringstream cmd;
        cmd << "./control_client " << multiplexer_ip_ << " add " << local_ip_ << " " << local_port_;
        
        std::cout << "Adding self as subscriber: " << cmd.str() << std::endl;
        
        int result = system(cmd.str().c_str());
        if (result != 0) {
            std::cerr << "Failed to add self as subscriber (exit code: " << result << ")" << std::endl;
            return false;
        }
        
        std::cout << "Successfully added as subscriber" << std::endl;
        return true;
    }
    
    void startReceiver() {
        receiver_thread_ = std::thread([this]() { receiveLoop(); });
        
        // Wait for receiver to start
        while (!g_receiving_started && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    void receiveLoop() {
        char buffer[2048];
        struct pollfd pfd;
        pfd.fd = recv_socket_;
        pfd.events = POLLIN;
        
        g_receiving_started = true;
        std::cout << "Receiver thread started" << std::endl;
        
        while (g_running) {
            int ret = poll(&pfd, 1, 100); // 100ms timeout
            if (ret <= 0) continue;
            
            ssize_t received = recv(recv_socket_, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                auto recv_time = std::chrono::high_resolution_clock::now();
                
                // Extract trade_id
                uint64_t trade_id = MarketDataProviderTradeMessage::extractTradeId(buffer, received);
                if (trade_id > 0) {
                    processReceivedMessage(trade_id, recv_time);
                }
            }
        }
        
        std::cout << "Receiver thread stopped" << std::endl;
    }
    
    void processReceivedMessage(uint64_t trade_id, const std::chrono::high_resolution_clock::time_point& recv_time) {
        std::lock_guard<std::mutex> lock(g_timing_mutex);
        
        auto it = g_timing_map.find(trade_id);
        if (it != g_timing_map.end() && !it->second.received) {
            // Calculate round-trip time
            auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                recv_time - it->second.send_time).count();
            
            it->second.received = true;
            it->second.rtt_us = rtt;
            
            // Update histogram
            if (rtt < 10000) {
                g_latency_histogram[rtt]++;
            } else {
                g_latency_over_10ms++;
            }
            
            g_total_received++;
            
            // Progress update every 10000 messages
            if (g_total_received % 10000 == 0) {
                std::cout << "Received: " << g_total_received << " messages, RTT: " << rtt << " μs" << std::endl;
            }
        }
    }
    
    void runBenchmark(uint64_t total_messages, uint64_t messages_per_sec) {
        std::cout << "\nStarting benchmark: " << total_messages << " messages at " 
                  << messages_per_sec << " msg/sec" << std::endl;
        
        // Start receiver thread
        startReceiver();
        
        // Give receiver time to stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Calculate timing
        auto interval_us = std::chrono::microseconds(1000000 / messages_per_sec);
        char message_buffer[512];
        size_t message_length;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        auto next_send_time = start_time;
        
        // Send messages
        for (uint64_t trade_id = 1; trade_id <= total_messages && g_running; trade_id++) {
            // Generate message
            MarketDataProviderTradeMessage::generateTradeMessage(trade_id, message_buffer, message_length);
            
            // Record send time
            auto send_time = std::chrono::high_resolution_clock::now();
            {
                std::lock_guard<std::mutex> lock(g_timing_mutex);
                g_timing_map[trade_id] = {send_time, false, 0};
            }
            
            // Send message
            ssize_t sent = sendto(send_socket_, message_buffer, message_length, 0,
                                 (struct sockaddr*)&multiplexer_addr_, sizeof(multiplexer_addr_));
            
            if (sent < 0) {
                std::cerr << "Send error: " << strerror(errno) << std::endl;
                continue;
            }
            
            g_total_sent++;
            
            // Progress update
            if (trade_id % 10000 == 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::high_resolution_clock::now() - start_time).count();
                std::cout << "Sent: " << trade_id << " messages (" << elapsed << "s)" << std::endl;
            }
            
            // Pace sending
            next_send_time += interval_us;
            std::this_thread::sleep_until(next_send_time);
        }
        
        auto send_complete_time = std::chrono::high_resolution_clock::now();
        auto send_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            send_complete_time - start_time).count();
        
        std::cout << "\nSending complete. Waiting for remaining responses..." << std::endl;
        
        // Wait for remaining responses (up to 5 seconds)
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Stop receiver
        g_running = false;
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
        
        // Calculate final statistics
        calculateStatistics(send_duration);
    }
    
    void calculateStatistics(uint64_t send_duration_ms) {
        std::cout << "\n=== Round-Trip Latency Benchmark Results ===" << std::endl;
        std::cout << "Total messages sent: " << g_total_sent << std::endl;
        std::cout << "Total messages received: " << g_total_received << std::endl;
        
        // Calculate packet loss
        uint64_t lost = 0;
        uint64_t total_rtt = 0;
        uint64_t min_rtt = UINT64_MAX;
        uint64_t max_rtt = 0;
        std::vector<uint64_t> all_rtts;
        
        {
            std::lock_guard<std::mutex> lock(g_timing_mutex);
            for (const auto& pair : g_timing_map) {
                if (!pair.second.received) {
                    lost++;
                } else {
                    uint64_t rtt = pair.second.rtt_us;
                    total_rtt += rtt;
                    min_rtt = std::min(min_rtt, rtt);
                    max_rtt = std::max(max_rtt, rtt);
                    all_rtts.push_back(rtt);
                }
            }
        }
        
        g_total_lost = lost;
        double loss_rate = (g_total_sent > 0) ? (100.0 * lost / g_total_sent) : 0;
        
        std::cout << "Packet loss: " << lost << " (" << std::fixed << std::setprecision(2) 
                  << loss_rate << "%)" << std::endl;
        
        // Calculate throughput
        double actual_send_rate = (send_duration_ms > 0) ? 
            (g_total_sent * 1000.0 / send_duration_ms) : 0;
        std::cout << "Actual send rate: " << std::fixed << std::setprecision(0) 
                  << actual_send_rate << " msg/sec" << std::endl;
        
        if (g_total_received > 0) {
            // Latency statistics
            double avg_rtt = (double)total_rtt / g_total_received;
            std::cout << "\nRound-Trip Time Statistics:" << std::endl;
            std::cout << "Min RTT: " << min_rtt << " μs" << std::endl;
            std::cout << "Avg RTT: " << std::fixed << std::setprecision(1) << avg_rtt << " μs" << std::endl;
            std::cout << "Max RTT: " << max_rtt << " μs" << std::endl;
            
            // Calculate percentiles
            if (!all_rtts.empty()) {
                std::sort(all_rtts.begin(), all_rtts.end());
                
                auto percentile = [&](double p) -> uint64_t {
                    size_t idx = std::min(all_rtts.size() - 1, 
                                         (size_t)(all_rtts.size() * p / 100.0));
                    return all_rtts[idx];
                };
                
                std::cout << "\nLatency Percentiles:" << std::endl;
                std::cout << "  50%: " << percentile(50) << " μs" << std::endl;
                std::cout << "  90%: " << percentile(90) << " μs" << std::endl;
                std::cout << "  95%: " << percentile(95) << " μs" << std::endl;
                std::cout << "  99%: " << percentile(99) << " μs" << std::endl;
                std::cout << "  99.9%: " << percentile(99.9) << " μs" << std::endl;
            }
            
            // Print histogram summary
            printHistogram();
        }
        
        std::cout << "============================================" << std::endl;
    }
    
    void printHistogram() {
        std::cout << "\nLatency Histogram (top buckets):" << std::endl;
        
        // Find non-zero buckets
        std::vector<std::pair<uint64_t, uint64_t>> buckets;
        for (size_t i = 0; i < g_latency_histogram.size(); i++) {
            uint64_t count = g_latency_histogram[i].load();
            if (count > 0) {
                buckets.push_back({i, count});
            }
        }
        
        // Sort by count (descending)
        std::sort(buckets.begin(), buckets.end(), 
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        
        // Show top 10 buckets
        size_t shown = 0;
        for (const auto& bucket : buckets) {
            if (shown++ >= 10) break;
            
            double percentage = (100.0 * bucket.second) / g_total_received;
            std::cout << "  " << bucket.first << " μs: " << bucket.second 
                      << " (" << std::fixed << std::setprecision(1) << percentage << "%)" << std::endl;
        }
        
        if (g_latency_over_10ms > 0) {
            double percentage = (100.0 * g_latency_over_10ms) / g_total_received;
            std::cout << "  >10ms: " << g_latency_over_10ms 
                      << " (" << std::fixed << std::setprecision(1) << percentage << "%)" << std::endl;
        }
    }
};

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " <multiplexer_ip> <multiplexer_port> <local_ip> <local_port> "
              << "<total_messages> <messages_per_sec>" << std::endl;
    std::cout << std::endl;
    std::cout << "Parameters:" << std::endl;
    std::cout << "  multiplexer_ip:   IP address of the packet multiplexer" << std::endl;
    std::cout << "  multiplexer_port: Port of the packet multiplexer" << std::endl;
    std::cout << "  local_ip:        Local IP to listen for echoed messages" << std::endl;
    std::cout << "  local_port:      Local port to listen on" << std::endl;
    std::cout << "  total_messages:  Total number of messages to send (e.g., 1000000)" << std::endl;
    std::cout << "  messages_per_sec: Target message rate (e.g., 10000)" << std::endl;
    std::cout << std::endl;
    std::cout << "Example:" << std::endl;
    std::cout << "  " << progName << " 10.0.0.71 9000 10.0.0.34 9001 1000000 10000" << std::endl;
    std::cout << std::endl;
    std::cout << "This client measures round-trip latency through the AF_XDP packet multiplexer." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 7) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string multiplexer_ip = argv[1];
    uint16_t multiplexer_port = std::stoi(argv[2]);
    std::string local_ip = argv[3];
    uint16_t local_port = std::stoi(argv[4]);
    uint64_t total_messages = std::stoull(argv[5]);
    uint64_t messages_per_sec = std::stoull(argv[6]);
    
    if (total_messages == 0 || messages_per_sec == 0) {
        std::cerr << "Error: total_messages and messages_per_sec must be positive" << std::endl;
        return 1;
    }
    
    std::cout << "=== Market Data Provider Trade Feed Round-Trip Benchmark ===" << std::endl;
    std::cout << "Multiplexer: " << multiplexer_ip << ":" << multiplexer_port << std::endl;
    std::cout << "Local endpoint: " << local_ip << ":" << local_port << std::endl;
    std::cout << "Total messages: " << total_messages << std::endl;
    std::cout << "Target rate: " << messages_per_sec << " msg/sec" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    // Setup signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Initialize message template
    MarketDataProviderTradeMessage::initializeTemplate();
    
    try {
        RoundTripBenchmark benchmark(multiplexer_ip, multiplexer_port, local_ip, local_port);
        
        if (!benchmark.initialize()) {
            std::cerr << "Failed to initialize benchmark" << std::endl;
            return 1;
        }
        
        benchmark.runBenchmark(total_messages, messages_per_sec);
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
