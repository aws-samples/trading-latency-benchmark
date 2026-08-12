/*
 * KernelEcho.cpp — Lightweight UDP echo server for integration testing.
 *
 * Provides the same external interface as the AF_XDP replicator (control
 * protocol on port 12345 + data echo) but uses standard kernel sockets.
 * No root, no XDP, no BPF — runs anywhere.
 *
 * Used with: ./replicator --echo-mode <listen_ip> <port>
 *
 * Limitations vs AF_XDP mode:
 *   - ~10-50x higher latency (kernel network stack overhead)
 *   - No zero-copy, no busy-poll at NIC level
 *   - Single-threaded (no multi-queue)
 *   - No tunnel decapsulation
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <thread>
#include <algorithm>

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "common/ControlPort.hpp"

// Shared with ReplicatorMain.cpp signal handler
extern volatile bool g_running;

static constexpr int MAX_DESTINATIONS = 64;

struct Destination {
    struct sockaddr_in addr;
    bool active;
};

static Destination g_destinations[MAX_DESTINATIONS] = {};
static int g_dest_count = 0;

// ── Control protocol ─────────────────────────────────────────────────────────
// Wire format: [1B command][4B IP (network order)][2B port (network order)]
// Commands: 1=ADD, 2=REMOVE, 3=LIST
// Response: 1B (1=OK, 0=FAIL)

static void handle_control(int ctrl_fd) {
    uint8_t buf[64];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ssize_t n = recvfrom(ctrl_fd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&client_addr, &client_len);
    if (n < 1) return;

    uint8_t cmd = buf[0];
    uint8_t ack = 0;

    switch (cmd) {
        case 1: { // ADD
            if (n < 7) break;
            struct sockaddr_in dest = {};
            dest.sin_family = AF_INET;
            memcpy(&dest.sin_addr, &buf[1], 4);
            memcpy(&dest.sin_port, &buf[5], 2);

            // Check if already registered
            bool found = false;
            for (int i = 0; i < g_dest_count; i++) {
                if (g_destinations[i].active &&
                    g_destinations[i].addr.sin_addr.s_addr == dest.sin_addr.s_addr &&
                    g_destinations[i].addr.sin_port == dest.sin_port) {
                    found = true;
                    break;
                }
            }
            if (!found && g_dest_count < MAX_DESTINATIONS) {
                g_destinations[g_dest_count].addr = dest;
                g_destinations[g_dest_count].active = true;
                g_dest_count++;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &dest.sin_addr, ip_str, sizeof(ip_str));
                std::cout << "[CTRL] Added destination: " << ip_str
                          << ":" << ntohs(dest.sin_port) << std::endl;
            }
            ack = 1;
            break;
        }
        case 2: { // REMOVE
            if (n < 7) break;
            uint32_t ip;
            uint16_t port;
            memcpy(&ip, &buf[1], 4);
            memcpy(&port, &buf[5], 2);
            for (int i = 0; i < g_dest_count; i++) {
                if (g_destinations[i].active &&
                    g_destinations[i].addr.sin_addr.s_addr == ip &&
                    g_destinations[i].addr.sin_port == port) {
                    g_destinations[i].active = false;
                    ack = 1;
                    std::cout << "[CTRL] Removed destination" << std::endl;
                    break;
                }
            }
            break;
        }
        case 3: { // LIST — return production wire format: [1B count][per dest: 4B IP + 2B port]
            std::vector<uint8_t> resp;
            uint8_t count = 0;
            for (int i = 0; i < g_dest_count; i++)
                if (g_destinations[i].active) count++;
            resp.push_back(count);
            for (int i = 0; i < g_dest_count; i++) {
                if (!g_destinations[i].active) continue;
                uint32_t ip = g_destinations[i].addr.sin_addr.s_addr;   // network order
                uint16_t port = g_destinations[i].addr.sin_port;         // network order
                resp.insert(resp.end(), reinterpret_cast<uint8_t*>(&ip), reinterpret_cast<uint8_t*>(&ip) + 4);
                resp.insert(resp.end(), reinterpret_cast<uint8_t*>(&port), reinterpret_cast<uint8_t*>(&port) + 2);
            }
            sendto(ctrl_fd, resp.data(), resp.size(), 0,
                   (struct sockaddr*)&client_addr, client_len);
            return;  // full response already sent; skip the trailing 1-byte ack
        }
    }

    sendto(ctrl_fd, &ack, 1, 0, (struct sockaddr*)&client_addr, client_len);
}

// ── Data echo ────────────────────────────────────────────────────────────────
static void handle_data(int data_fd) {
    uint8_t buf[2048];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ssize_t n = recvfrom(data_fd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&src_addr, &src_len);
    if (n <= 0) return;

    // Echo to all registered destinations
    for (int i = 0; i < g_dest_count; i++) {
        if (!g_destinations[i].active) continue;
        sendto(data_fd, buf, n, 0,
               (struct sockaddr*)&g_destinations[i].addr,
               sizeof(g_destinations[i].addr));
    }
}

// ── Main loop ────────────────────────────────────────────────────────────────
int run_echo_mode(const std::string& listen_ip, uint16_t listen_port) {
    const uint16_t CONTROL_PORT = afxdp_control_port();
    std::cout << "=== Replicator (echo mode) ===" << std::endl;
    std::cout << "Listen:   " << listen_ip << ":" << listen_port << std::endl;
    std::cout << "Control:  port " << CONTROL_PORT << std::endl;
    std::cout << "Mode:     kernel UDP echo (no AF_XDP, no root required)" << std::endl;
    std::cout << "================================" << std::endl;

    // Data socket
    int data_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data_fd < 0) { perror("data socket"); return 1; }

    struct sockaddr_in data_addr = {};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(listen_port);
    inet_pton(AF_INET, listen_ip.c_str(), &data_addr.sin_addr);

    if (bind(data_fd, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("bind data"); close(data_fd); return 1;
    }

    // Control socket
    int ctrl_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctrl_fd < 0) { perror("ctrl socket"); close(data_fd); return 1; }

    struct sockaddr_in ctrl_addr = {};
    ctrl_addr.sin_family = AF_INET;
    ctrl_addr.sin_port = htons(CONTROL_PORT);
    ctrl_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ctrl_fd, (struct sockaddr*)&ctrl_addr, sizeof(ctrl_addr)) < 0) {
        perror("bind ctrl"); close(data_fd); close(ctrl_fd); return 1;
    }

    std::cout << "Listening... (Ctrl+C to stop)" << std::endl;

    struct pollfd fds[2];
    fds[0].fd = data_fd;  fds[0].events = POLLIN;
    fds[1].fd = ctrl_fd;  fds[1].events = POLLIN;

    while (g_running) {
        int ret = poll(fds, 2, 500);  // 500ms timeout for signal check
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) handle_data(data_fd);
        if (fds[1].revents & POLLIN) handle_control(ctrl_fd);
    }

    std::cout << "\nShutting down echo-mode replicator." << std::endl;
    close(data_fd);
    close(ctrl_fd);
    return 0;
}
