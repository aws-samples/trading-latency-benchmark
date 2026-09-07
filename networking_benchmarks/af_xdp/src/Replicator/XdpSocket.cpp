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

#include "XdpSocket.hpp"
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>          // poll() drives the AF_XDP busy-poll path in receive()
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>       // AF_XDP socket API (from xdp-tools)
#include <bpf/libbpf.h>   // BPF object loading + XDP attach (from system libbpf)
#include <bpf/bpf.h>      // bpf_xdp_attach/detach, bpf_map__fd
#include <iostream>
#include <algorithm>
#include <errno.h>

// Define constants for UMEM management - following ena-xdp example
#define TX_FRAMES 2048
#define RX_FRAMES 2048
#define UMEM_FRAMES (TX_FRAMES + RX_FRAMES)
#define UMEM_RX_FIRST_FRAME_IX TX_FRAMES
#define FRAME_SIZE 4096

// Debug print macro - DISABLED for performance
static int g_debug_enabled = 0;
#define DEBUG_PRINT(fmt, ...)                                  \
    do                                                         \
    {                                                          \
        if (__builtin_expect(g_debug_enabled, 0))                                                 \
            fprintf(stderr, "DEBUG CPP: " fmt, ##__VA_ARGS__); \
    } while (0)

static uint32_t opt_xdp_flags = XDP_FLAGS_DRV_MODE;
static bool opt_frags = true;
// Raw libbpf state (no libxdp dispatcher — direct attach/detach, no slot leaks)
static struct bpf_object *g_bpf_obj = NULL;
static int g_xdp_prog_fd = -1;
static int g_attached_ifindex = 0;

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
void XdpSocket::enableDebug(bool enable) {
    g_debug_enabled = enable;
    DEBUG_PRINT("Debug mode %s\n", enable ? "enabled" : "disabled");
}

void* XdpSocket::allocateAlignedBuffer(int size) {
    // Get page size
    long pageSize = sysconf(_SC_PAGESIZE);

    // Round up to the 2 MiB huge-page boundary so the UMEM can be backed by
    // explicit huge pages (reserved at boot via the hugepages= cmdline). A
    // TLB-resident UMEM removes per-packet TLB-miss jitter on the datapath.
    const size_t HUGE_2M = 2 * 1024 * 1024;
    size_t alignedSize = (size + HUGE_2M - 1) & ~(HUGE_2M - 1);

    void *buffer = MAP_FAILED;
#ifdef MAP_HUGETLB
    // Prefer reserved huge pages; fall back cleanly when none are available.
    buffer = mmap(NULL, alignedSize, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (buffer == MAP_FAILED)
    {
        DEBUG_PRINT("MAP_HUGETLB unavailable (%s); falling back to anon + MADV_HUGEPAGE\n",
                    strerror(errno));
    }
#endif
    if (buffer == MAP_FAILED)
    {
        buffer = mmap(NULL, alignedSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef MADV_HUGEPAGE
        if (buffer != MAP_FAILED)
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

void XdpSocket::freeAlignedBuffer(void* buffer, size_t size) {
    if (!buffer) {
        return;
    }

    // Use munmap for mmap-allocated memory. Match the 2 MiB rounding used at
    // allocation time so the whole mapping (huge or not) is released.
    const size_t HUGE_2M = 2 * 1024 * 1024;
    size_t alignedSize = (size + HUGE_2M - 1) & ~(HUGE_2M - 1);
    munmap(buffer, alignedSize);
    DEBUG_PRINT("Freed aligned buffer at %p, size %zu\n", buffer, alignedSize);
}

int XdpSocket::setResourceLimits() {
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

void XdpSocket::loadXdpProgram(const std::string& ifName, const std::string& programPath, bool nativeMode) {
    if (!nativeMode) {
        opt_xdp_flags = XDP_FLAGS_SKB_MODE;
    }

    DEBUG_PRINT("Loading the xdp program at path: %s\n", programPath.c_str());
    int ifindex = if_nametoindex(ifName.c_str());
    DEBUG_PRINT("ifindex: %i\n", ifindex);
    int err;

    // Clean up any existing XDP program on this interface
    if (g_bpf_obj != NULL) {
        bpf_xdp_detach(g_attached_ifindex, opt_xdp_flags, NULL);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        g_xdp_prog_fd = -1;
    }

    // Load BPF object file
    g_bpf_obj = bpf_object__open_file(programPath.c_str(), NULL);
    if (!g_bpf_obj || libbpf_get_error(g_bpf_obj)) {
        fprintf(stderr, "ERROR: program loading failed: %s\n", strerror(errno));
        g_bpf_obj = NULL;
        throw std::runtime_error("XDP program loading failed: " + programPath);
    }

    // Load all programs and maps into kernel
    err = bpf_object__load(g_bpf_obj);
    if (err) {
        fprintf(stderr, "ERROR: bpf_object__load failed: %s\n", strerror(-err));
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        throw std::runtime_error("BPF object load failed");
    }

    // Find the XDP program (first program in the object)
    struct bpf_program *prog = bpf_object__next_program(g_bpf_obj, NULL);
    if (!prog) {
        fprintf(stderr, "ERROR: no XDP program found in %s\n", programPath.c_str());
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        throw std::runtime_error("No XDP program found in BPF object");
    }
    g_xdp_prog_fd = bpf_program__fd(prog);

    // Attach XDP program directly (no dispatcher — clean detach guaranteed)
    err = bpf_xdp_attach(ifindex, g_xdp_prog_fd, opt_xdp_flags, NULL);
    if (err) {
        fprintf(stderr, "ERROR: XDP attach failed (native mode): %s\n", strerror(-err));
        // Fallback to SKB mode
        opt_xdp_flags = XDP_FLAGS_SKB_MODE;
        err = bpf_xdp_attach(ifindex, g_xdp_prog_fd, opt_xdp_flags, NULL);
        if (err) {
            fprintf(stderr, "ERROR: XDP attach failed (SKB fallback): %s\n", strerror(-err));
            bpf_object__close(g_bpf_obj);
            g_bpf_obj = NULL;
            g_xdp_prog_fd = -1;
            throw std::runtime_error("XDP program attach failed");
        }
    }
    g_attached_ifindex = ifindex;
    DEBUG_PRINT("Successfully attached XDP program: %s (flags=%u)\n", programPath.c_str(), opt_xdp_flags);
}

int XdpSocket::getXdpMapFd(const std::string& mapName) {
    if (!g_bpf_obj)
        return -1;
    struct bpf_map *map = bpf_object__find_map_by_name(g_bpf_obj, mapName.c_str());
    if (!map)
        return -1;
    return bpf_map__fd(map);
}

void XdpSocket::unloadXdpProgram(const std::string& ifName, bool nativeMode) {
    (void)ifName; // ifindex cached in g_attached_ifindex
    (void)nativeMode;

    if (g_attached_ifindex > 0) {
        bpf_xdp_detach(g_attached_ifindex, opt_xdp_flags, NULL);
        g_attached_ifindex = 0;
    }
    if (g_bpf_obj != NULL) {
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        g_xdp_prog_fd = -1;
    }
}

// Constructor
XdpSocket::XdpSocket(int frameSize, int frameCount, int headroom)
    : closed_(false), chunk_size_(frameSize), headroom_(headroom),
      tx_frames_(DEFAULT_TX_FRAMES), rx_frames_(DEFAULT_RX_FRAMES),
      outstanding_tx_(0) {
    
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
    
    DEBUG_PRINT("XdpSocket created: TX frames=%d, RX frames=%d, chunk_size=%d, buffer_size=%zu\n",
                tx_frames_, rx_frames_, chunk_size_, umem_buffer_size_);
}

// Move constructor
XdpSocket::XdpSocket(XdpSocket&& other) noexcept
    : wrapper_(std::move(other.wrapper_)),
      umem_buffer_(other.umem_buffer_),
      umem_buffer_size_(other.umem_buffer_size_),
      closed_(other.closed_.load()),
      chunk_size_(other.chunk_size_),
      headroom_(other.headroom_),
      tx_frames_(other.tx_frames_),
      rx_frames_(other.rx_frames_),
      outstanding_tx_(other.outstanding_tx_),
      registered_queue_id_(other.registered_queue_id_),
      pending_recycle_addrs_(std::move(other.pending_recycle_addrs_)) {
    other.umem_buffer_ = nullptr;
    other.umem_buffer_size_ = 0;
    other.closed_ = true;
}

// Move assignment operator
XdpSocket& XdpSocket::operator=(XdpSocket&& other) noexcept {
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
        outstanding_tx_ = other.outstanding_tx_;
        registered_queue_id_ = other.registered_queue_id_;
        pending_recycle_addrs_ = std::move(other.pending_recycle_addrs_);
        other.umem_buffer_ = nullptr;
        other.umem_buffer_size_ = 0;
        other.closed_ = true;
    }
    return *this;
}

// Destructor
XdpSocket::~XdpSocket() {
    close();
    if (umem_buffer_) {
        freeAlignedBuffer(umem_buffer_, umem_buffer_size_);
        umem_buffer_ = nullptr;
        umem_buffer_size_ = 0;
    }
}

uint8_t* XdpSocket::getUmemBuffer() {
    if (closed_.load()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(umem_buffer_);
}

size_t XdpSocket::getUmemBufferSize() const {
    return umem_buffer_size_;
}

int XdpSocket::setupUMem() {
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

int XdpSocket::bind(const std::string& ifName, int queueId, int flags) {
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

    // NAPI busy-poll on the XSK fd: the same poll that services RX also drains
    // the need_wakeup TX ring, which cuts IRQ->wakeup latency and makes the
    // sendto() TX kick rare. We KEEP the kick (requestDriverPoll) as a safety
    // net rather than removing it — busy-poll only reduces how often it fires,
    // it does not guarantee TX drain on every driver. Pairs with the
    // napi_defer_hard_irqs / gro_flush_timeout knobs set in provisioning.
    // Best-effort: unsupported options are simply ignored by the kernel.
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
    {
        int xfd = xsk_socket__fd(wrapper_->xsk);
        int on = 1, busy_us = 50, budget = 64;
        // Report results rather than discarding them: an inert SO_BUSY_POLL is
        // indistinguishable from a working one except by a ~10us per-packet
        // latency difference. receive()'s poll() is what drives the busy-poll
        // path these options enable.
        int r_pref = setsockopt(xfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on));
        int r_busy = setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
        int r_budg = setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
        int rb = 0; socklen_t rl = sizeof(rb);
        getsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL, &rb, &rl);
        std::cout << "XSK busy-poll (fd=" << xfd << "): SO_PREFER_BUSY_POLL="
                  << (r_pref == 0 ? "ok" : strerror(errno))
                  << " SO_BUSY_POLL(" << busy_us << "us)="
                  << (r_busy == 0 ? "ok" : strerror(errno))
                  << " SO_BUSY_POLL_BUDGET(" << budget << ")="
                  << (r_budg == 0 ? "ok" : strerror(errno))
                  << " readback=" << rb << "us" << std::endl;
    }

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

int XdpSocket::sendBatch(const std::vector<int>& offsets, const std::vector<int>& lengths, int batchSize) {
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

void XdpSocket::pollTxCompletions() {
    if (!outstanding_tx_) return;

    uint32_t idx = 0;
    uint32_t completed = xsk_ring_cons__peek(&wrapper_->cq, outstanding_tx_, &idx);
    if (!completed) return;

    // In-place forward (REPLICATOR_FWD_MODE=inplace) submits *RX-range* UMEM frames
    // to the TX ring. Those must be returned to the fill queue on completion (they
    // are borrowed RX buffers), whereas TX-pool frames are reused by ring index and
    // only need counting. Distinguish by UMEM address range — stateless, no tracking.
    const uint64_t rx_base = (uint64_t)UMEM_RX_FIRST_FRAME_IX * (uint64_t)chunk_size_;
    uint32_t rx_returned = 0, fq_idx = 0;
    for (uint32_t c = 0; c < completed; c++) {
        uint64_t caddr = *xsk_ring_cons__comp_addr(&wrapper_->cq, idx + c);
        if (caddr >= rx_base) {
            // Borrowed RX frame — hand it back to the fill queue.
            if (xsk_ring_prod__reserve(&wrapper_->fq, 1, &fq_idx) == 1) {
                *xsk_ring_prod__fill_addr(&wrapper_->fq, fq_idx) = caddr;
                xsk_ring_prod__submit(&wrapper_->fq, 1);
                rx_returned++;
            }
        }
    }

    xsk_ring_cons__release(&wrapper_->cq, completed);
    outstanding_tx_ = (completed <= outstanding_tx_) ? outstanding_tx_ - completed : 0;

    DEBUG_PRINT("Released %u TX completions (%u RX frames recycled), %u still outstanding\n",
                completed, rx_returned, outstanding_tx_);
}

void XdpSocket::requestDriverPoll() {
    // No checkOpen(): called from replicatePacket after the fan-out loop,
    // inside processPacketsForQueue's running_ guard.

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

int XdpSocket::reserveTxRing(int count, uint32_t* tx_idx) {
    // No checkOpen(): called only from sendSinglePacketDirect which is guarded
    // by sendToDestinationWithQueue's queueId/socket validity check.
    //
    // B5 — FRAME-SAFETY BACKPRESSURE. A successful xsk_ring_prod__reserve() only
    // means a DESCRIPTOR slot is free: the TX ring's consumer index advances when
    // the kernel DEQUEUES the descriptor, not when the NIC has finished DMA-ing the
    // buffer. The UMEM frame that sendSinglePacketDirect() derives from the ring
    // index — (tx_idx % TX_FRAMES) * FRAME_SIZE — is only safe to overwrite once
    // its COMPLETION has been reaped. Without this guard the producer can wrap onto
    // a frame that is still in flight and rewrite it mid-transmission, putting a
    // torn/inconsistent frame on the wire; AWS VPC then drops it silently, which
    // shows up as apparent packet "loss" that grows with run length and has no
    // local drop counter.
    //
    // The batched path (sendPackets) has always carried the equivalent ena-xdp
    // check `outstanding_tx_ > TX_FRAMES - TX_BATCH_SIZE`; it was never ported to
    // this single-packet path, which is the one the replicator hot path uses.
    // The guard must cover the WHOLE reservation: a fan-out reserves K slots at
    // once, so checking only that one frame is free lets a K-slot reserve wrap onto
    // frames still being DMA'd (e.g. 2040 outstanding + 24 reserved > 2048 frames),
    // which puts torn frames on the wire that AWS VPC drops without any local
    // counter moving.
    const uint32_t need = static_cast<uint32_t>(count);
    if (outstanding_tx_ + need > static_cast<uint32_t>(TX_FRAMES)) {
        pollTxCompletions();
        if (outstanding_tx_ + need > static_cast<uint32_t>(TX_FRAMES))
            return 0;   // caller kicks + retries, then falls back to the kernel socket
    }
    return xsk_ring_prod__reserve(&wrapper_->tx, count, tx_idx);
}

void XdpSocket::setTxDescriptor(uint32_t idx, uint64_t addr, uint32_t len) {
    // No checkOpen(): same caller-chain guarantee as reserveTxRing.
    struct xdp_desc* tx_desc = xsk_ring_prod__tx_desc(&wrapper_->tx, idx);
    tx_desc->addr = addr;
    tx_desc->len = len;
}

void XdpSocket::submitTxRing(int count) {
    // No checkOpen(): same caller-chain guarantee as reserveTxRing.
    // Submit TX ring and track outstanding packets (ena-xdp pattern)
    xsk_ring_prod__submit(&wrapper_->tx, count);
    outstanding_tx_ += count;
    DEBUG_PRINT("Submitted %d TX packets, outstanding_tx=%u\n", count, outstanding_tx_);
}

bool XdpSocket::forwardFrameInPlace(uint64_t rx_addr, uint32_t len) {
    // Release TX ring slots. RX-range frames (inplace mode) are returned to the fill queue.
    pollTxCompletions();

    // This RX frame is being handed to TX; make sure recycleFrames() does NOT also
    // return it to the fill queue (it comes back via the completion ring instead —
    // otherwise the same UMEM addr would sit in the fill queue twice → corruption).
    for (auto it = pending_recycle_addrs_.begin(); it != pending_recycle_addrs_.end(); ++it) {
        if (*it == rx_addr) { pending_recycle_addrs_.erase(it); break; }
    }

    uint32_t tx_idx = 0;
    if (xsk_ring_prod__reserve(&wrapper_->tx, 1, &tx_idx) != 1) {
        requestDriverPoll();
        pollTxCompletions();
        if (xsk_ring_prod__reserve(&wrapper_->tx, 1, &tx_idx) != 1)
            return false;  // ring full — caller may fall back to copy path
    }
    struct xdp_desc* d = xsk_ring_prod__tx_desc(&wrapper_->tx, tx_idx);
    d->addr = rx_addr;
    d->len  = len;
    xsk_ring_prod__submit(&wrapper_->tx, 1);
    outstanding_tx_++;
    return true;
}

int XdpSocket::receive(std::vector<int>& offsets, std::vector<int>& lengths) {
    checkOpen();

    // Get array length (max number of packets we can receive)
    size_t max_entries = std::min(offsets.size(), lengths.size());

    // Receive packets efficiently
    uint32_t idx_rx = 0;
    unsigned int received = xsk_ring_cons__peek(&wrapper_->rx, max_entries, &idx_rx);

    // Append new addresses to pending_recycle_addrs_ in-place.
    // Avoids heap allocation after warmup and preserves any addresses
    // retained from a prior partial recycleFrames() call.
    size_t base = pending_recycle_addrs_.size();
    pending_recycle_addrs_.resize(base + received);
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

            // Save address for recycling - this is critical!
            pending_recycle_addrs_[base + i] = addr;

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
        DEBUG_PRINT("Appended %u addresses for recycling (total pending=%zu)\n", received, pending_recycle_addrs_.size());
    }
    else
    {
        // App-driven busy-poll: run NAPI in *this* thread's context (the pinned
        // isolated CPU) so RX delivery does not wait on the deferred ENA hard IRQ
        // (napi_defer_hard_irqs / gro_flush_timeout) which fires on a different,
        // often-contended CPU.
        //
        // poll() is the entry point that drives this: xsk_poll() calls
        // sk_busy_loop() when SO_BUSY_POLL is active on the fd (set in
        // openSocket). A zero timeout makes it a single non-blocking pass, so the
        // caller's loop keeps spinning the ring. recvfrom() does NOT reliably
        // enter the busy-poll path on an XSK fd, so it is used strictly for the
        // fill-ring wakeup it is meant for.
        struct pollfd pfd{};
        pfd.fd     = xsk_socket__fd(wrapper_->xsk);
        pfd.events = POLLIN;
        poll(&pfd, 1, 0);
        if (xsk_ring_prod__needs_wakeup(&wrapper_->fq))
            recvfrom(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
    }

    return valid_packets;
}

void XdpSocket::recycleFrames() {
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

        // On full recycle clear the list; on partial, retain unrecycled addresses
        // so they are returned to the fill queue on the next recycleFrames() call.
        if (free_capacity >= pending_recycle_addrs_.size()) {
            pending_recycle_addrs_.clear();
        } else if (free_capacity > 0) {
            pending_recycle_addrs_.erase(pending_recycle_addrs_.begin(),
                                         pending_recycle_addrs_.begin() + free_capacity);
        }
        // free_capacity == 0: fill queue full, keep all pending for next call.
    }
}

int XdpSocket::getFd() {
    checkOpen();
    
    int fd = xsk_socket__fd(wrapper_->xsk);
    if (fd < 0) {
        throw std::runtime_error("Failed to get socket fd: " + std::string(strerror(-fd)));
    }
    return fd;
}

int XdpSocket::registerXskMap(int queueId) {
    checkOpen();
    
    if (!g_bpf_obj) {
        throw std::runtime_error("XDP program not loaded");
    }

    // Find the XSK map
    int xsks_map_fd = -1;
    struct bpf_map *map = bpf_object__find_map_by_name(g_bpf_obj, "xsks_map");
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
    registered_queue_id_ = queueId;
    return 0;
}

bool XdpSocket::isClosed() const {
    return closed_.load();
}

void XdpSocket::checkOpen() const {
    if (closed_.load()) {
        throw std::runtime_error("Socket is closed");
    }
}

void XdpSocket::close() {
    if (closed_.exchange(true) == false) {
        // Complete any pending TX (use class-level outstanding_tx_, not wrapper_->outstanding_tx
        // which is never updated and always reads 0)
        if (wrapper_ && wrapper_->xsk && outstanding_tx_ > 0) {
            DEBUG_PRINT("Completing %u outstanding TX packets before close\n", outstanding_tx_);

            int retries = 10;
            while (outstanding_tx_ > 0 && retries-- > 0) {
                uint32_t idx_cq = 0;
                unsigned int completed = xsk_ring_cons__peek(&wrapper_->cq, outstanding_tx_, &idx_cq);
                if (completed > 0) {
                    xsk_ring_cons__release(&wrapper_->cq, completed);
                    outstanding_tx_ = (completed <= outstanding_tx_) ? outstanding_tx_ - completed : 0;
                }
                if (outstanding_tx_ > 0) {
                    if (xsk_ring_prod__needs_wakeup(&wrapper_->tx))
                        sendto(xsk_socket__fd(wrapper_->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
                    usleep(1000);
                }
            }
        }

        if (wrapper_) {
            // Remove our entry from the XSK map if registered.
            // Use the stored key (registered_queue_id_) for a single O(1) delete
            // rather than scanning all 256 possible entries.
            if (wrapper_->xsk_map_fd >= 0 && registered_queue_id_ >= 0) {
                uint32_t key = static_cast<uint32_t>(registered_queue_id_);
                bpf_map_delete_elem(wrapper_->xsk_map_fd, &key);
                DEBUG_PRINT("Removed socket from XSK map at key %u\n", key);
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
