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

#include "AFXDPSocket.hpp"
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <iostream>
#include <algorithm>
#include <errno.h>

// Define constants for UMEM management - following ena-xdp example
#define TX_FRAMES 2048
#define RX_FRAMES 2048
#define UMEM_FRAMES (TX_FRAMES + RX_FRAMES)
#define UMEM_RX_FIRST_FRAME_IX TX_FRAMES
#define FRAME_SIZE 4096

// Debug print macro
static int g_debug_enabled = 0;
#define DEBUG_PRINT(fmt, ...)                                  \
    do                                                         \
    {                                                          \
        if (g_debug_enabled)                                   \
            fprintf(stderr, "DEBUG CPP: " fmt, ##__VA_ARGS__); \
    } while (0)

static enum xdp_attach_mode opt_attach_mode = XDP_MODE_NATIVE;
static bool opt_frags = true;
// Global variable to hold the XDP program
static struct xdp_program *xdp_prog = NULL;

// Wrapper for AF_XDP socket and related resources (same as JNI)
struct xsk_socket_wrapper
{
    struct xsk_socket *xsk;
    struct xsk_umem *umem;
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    void *umem_area;
    uint32_t umem_size;
    uint32_t chunk_size;
    uint32_t headroom;
    int xsk_map_fd;
    uint32_t outstanding_tx;
    int ifindex; // Store interface index for cleanup
};

// Static methods
void AFXDPSocket::enableDebug(bool enable) {
    g_debug_enabled = enable;
    DEBUG_PRINT("Debug mode %s\n", enable ? "enabled" : "disabled");
}

void* AFXDPSocket::allocateAlignedBuffer(int size) {
    // Get page size
    long pageSize = sysconf(_SC_PAGESIZE);

    // Round up size to page size
    size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

    // Use mmap instead of posix_memalign for better zero-copy performance
    void *buffer = mmap(NULL, alignedSize,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // Try to use huge pages if available
    if (buffer != MAP_FAILED)
    {
#ifdef MADV_HUGEPAGE
        madvise(buffer, alignedSize, MADV_HUGEPAGE);
#endif
    }

    if (buffer == MAP_FAILED)
    {
        throw std::runtime_error("Failed to allocate memory with mmap: " + std::string(strerror(errno)));
    }

    // Zero the buffer
    memset(buffer, 0, alignedSize);

    DEBUG_PRINT("Allocated aligned buffer at %p, size %zu, page size %ld\n",
                buffer, alignedSize, pageSize);

    return buffer;
}

void AFXDPSocket::freeAlignedBuffer(void* buffer, size_t size) {
    if (!buffer) {
        return;
    }

    // Use munmap for mmap-allocated memory
    munmap(buffer, size);
    DEBUG_PRINT("Freed aligned buffer at %p, size %zu\n", buffer, size);
}

int AFXDPSocket::setResourceLimits() {
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};

    DEBUG_PRINT("Setting RLIMIT_MEMLOCK to INFINITY\n");

    // Set RLIMIT_MEMLOCK to allow locking memory for zero-copy operations
    if (setrlimit(RLIMIT_MEMLOCK, &r))
    {
        int err = errno;
        DEBUG_PRINT("ERROR: setrlimit(RLIMIT_MEMLOCK) failed: %s (errno=%d)\n",
                    strerror(err), err);

        // Get current limits
        if (getrlimit(RLIMIT_MEMLOCK, &r) == 0)
        {
            DEBUG_PRINT("Current RLIMIT_MEMLOCK: soft=%lu, hard=%lu\n",
                        r.rlim_cur, r.rlim_max);
        }

        throw std::runtime_error("Failed to set RLIMIT_MEMLOCK: " + std::string(strerror(err)));
    }

    DEBUG_PRINT("RLIMIT_MEMLOCK set successfully\n");
    return 0;
}

void AFXDPSocket::loadXdpProgram(const std::string& ifName, const std::string& programPath, bool nativeMode) {
    if (!nativeMode) {
        opt_attach_mode = XDP_MODE_SKB;
    }

    DEBUG_PRINT("Loading the xdp program at path: %s\n", programPath.c_str());
    int ifindex = if_nametoindex(ifName.c_str());
    DEBUG_PRINT("ifindex: %i\n", ifindex);
    char errmsg[1024];
    int err;

    // Clean up any existing XDP programs
    if (xdp_prog != NULL)
    {
        xdp_program__detach(xdp_prog, ifindex, opt_attach_mode, 0);
        xdp_program__close(xdp_prog);
        xdp_prog = NULL;
    }

    // Load the XDP program
    xdp_prog = xdp_program__open_file(programPath.c_str(), NULL, NULL);
    err = libxdp_get_error(xdp_prog);
    if (err)
    {
        libxdp_strerror(err, errmsg, sizeof(errmsg));
        fprintf(stderr, "ERROR: program loading failed: %s\n", errmsg);
        throw std::runtime_error("XDP program loading failed: " + std::string(errmsg));
    }
    err = xdp_program__set_xdp_frags_support(xdp_prog, opt_frags);
    if (err)
    {
        libxdp_strerror(err, errmsg, sizeof(errmsg));
        fprintf(stderr, "ERROR: Enable frags support failed: %s\n", errmsg);
        throw std::runtime_error("Enable frags support failed: " + std::string(errmsg));
    }
    // Attach the XDP program to the interface
    err = xdp_program__attach(xdp_prog, ifindex, opt_attach_mode, 0);
    if (err)
    {
        libxdp_strerror(err, errmsg, sizeof(errmsg));
        fprintf(stderr, "ERROR: attaching program failed: %s\n", errmsg);
        xdp_program__close(xdp_prog);
        xdp_prog = NULL;
        throw std::runtime_error("XDP program attach failed: " + std::string(errmsg));
    }
    else
    {
        DEBUG_PRINT("Successfully loaded the program: %s\n", programPath.c_str());
    }
}

void AFXDPSocket::unloadXdpProgram(const std::string& ifName, bool nativeMode) {
    if (!nativeMode) {
        opt_attach_mode = XDP_MODE_SKB;
    }
    int ifindex = if_nametoindex(ifName.c_str());

    if (xdp_prog != NULL)
    {
        xdp_program__detach(xdp_prog, ifindex, opt_attach_mode, 0);
        xdp_program__close(xdp_prog);
        xdp_prog = NULL;
    }
}

// Constructor
AFXDPSocket::AFXDPSocket(int frameSize, int frameCount, int headroom)
    : closed_(false), chunk_size_(frameSize), headroom_(headroom),
      tx_frames_(DEFAULT_TX_FRAMES), rx_frames_(DEFAULT_RX_FRAMES),
      prev_umem_tx_frame_(0), cached_completions_(0), outstanding_tx_(0) {
    
    wrapper_ = std::make_unique<xsk_socket_wrapper>();
    memset(wrapper_.get(), 0, sizeof(xsk_socket_wrapper));
    wrapper_->xsk_map_fd = -1;
    wrapper_->ifindex = -1;
    wrapper_->outstanding_tx = 0;

    // Verify frame size is power of 2
    if ((frameSize & (frameSize - 1)) != 0) {
        throw std::invalid_argument("Frame size must be a power of 2");
    }

    // Ensure frameCount accommodates both TX and RX frames (following ena-xdp)
    int required_frames = tx_frames_ + rx_frames_;
    if (frameCount < required_frames) {
        std::cout << "Warning: frameCount " << frameCount << " too small, using " << required_frames << std::endl;
        frameCount = required_frames;
    }

    // Allocate UMEM - ensure it's large enough for both TX and RX frames
    umem_buffer_size_ = static_cast<size_t>(frameSize) * frameCount;
    
    // Use page-aligned allocation for AF_XDP compatibility
    umem_buffer_ = allocateAlignedBuffer(umem_buffer_size_);
    
    DEBUG_PRINT("AFXDPSocket created: TX frames=%d, RX frames=%d, chunk_size=%d, buffer_size=%zu\n",
                tx_frames_, rx_frames_, chunk_size_, umem_buffer_size_);
}

// Move constructor
AFXDPSocket::AFXDPSocket(AFXDPSocket&& other) noexcept
    : wrapper_(std::move(other.wrapper_)),
      umem_buffer_(other.umem_buffer_),
      umem_buffer_size_(other.umem_buffer_size_),
      closed_(other.closed_.load()),
      chunk_size_(other.chunk_size_),
      headroom_(other.headroom_),
      tx_frames_(other.tx_frames_),
      rx_frames_(other.rx_frames_),
      prev_umem_tx_frame_(other.prev_umem_tx_frame_.load()),
      cached_completions_(other.cached_completions_),
      outstanding_tx_(other.outstanding_tx_),
      pending_recycle_addrs_(std::move(other.pending_recycle_addrs_)) {
    other.umem_buffer_ = nullptr;
    other.umem_buffer_size_ = 0;
    other.closed_ = true;
}

// Move assignment operator
AFXDPSocket& AFXDPSocket::operator=(AFXDPSocket&& other) noexcept {
    if (this != &other) {
        close();
        wrapper_ = std::move(other.wrapper_);
        umem_buffer_ = other.umem_buffer_;
        umem_buffer_size_ = other.umem_buffer_size_;
        closed_ = other.closed_.load();
        chunk_size_ = other.chunk_size_;
        headroom_ = other.headroom_;
        tx_frames_ = other.tx_frames_;
        rx_frames_ = other.rx_frames_;
        prev_umem_tx_frame_ = other.prev_umem_tx_frame_.load();
        cached_completions_ = other.cached_completions_;
        outstanding_tx_ = other.outstanding_tx_;
        pending_recycle_addrs_ = std::move(other.pending_recycle_addrs_);
        other.umem_buffer_ = nullptr;
        other.umem_buffer_size_ = 0;
        other.closed_ = true;
    }
    return *this;
}

// Destructor
AFXDPSocket::~AFXDPSocket() {
    close();
    if (umem_buffer_) {
        freeAlignedBuffer(umem_buffer_, umem_buffer_size_);
        umem_buffer_ = nullptr;
        umem_buffer_size_ = 0;
    }
}

uint8_t* AFXDPSocket::getUmemBuffer() {
    if (closed_.load()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(umem_buffer_);
}

size_t AFXDPSocket::getUmemBufferSize() const {
    return umem_buffer_size_;
}

int AFXDPSocket::setupUMem() {
    checkOpen();

    if (!umem_buffer_) {
        throw std::invalid_argument("UMEM buffer must be allocated");
    }

    wrapper_->umem_area = umem_buffer_;
    wrapper_->umem_size = umem_buffer_size_;
    wrapper_->chunk_size = chunk_size_;
    wrapper_->headroom = headroom_;

    // Check page alignment
    long pageSize = sysconf(_SC_PAGESIZE);
    if ((uintptr_t)wrapper_->umem_area % pageSize != 0) {
        throw std::invalid_argument("Buffer must be page-aligned for AF_XDP");
    }

    // Check minimum size - ensure we have enough space for both TX and RX frames
    if (wrapper_->umem_size < UMEM_FRAMES * wrapper_->chunk_size) {
        throw std::invalid_argument("Buffer size too small for AF_XDP - need space for both TX and RX frames");
    }

    // Create UMEM with configuration similar to ena-xdp example
    struct xsk_umem_config umem_cfg = {
        .fill_size = RX_FRAMES * 2,  // Double the size for better performance
        .comp_size = TX_FRAMES * 2,  // Double the size for better performance
        .frame_size = wrapper_->chunk_size,
        .frame_headroom = wrapper_->headroom,
        .flags = 0,
    };

    int ret = xsk_umem__create(&wrapper_->umem, wrapper_->umem_area, wrapper_->umem_size,
                               &wrapper_->fq, &wrapper_->cq, &umem_cfg);

    if (ret) {
        throw std::runtime_error("Failed to create AF_XDP UMEM: " + std::string(strerror(-ret)));
    }

    DEBUG_PRINT("UMEM setup - Address: %p, Size: %u bytes, Chunk size: %u, Headroom: %u\n",
                wrapper_->umem_area, wrapper_->umem_size, wrapper_->chunk_size, wrapper_->headroom);
    DEBUG_PRINT("UMEM configuration: TX frames: %d, RX frames: %d, Total frames: %d\n",
                TX_FRAMES, RX_FRAMES, UMEM_FRAMES);

    return 0;
}

int AFXDPSocket::bind(const std::string& ifName, int queueId, int flags) {
    checkOpen();

    if (!wrapper_->umem) {
        throw std::runtime_error("UMEM not configured - call setupUMem first");
    }

    // Get interface index
    int ifindex = if_nametoindex(ifName.c_str());
    if (ifindex == 0) {
        throw std::invalid_argument("Invalid interface name");
    }

    // Store the interface index for cleanup
    wrapper_->ifindex = ifindex;

    // Determine XDP flags based on mode
    uint32_t xdp_flags = 0;
    uint32_t bind_flags = 0;

    switch (flags) {
    case XDP_FLAGS_SKB_MODE: // SKB mode
        xdp_flags |= XDP_FLAGS_SKB_MODE;
        bind_flags |= XDP_COPY;
        DEBUG_PRINT("Using SKB mode (XDP_COPY)\n");
        break;
    case XDP_FLAGS_DRV_MODE: // Driver mode
        xdp_flags |= XDP_FLAGS_DRV_MODE;
        DEBUG_PRINT("Using driver mode (XDP_DRV_MODE)\n");
        break;
    case XDP_FLAGS_HW_MODE: // Hardware mode
        xdp_flags |= XDP_FLAGS_HW_MODE;
        DEBUG_PRINT("Using hardware mode (XDP_HW_MODE)\n");
        break;
    case XDP_FLAGS_ZERO_COPY: // Zero-copy mode
        xdp_flags |= XDP_FLAGS_DRV_MODE;
        bind_flags |= XDP_ZEROCOPY;
        DEBUG_PRINT("Attempting zero-copy mode (XDP_ZEROCOPY with DRV_MODE)\n");
        break;
    default:
        throw std::invalid_argument("Invalid XDP mode");
    }

    // Add wakeup flag for better performance
    bind_flags |= XDP_USE_NEED_WAKEUP;

    // Configure socket - use RX_FRAMES and TX_FRAMES from ena-xdp example
    struct xsk_socket_config xsk_cfg = {
        .rx_size = RX_FRAMES,
        .tx_size = TX_FRAMES,
        .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,  // Critical: Prevent loading XDP program multiple times
        .xdp_flags = 0,  // CRITICAL FIX: Don't set xdp_flags when inhibiting program load
        .bind_flags = static_cast<__u16>(bind_flags),
    };

    // Create AF_XDP socket
    int ret = xsk_socket__create(&wrapper_->xsk, ifName.c_str(), queueId, wrapper_->umem,
                                 &wrapper_->rx, &wrapper_->tx, &xsk_cfg);
    if (ret) {
        if (bind_flags & XDP_ZEROCOPY) {
            DEBUG_PRINT("Zero-copy mode failed with error %d: %s\n", ret, strerror(abs(ret)));
            DEBUG_PRINT("This network interface or driver likely doesn't support zero-copy mode\n");
        }
        throw std::runtime_error("Failed to create AF_XDP socket: " + std::string(strerror(-ret)));
    }
    DEBUG_PRINT("AF_XDP socket bound successfully to %s queue %d\n", ifName.c_str(), queueId);
    DEBUG_PRINT("Socket configuration: RX size: %d, TX size: %d\n", RX_FRAMES, TX_FRAMES);

    // Populate the fill queue
    uint32_t idx;
    DEBUG_PRINT("Populating fill queue with %u RX frames (reserving %u for TX)\n", 
                RX_FRAMES, TX_FRAMES);

    // Reserve space in the fill queue - do it all at once like in the example code
    uint32_t reserved = xsk_ring_prod__reserve(&wrapper_->fq, RX_FRAMES, &idx);
    if (reserved != RX_FRAMES) {
        DEBUG_PRINT("Warning: Could only reserve %u out of %u frames\n", reserved, RX_FRAMES);
    }

    // Add only RX frames to the fill queue, starting after the TX frames
    for (uint32_t i = 0; i < reserved; i++) {
        uint64_t addr = (UMEM_RX_FIRST_FRAME_IX + i) * wrapper_->chunk_size;
        *xsk_ring_prod__fill_addr(&wrapper_->fq, idx++) = addr;
    }

    xsk_ring_prod__submit(&wrapper_->fq, reserved);
    DEBUG_PRINT("Added %u frames to fill queue\n", reserved);

    return 0;
}

int AFXDPSocket::send(int offset, int length) {
    checkOpen();
    
    std::vector<int> offsets = {offset};
    std::vector<int> lengths = {length};
    return sendBatch(offsets, lengths, 1);
}

int AFXDPSocket::sendBatch(const std::vector<int>& offsets, const std::vector<int>& lengths, int batchSize) {
    checkOpen();

    if (offsets.size() < static_cast<size_t>(batchSize) || lengths.size() < static_cast<size_t>(batchSize)) {
        throw std::invalid_argument("Vectors must be at least as long as batchSize");
    }

    // Use the new batched completion handling (following ena-xdp)
    pollTxCompletions();

    // Don't enqueue new packets if we can't enqueue a full batch (ena-xdp approach)
    if (outstanding_tx_ > (TX_FRAMES - TX_BATCH_SIZE)) {
        DEBUG_PRINT("TX ring too full, outstanding_tx=%u\n", outstanding_tx_);
        return 0;  // TX ring too full
    }

    // Ensure we don't exceed batch size
    batchSize = std::min(batchSize, TX_BATCH_SIZE);
    if (batchSize <= 0) {
        return 0;
    }

    // Try to reserve space in the TX ring (following ena-xdp pattern)
    uint32_t tx_idx = 0;
    int ret = xsk_ring_prod__reserve(&wrapper_->tx, batchSize, &tx_idx);

    if (ret != batchSize) {
        // CRITICAL: Handle zero-copy vs copy mode differently (ena-xdp approach)
        if (ret == 0) {
            // Request driver poll before giving up (ena-xdp pattern)
            requestDriverPoll();
            return 0;
        }
        // Use what we could get
        batchSize = ret;
    }

    // Fill descriptors with packet data (following ena-xdp exactly)
    for (int i = 0; i < batchSize; i++) {
        xsk_ring_prod__tx_desc(&wrapper_->tx, tx_idx + i)->addr = offsets[i];
        xsk_ring_prod__tx_desc(&wrapper_->tx, tx_idx + i)->len = lengths[i];
    }

    // Submit packets for transmission
    xsk_ring_prod__submit(&wrapper_->tx, batchSize);
    outstanding_tx_ += batchSize;

    // CRITICAL: Always request driver poll after submit (ena-xdp requirement)
    requestDriverPoll();

    DEBUG_PRINT("Sent batch of %d packets, outstanding_tx=%u\n", batchSize, outstanding_tx_);
    return batchSize;
}

int AFXDPSocket::sendBatchToSubscribers(const std::vector<int>& offsets, const std::vector<int>& lengths, int batchSize) {
    // This is a specialized version of sendBatch for subscriber forwarding
    return sendBatch(offsets, lengths, batchSize);
}

int AFXDPSocket::getNextTxFrame() {
    // CRITICAL FIX: Return frame number, not address (following ena-xdp exactly)
    // The address calculation happens when setting TX descriptor
    uint32_t frame_nb = prev_umem_tx_frame_.fetch_add(1) % tx_frames_;
    return frame_nb;  // Return frame number, not multiplied address
}

void AFXDPSocket::pollTxCompletions() {
    checkOpen();
    
    if (!outstanding_tx_) {
        return;
    }
    
    // Poll for new completions (following ena-xdp batching strategy)
    uint32_t idx = 0;
    uint32_t new_completions = xsk_ring_cons__peek(&wrapper_->cq, tx_frames_, &idx);
    
    if (!new_completions) {
        return;
    }
    
    // Add to cached completions for batching
    cached_completions_ += new_completions;
    
    // Only release when we have a full batch (following ena-xdp pattern)
    if (cached_completions_ < TX_BATCH_SIZE) {
        return;
    }
    
    // Release the cached completions
    xsk_ring_cons__release(&wrapper_->cq, cached_completions_);
    outstanding_tx_ -= cached_completions_;
    
    DEBUG_PRINT("Released %u TX completions, %u still outstanding\n", 
                cached_completions_, outstanding_tx_);
    
    cached_completions_ = 0;
}

void AFXDPSocket::requestDriverPoll() {
    checkOpen();
    
    // CRITICAL: This is the missing piece! Following ena-xdp exactly
    // Check if we need to wake up the driver (only if using XDP_USE_NEED_WAKEUP)
    if (!xsk_ring_prod__needs_wakeup(&wrapper_->tx)) {
        return;  // Driver doesn't need wakeup
    }
    
    // Wake up the driver by sending empty packet (ena-xdp pattern)
    int ret = sendto(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
    
    if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN || 
        errno == EBUSY || errno == ENETDOWN) {
        // These are expected/acceptable errors
        DEBUG_PRINT("Driver wakeup sent successfully or acceptable error\n");
        return;
    }
    
    // Log unexpected errors but don't fail
    DEBUG_PRINT("Driver wakeup sendto error: %s (errno=%d)\n", strerror(errno), errno);
}

int AFXDPSocket::reserveTxRing(int count, uint32_t* tx_idx) {
    checkOpen();
    // Direct wrapper around xsk_ring_prod__reserve (ena-xdp pattern)
    return xsk_ring_prod__reserve(&wrapper_->tx, count, tx_idx);
}

void AFXDPSocket::setTxDescriptor(uint32_t idx, uint64_t addr, uint32_t len) {
    checkOpen();
    // Direct TX descriptor access (ena-xdp pattern)
    struct xdp_desc* tx_desc = xsk_ring_prod__tx_desc(&wrapper_->tx, idx);
    tx_desc->addr = addr;
    tx_desc->len = len;
}

void AFXDPSocket::submitTxRing(int count) {
    checkOpen();
    // Submit TX ring and track outstanding packets (ena-xdp pattern)
    xsk_ring_prod__submit(&wrapper_->tx, count);
    outstanding_tx_ += count;
    DEBUG_PRINT("Submitted %d TX packets, outstanding_tx=%u\n", count, outstanding_tx_);
}

void AFXDPSocket::copyUmemData(const uint8_t* sourceBuffer, int sourceOffset, int destOffset, int length) {
    checkOpen();
    
    if (!sourceBuffer) {
        throw std::invalid_argument("Source buffer must not be null");
    }

    // Copy data from source buffer to this UMEM
    const uint8_t* src = sourceBuffer + sourceOffset;
    uint8_t* dst = static_cast<uint8_t*>(umem_buffer_) + destOffset;

    // Do the copy
    memcpy(dst, src, length);
}

int AFXDPSocket::receive(std::vector<int>& offsets, std::vector<int>& lengths) {
    checkOpen();

    // Get array length (max number of packets we can receive)
    size_t max_entries = std::min(offsets.size(), lengths.size());

    // Receive packets efficiently
    uint32_t idx_rx = 0;
    unsigned int received = xsk_ring_cons__peek(&wrapper_->rx, max_entries, &idx_rx);

    // Track packet addresses for recycling - critical for proper operation
    std::vector<uint64_t> recycle_addrs(received);
    int valid_packets = 0;

    if (received > 0)
    {
        DEBUG_PRINT("Received %u packets from RX ring\n", received);

        // Process all received packets - filtering happens in XDP program
        for (unsigned int i = 0; i < received; i++)
        {
            const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&wrapper_->rx, idx_rx++);
            uint64_t addr = desc->addr;
            uint32_t len = desc->len;
            
            // Debug - inspect packet to check if it's UDP
            uint8_t *pkt_data = static_cast<uint8_t*>(wrapper_->umem_area) + xsk_umem__extract_addr(addr);

            // Check if this is an Ethernet IPv4 packet
            if (len >= 34 && pkt_data[12] == 0x08 && pkt_data[13] == 0x00)
            {
                // Extract IP header
                uint8_t *ip_hdr = pkt_data + 14;
                uint8_t ip_proto = ip_hdr[9];

                // Check if it's UDP (protocol 17)
                if (ip_proto == 17)
                {
                    // Calculate IP header length to find UDP header
                    uint8_t ip_hdr_len = (ip_hdr[0] & 0x0F) * 4;
                    uint8_t *udp_hdr = ip_hdr + ip_hdr_len;

                    // Extract UDP ports
                    uint16_t src_port = (udp_hdr[0] << 8) | udp_hdr[1];
                    uint16_t dst_port = (udp_hdr[2] << 8) | udp_hdr[3];

                    // Extract source and destination IP for better debugging
                    uint8_t *saddr = ip_hdr + 12;
                    uint8_t *daddr = ip_hdr + 16;
                    
                    DEBUG_PRINT("UDP packet received: %d.%d.%d.%d:%u -> %d.%d.%d.%d:%u, len=%u\n",
                                saddr[0], saddr[1], saddr[2], saddr[3], src_port,
                                daddr[0], daddr[1], daddr[2], daddr[3], dst_port, len);
                }
            }

            // Save address for recycling - this is critical!
            recycle_addrs[i] = addr;

            // Pass packet info to caller
            if (valid_packets < static_cast<int>(max_entries)) {
                offsets[valid_packets] = xsk_umem__extract_addr(addr);
                lengths[valid_packets] = len;
                valid_packets++;
            }
        }

        // Release processed entries in RX ring - following ena-xdp example
        xsk_ring_cons__release(&wrapper_->rx, received);
        DEBUG_PRINT("Released %u packets from RX ring, valid_packets=%d\n", received, valid_packets);

        // Store the recycle addresses for later recycling
        pending_recycle_addrs_ = std::move(recycle_addrs);
        DEBUG_PRINT("Stored %u addresses for later recycling\n", received);
    }
    else
    {
        // No packets received, check if we need to wake up the fill ring
        if (xsk_ring_prod__needs_wakeup(&wrapper_->fq))
        {
            recvfrom(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
        }
    }

    return valid_packets;
}

void AFXDPSocket::recycleFrames() {
    checkOpen();
    
    if (!pending_recycle_addrs_.empty()) {
        DEBUG_PRINT("Recycling %zu frames\n", pending_recycle_addrs_.size());

        // Recycle frames back to fill queue - following ena-xdp example
        uint32_t idx_fq = 0;
        size_t free_capacity = xsk_ring_prod__reserve(&wrapper_->fq, pending_recycle_addrs_.size(), &idx_fq);

        if (free_capacity > 0) {
            for (size_t i = 0; i < free_capacity; i++) {
                uint64_t addr = xsk_umem__extract_addr(pending_recycle_addrs_[i]);
                DEBUG_PRINT("Recycling frame at address 0x%lx (frame %lu)\n", 
                           addr, addr / wrapper_->chunk_size);
                *xsk_ring_prod__fill_addr(&wrapper_->fq, idx_fq++) = addr;
            }

            xsk_ring_prod__submit(&wrapper_->fq, free_capacity);
            DEBUG_PRINT("Successfully recycled %zu frames out of %zu requested\n", free_capacity, pending_recycle_addrs_.size());

            // Wake up fill ring if needed
            if (xsk_ring_prod__needs_wakeup(&wrapper_->fq)) {
                recvfrom(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
                DEBUG_PRINT("Woke up fill ring\n");
            }
        } else {
            DEBUG_PRINT("Warning: Failed to recycle frames - fill queue is full\n");
        }
        
        pending_recycle_addrs_.clear();
    }
}

int AFXDPSocket::getFd() {
    checkOpen();
    
    int fd = xsk_socket__fd(wrapper_->xsk);
    if (fd < 0) {
        throw std::runtime_error("Failed to get socket fd: " + std::string(strerror(-fd)));
    }
    return fd;
}

int AFXDPSocket::registerXskMap(int queueId) {
    checkOpen();
    
    if (!xdp_prog) {
        throw std::runtime_error("XDP program not loaded");
    }

    // Find the XSK map
    int xsks_map_fd = -1;
    struct bpf_object *bpf_obj = xdp_program__bpf_obj(xdp_prog);

    if (!bpf_obj) {
        throw std::runtime_error("Failed to get BPF object from XDP program");
    }
    
    // Try direct map lookup first
    DEBUG_PRINT("Looking for maps in XDP program...\n");
    struct bpf_map *map = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
    if (map) {
        xsks_map_fd = bpf_map__fd(map);
        DEBUG_PRINT("Found XSK map 'xsks_map' with fd: %d\n", xsks_map_fd);
    } else {
        throw std::runtime_error("Failed to find XSK map");
    }

    // Store the map fd for future use
    wrapper_->xsk_map_fd = xsks_map_fd;

    // Get socket's fd and validate it
    int sock_fd = xsk_socket__fd(wrapper_->xsk);
    if (sock_fd < 0) {
        throw std::runtime_error("Invalid socket file descriptor");
    }
    DEBUG_PRINT("Using socket fd %d for queue %d\n", sock_fd, queueId);

    // Use the queue_id as the key
    uint32_t key = queueId;

    // Update the map with our socket fd
    int ret = bpf_map_update_elem(xsks_map_fd, &key, &sock_fd, 0);
    if (ret) {
        ret = -errno;
        fprintf(stderr, "ERROR: Failed to update XSK map: %s (errno=%d)\n",
                strerror(abs(ret)), abs(ret));
        throw std::runtime_error("Failed to update XSK map: " + std::string(strerror(abs(ret))));
    }

    fprintf(stderr, "Successfully registered AF_XDP socket with XSK map (key=%u)\n", key);
    return 0;
}

bool AFXDPSocket::isClosed() const {
    return closed_.load();
}

void AFXDPSocket::checkOpen() const {
    if (closed_.load()) {
        throw std::runtime_error("Socket is closed");
    }
}

void AFXDPSocket::close() {
    if (closed_.exchange(true) == false) {
        // Complete any pending TX
        if (wrapper_ && wrapper_->xsk && wrapper_->outstanding_tx > 0) {
            DEBUG_PRINT("Completing %u outstanding TX packets before close\n",
                        wrapper_->outstanding_tx);

            // Try to complete outstanding TX
            int retries = 10;
            while (wrapper_->outstanding_tx > 0 && retries-- > 0) {
                uint32_t idx_cq = 0;
                unsigned int completed = xsk_ring_cons__peek(&wrapper_->cq,
                                                             wrapper_->outstanding_tx, &idx_cq);
                if (completed > 0) {
                    xsk_ring_cons__release(&wrapper_->cq, completed);
                    wrapper_->outstanding_tx -= completed;
                }

                if (wrapper_->outstanding_tx > 0) {
                    if (xsk_ring_prod__needs_wakeup(&wrapper_->tx)) {
                        sendto(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                    }
                    usleep(1000); // Short delay before retry
                }
            }
        }

        if (wrapper_) {
            // Remove our entry from the XSK map if registered
            if (wrapper_->xsk_map_fd >= 0) {
                // Try to clean up map entry for our queue
                for (uint32_t i = 0; i < 256; i++) {
                    int sock_fd = 0;
                    if (bpf_map_lookup_elem(wrapper_->xsk_map_fd, &i, &sock_fd) == 0) {
                        if (wrapper_->xsk && sock_fd == xsk_socket__fd(wrapper_->xsk)) {
                            bpf_map_delete_elem(wrapper_->xsk_map_fd, &i);
                            DEBUG_PRINT("Removed socket from XSK map at index %u\n", i);
                        }
                    }
                }
            }

            // Clean up socket and UMEM
            if (wrapper_->xsk) {
                xsk_socket__delete(wrapper_->xsk);
                wrapper_->xsk = nullptr;
            }

            if (wrapper_->umem) {
                xsk_umem__delete(wrapper_->umem);
                wrapper_->umem = nullptr;
            }
        }
    }
}
