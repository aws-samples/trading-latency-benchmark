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

// ReplicatorCore.cpp — object lifecycle (construction/destruction/move),
// thread start/stop orchestration, statistics reporting, and CPU affinity.

#include <algorithm>
#include "Internal.hpp"

// Replicator implementation
Replicator::Replicator(const std::string& interface, const std::string& listenIp, uint16_t listenPort, int numQueues)
    : listen_interface_(interface), listen_ip_(listenIp), listen_port_(listenPort),
      num_queues_(numQueues), mcast_mode_(false),
      control_socket_(-1), output_socket_(-1),
      ctrl_multicast_port_(0), producer_port_(0),
      ctrl_multicast_socket_(-1), ctrl_forward_socket_(-1),
      running_(false),
      packets_received_(0), packets_sent_(0), bytes_received_(0), bytes_sent_(0) {
    
    // Initialize per-queue statistics
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].v.store(0);
        packets_sent_per_queue_[i].v.store(0);
    }
    
    // Initialize CPU core assignments
    initializeCpuCores();

    std::cout << "Replicator initializing for " << listen_ip_ << ":" << listen_port_
              << " on interface " << listen_interface_ << " with " << num_queues_ << " queues" << std::endl;
}

Replicator::~Replicator() {
    stop();

    if (control_socket_ >= 0) {
        ::close(control_socket_);
    }
    if (output_socket_ >= 0) {
        ::close(output_socket_);
    }
}

Replicator::Replicator(Replicator&& other) noexcept
    : listen_interface_(std::move(other.listen_interface_)),
      listen_ip_(std::move(other.listen_ip_)),
      listen_port_(other.listen_port_),
      num_queues_(other.num_queues_),
      mcast_mode_(other.mcast_mode_),
      config_map_fd_(other.config_map_fd_),
      group_slots_(std::move(other.group_slots_)),
      group_ref_counts_(std::move(other.group_ref_counts_)),
      free_slots_(std::move(other.free_slots_)),
      group_destinations_(std::move(other.group_destinations_)),
      listen_ip_nbo_(other.listen_ip_nbo_),
      ctrl_multicast_group_(std::move(other.ctrl_multicast_group_)),
      ctrl_multicast_port_(other.ctrl_multicast_port_),
      producer_ip_(std::move(other.producer_ip_)),
      producer_port_(other.producer_port_),
      ctrl_multicast_socket_(other.ctrl_multicast_socket_),
      ctrl_forward_socket_(other.ctrl_forward_socket_),
      cached_iface_ip_(std::move(other.cached_iface_ip_)),
      xdp_sockets_(std::move(other.xdp_sockets_)),
      control_socket_(other.control_socket_),
      output_socket_(other.output_socket_),
      running_(other.running_.load()),
      packet_processor_threads_(std::move(other.packet_processor_threads_)),
      control_thread_(std::move(other.control_thread_)),
      ctrl_upstream_thread_(std::move(other.ctrl_upstream_thread_)),
      all_destinations_(std::move(other.all_destinations_)),
      packets_received_(other.packets_received_.load()),
      packets_sent_(other.packets_sent_.load()),
      bytes_received_(other.bytes_received_.load()),
      bytes_sent_(other.bytes_sent_.load()) {

    memcpy(cached_iface_mac_, other.cached_iface_mac_, sizeof(cached_iface_mac_));

    // Copy per-queue statistics arrays
    for (int i = 0; i < MAX_QUEUES; i++) {
        packets_received_per_queue_[i].v.store(other.packets_received_per_queue_[i].v.load());
        packets_sent_per_queue_[i].v.store(other.packets_sent_per_queue_[i].v.load());
    }

    other.config_map_fd_ = -1;
    other.ctrl_multicast_socket_ = -1;
    other.ctrl_forward_socket_ = -1;
    other.control_socket_ = -1;
    other.output_socket_ = -1;
    other.running_ = false;
}

Replicator& Replicator::operator=(Replicator&& other) noexcept {
    if (this != &other) {
        stop();

        listen_interface_      = std::move(other.listen_interface_);
        listen_ip_             = std::move(other.listen_ip_);
        listen_port_           = other.listen_port_;
        num_queues_            = other.num_queues_;
        mcast_mode_              = other.mcast_mode_;
        config_map_fd_         = other.config_map_fd_;
        group_slots_           = std::move(other.group_slots_);
        group_ref_counts_      = std::move(other.group_ref_counts_);
        free_slots_            = std::move(other.free_slots_);
        group_destinations_    = std::move(other.group_destinations_);
        listen_ip_nbo_         = other.listen_ip_nbo_;
        ctrl_multicast_group_  = std::move(other.ctrl_multicast_group_);
        ctrl_multicast_port_   = other.ctrl_multicast_port_;
        producer_ip_           = std::move(other.producer_ip_);
        producer_port_         = other.producer_port_;
        ctrl_multicast_socket_ = other.ctrl_multicast_socket_;
        ctrl_forward_socket_   = other.ctrl_forward_socket_;
        cached_iface_ip_       = std::move(other.cached_iface_ip_);
        memcpy(cached_iface_mac_, other.cached_iface_mac_, sizeof(cached_iface_mac_));
        xdp_sockets_           = std::move(other.xdp_sockets_);
        control_socket_        = other.control_socket_;
        output_socket_         = other.output_socket_;
        running_               = other.running_.load();
        packet_processor_threads_ = std::move(other.packet_processor_threads_);
        control_thread_        = std::move(other.control_thread_);
        ctrl_upstream_thread_  = std::move(other.ctrl_upstream_thread_);
        all_destinations_      = std::move(other.all_destinations_);
        packets_received_      = other.packets_received_.load();
        packets_sent_          = other.packets_sent_.load();
        bytes_received_        = other.bytes_received_.load();
        bytes_sent_            = other.bytes_sent_.load();

        for (int i = 0; i < MAX_QUEUES; i++) {
            packets_received_per_queue_[i].v.store(other.packets_received_per_queue_[i].v.load());
            packets_sent_per_queue_[i].v.store(other.packets_sent_per_queue_[i].v.load());
        }

        other.config_map_fd_         = -1;
        other.ctrl_multicast_socket_ = -1;
        other.ctrl_forward_socket_   = -1;
        other.control_socket_        = -1;
        other.output_socket_         = -1;
        other.running_               = false;
    }
    return *this;
}

void Replicator::start() {
    if (!running_.exchange(true)) {
        std::cout << "Starting HFT-optimized Replicator..." << std::endl;
        
        // Start packet processing threads for each queue
        packet_processor_threads_.resize(num_queues_);
        for (int queue_id = 0; queue_id < num_queues_; queue_id++) {
            packet_processor_threads_[queue_id] = std::make_unique<std::thread>(
                [this, queue_id]() { this->processPacketsForQueue(queue_id); }
            );
            
            // Bind each packet-processing thread to its dedicated isolated core.
            if (queue_id < static_cast<int>(cpu_cores_.size())) {
                setCpuAffinity(*packet_processor_threads_[queue_id], cpu_cores_[queue_id]);
            }
            
            std::cout << "Started HFT-optimized packet processing thread for queue " << queue_id << std::endl;
        }
        
        // Publish the fan-out snapshot off the packet threads: rebuilding it inline
        // took a mutex and resolved ARP on the RX path every 100 ms.
        dest_refresh_thread_ = std::make_unique<std::thread>(&Replicator::destRefreshLoop, this);

        // Start control protocol thread (don't bind to specific core to avoid interference)
        control_thread_ = std::make_unique<std::thread>(&Replicator::handleControlProtocol, this);

        // Start upstream control forwarding thread if configured
        if (!ctrl_multicast_group_.empty()) {
            ctrl_upstream_thread_ = std::make_unique<std::thread>(&Replicator::handleUpstreamControl, this);
            std::cout << "Started upstream control thread" << std::endl;
        }

        std::cout << "HFT-optimized Replicator started with " << num_queues_ << " processing threads" << std::endl;
        std::cout << "CPU affinity applied, busy polling enabled, lock-free operations active" << std::endl;
    }
}

void Replicator::stop() {
    if (running_.exchange(false)) {
        std::cout << "Stopping Replicator..." << std::endl;
        
        // Wait for all packet processor threads to finish
        for (auto& thread : packet_processor_threads_) {
            if (thread && thread->joinable()) {
                thread->join();
            }
        }
        packet_processor_threads_.clear();
        
        // Wait for control thread to finish
        if (dest_refresh_thread_ && dest_refresh_thread_->joinable()) {
            dest_refresh_thread_->join();
            dest_refresh_thread_.reset();
        }
        // Readers are stopped, so retired snapshots can go.
        dest_snapshot_.store(nullptr, std::memory_order_release);
        for (auto& s : snapshot_ring_) s.reset();

        if (control_thread_ && control_thread_->joinable()) {
            control_thread_->join();
            control_thread_.reset();
        }

        // Wait for upstream control thread to finish
        if (ctrl_upstream_thread_ && ctrl_upstream_thread_->joinable()) {
            ctrl_upstream_thread_->join();
            ctrl_upstream_thread_.reset();
        }

        // Leave control multicast group
        if (ctrl_multicast_socket_ >= 0) {
            struct ip_mreqn mreq{};
            inet_aton(ctrl_multicast_group_.c_str(), &mreq.imr_multiaddr);
            mreq.imr_address.s_addr = INADDR_ANY;
            mreq.imr_ifindex = static_cast<int>(if_nametoindex(listen_interface_.c_str()));
            setsockopt(ctrl_multicast_socket_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            ::close(ctrl_multicast_socket_);
            ctrl_multicast_socket_ = -1;
        }
        if (ctrl_forward_socket_ >= 0) {
            ::close(ctrl_forward_socket_);
            ctrl_forward_socket_ = -1;
        }

        // Clear BPF group tracking tables
        {
            std::lock_guard<std::mutex> lock(group_mutex_);
            group_slots_.clear();
            group_ref_counts_.clear();
            free_slots_.clear();
        }

        // Clear destination routing tables so a restart begins clean
        {
            std::lock_guard<std::mutex> lock(destinations_mutex_);
            group_destinations_.clear();
            all_destinations_.clear();
        }

        // Unload XDP program
        XdpSocket::unloadXdpProgram(listen_interface_, true);
        
        std::cout << "Replicator stopped" << std::endl;
    }
}

bool Replicator::isRunning() const {
    return running_.load();
}

Replicator::Statistics Replicator::getStatistics() const {
    std::lock_guard<std::mutex> lock(destinations_mutex_);
    return {
        packets_received_.load(),
        packets_sent_.load(),
        bytes_received_.load(),
        bytes_sent_.load(),
        all_destinations_.size()
    };
}

void Replicator::printStatistics() const {
    auto stats = getStatistics();
    std::cout << "=== Replicator Statistics ===" << std::endl;
    std::cout << "Packets received: " << stats.packets_received << std::endl;
    std::cout << "Packets sent: " << stats.packets_sent << std::endl;
    std::cout << "Bytes received: " << stats.bytes_received << std::endl;
    std::cout << "Bytes sent: " << stats.bytes_sent << std::endl;
    std::cout << "Active destinations: " << stats.destinations_count << std::endl;
    std::cout << "=================================" << std::endl;
}

bool Replicator::setCpuAffinity(std::thread& thread, int cpu_core) {
    // Pin a thread to a specific CPU core and print the result for verification.
    if (!enable_cpu_affinity_) {
        return true;  // CPU affinity disabled
    }
    
    pthread_t native_handle = thread.native_handle();
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core, &cpuset);
    
    int result = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        std::cerr << "Failed to set CPU affinity for thread to core " << cpu_core 
                  << ": " << strerror(result) << std::endl;
        return false;
    }
    
    std::cout << "Successfully bound thread to CPU core " << cpu_core << std::endl;
    return true;
}

// Parse a Linux cpu-list string ("2", "2,4", "1-3", "1-3,5,7-8") into a sorted
// vector of CPU indices. Used for REPLICATOR_CPUS and /sys/.../cpu/isolated.
static std::vector<int> parseCpuList(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // trim whitespace
        size_t a = tok.find_first_not_of(" \t\n\r");
        if (a == std::string::npos) continue;
        size_t b = tok.find_last_not_of(" \t\n\r");
        tok = tok.substr(a, b - a + 1);
        auto dash = tok.find('-');
        if (dash == std::string::npos) {
            try { out.push_back(std::stoi(tok)); } catch (...) {}
        } else {
            try {
                int lo = std::stoi(tok.substr(0, dash));
                int hi = std::stoi(tok.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) out.push_back(c);
            } catch (...) {}
        }
    }
    return out;
}

// Read the kernel's isolated-CPU set (isolcpus= / nohz_full=) from sysfs.
// Empty when the host has no CPU isolation configured.
static std::vector<int> readIsolatedCpus() {
    std::ifstream f("/sys/devices/system/cpu/isolated");
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return parseCpuList(line);
}

// Read the kernel's ONLINE CPU set. Excludes SMT siblings that nosmt took
// offline (so we never pin a poll thread to a non-existent core).
static std::vector<int> readOnlineCpus() {
    std::ifstream f("/sys/devices/system/cpu/online");
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return parseCpuList(line);
}

// CPUs actually servicing the ENA NIC hard IRQs (from /proc/interrupts +
// /proc/irq/N/smp_affinity_list). Poll threads must avoid these — but we must
// use the REAL placement, not an assumption (the IRQ usually sits on CPU 0 with
// the OS, not on the first isolated core).
static std::vector<int> readEnaIrqCpus(size_t online_count) {
    std::vector<int> cpus;
    std::ifstream f("/proc/interrupts");
    if (!f.is_open()) return cpus;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("ena") == std::string::npos && line.find("enp") == std::string::npos) continue;
        std::istringstream iss(line);
        std::string irqTok;
        iss >> irqTok;
        if (irqTok.empty() || irqTok.back() != ':') continue;
        irqTok.pop_back();
        std::ifstream af("/proc/irq/" + irqTok + "/smp_affinity_list");
        std::string al;
        if (!std::getline(af, al)) continue;
        std::vector<int> mask = parseCpuList(al);
        // An IRQ left at the default affinity spans every online CPU (the ENA admin
        // queue does this) and says nothing about placement. Counting it would
        // exclude every isolated core and collapse the pool, so only a strict
        // subset counts as a real pin.
        if (online_count && mask.size() >= online_count) continue;
        for (int c : mask) cpus.push_back(c);
    }
    return cpus;
}

void Replicator::initializeCpuCores() {
    // Pin each per-queue packet-processor thread to a dedicated CPU. Core
    // selection is dynamic so the same binary is correct from a c7i.4xlarge up to
    // bare metal, and never collides the poll thread with the ENA hard IRQ.
    //
    // Priority:
    //   1. REPLICATOR_CPUS env — explicit list ("2,3" or "2-5"). Full operator control.
    //   2. /sys/.../cpu/isolated MINUS its first entry. The AMI pins the ENA hard
    //      IRQ to the FIRST isolated CPU (bake-ami.sh ena-irq-affinity), so we skip
    //      it and place poll threads on the remaining isolated cores — one dedicated
    //      core per queue, none shared with the IRQ. Scales with the instance.
    //   3. Fallback (no isolation): cores 1..num_queues (avoid core 0 = OS/IRQ).
    int num_cores = std::thread::hardware_concurrency();
    std::cout << "Detected " << num_cores << " CPU cores" << std::endl;

    std::vector<int> pool;
    const char* env = getenv("REPLICATOR_CPUS");
    if (env && *env) {
        pool = parseCpuList(env);
        std::cout << "CPU pool from REPLICATOR_CPUS=\"" << env << "\"" << std::endl;
    }
    if (pool.empty()) {
        // isolated ∩ online, MINUS the cores actually servicing the ENA IRQ.
        // Previously this blindly skipped isolated[0] assuming the ENA IRQ was
        // pinned to the first isolated core — but the IRQ sits on CPU 0 with the
        // OS, so that wasted an isolated core and pushed the poll thread onto the
        // rtt recv core (collision). Skip the REAL IRQ cores + any offline core.
        std::vector<int> isol = readIsolatedCpus();
        std::vector<int> online = readOnlineCpus();
        std::vector<int> irq = readEnaIrqCpus(online.size());
        auto has = [](const std::vector<int>& v, int x) { for (int e : v) if (e == x) return true; return false; };
        for (int c : isol)
            if ((online.empty() || has(online, c)) && !has(irq, c)) pool.push_back(c);
        if (!pool.empty())
            std::cout << "CPU pool = isolated ∩ online − ENA-IRQ cores: " << pool.size() << " cores" << std::endl;
    }
    if (pool.empty()) {
        // Isolation exists but every isolated core looked like an IRQ core. Keep the
        // isolated set rather than dropping to the legacy fallback, which pins the
        // poll thread to a shared core and shows up as tail-latency outliers.
        std::vector<int> isol = readIsolatedCpus();
        std::vector<int> online = readOnlineCpus();
        for (int c : isol)
            if (online.empty() || std::find(online.begin(), online.end(), c) != online.end())
                pool.push_back(c);
        if (!pool.empty())
            std::cout << "CPU pool = isolated \u2229 online (ENA-IRQ exclusion would have emptied it): "
                      << pool.size() << " cores" << std::endl;
    }
    if (pool.empty()) {
        for (int i = 1; i <= num_queues_; ++i) pool.push_back(i);  // legacy fallback
        std::cout << "CPU pool: fallback cores 1.." << num_queues_ << " (no isolation)" << std::endl;
    }

    cpu_cores_.clear();
    cpu_cores_.reserve(num_queues_);
    for (int i = 0; i < num_queues_ && i < static_cast<int>(pool.size()); ++i) {
        if (pool[i] >= 0 && pool[i] < num_cores) cpu_cores_.push_back(pool[i]);
    }

    std::cout << "Assigned CPU cores for packet processing: ";
    for (int core : cpu_cores_) {
        std::cout << core << " ";
    }
    std::cout << std::endl;
}
