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

#ifndef AFXDP_SOCKET_HPP
#define AFXDP_SOCKET_HPP

#include <memory>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <cstdint>

// Forward declarations
struct xsk_socket_wrapper;

/**
 * C++ wrapper for AF_XDP socket with true zero-copy support
 */
class AFXDPSocket {
public:
    // XDP flags (same as Java version)
    static constexpr int XDP_FLAGS_UPDATE_IF_NOEXIST = 1;
    static constexpr int XDP_FLAGS_SKB_MODE = 2;
    static constexpr int XDP_FLAGS_DRV_MODE = 4;
    static constexpr int XDP_FLAGS_HW_MODE = 8;
    static constexpr int XDP_FLAGS_ZERO_COPY = 16;

    // Frame management constants (following ena-xdp best practices)
    static constexpr int TX_BATCH_SIZE = 64;
    static constexpr int DEFAULT_TX_FRAMES = 2048;
    static constexpr int DEFAULT_RX_FRAMES = 2048;
    static constexpr int DEFAULT_UMEM_FRAMES = DEFAULT_TX_FRAMES + DEFAULT_RX_FRAMES;
    static constexpr int UMEM_RX_FIRST_FRAME_IX = DEFAULT_TX_FRAMES;

private:
    std::unique_ptr<xsk_socket_wrapper> wrapper_;
    void* umem_buffer_;
    size_t umem_buffer_size_;
    std::atomic<bool> closed_;
    int chunk_size_;
    int headroom_;
    
    // Frame management (following ena-xdp best practices)
    int tx_frames_;
    int rx_frames_;
    std::atomic<uint32_t> prev_umem_tx_frame_;  // Track TX frame cycling
    uint32_t cached_completions_;               // Batch completions
    uint32_t outstanding_tx_;                   // Track pending TX
    
    // Field to store addresses pending recycling
    std::vector<uint64_t> pending_recycle_addrs_;

public:
    /**
     * Enable or disable debug output
     * 
     * @param enable True to enable debug output, false to disable
     */
    static void enableDebug(bool enable);

    /**
     * Allocate a page-aligned buffer for use with AF_XDP
     * 
     * @param size Buffer size in bytes
     * @return Pointer to aligned memory
     */
    static void* allocateAlignedBuffer(int size);

    /**
     * Free a page-aligned buffer allocated with allocateAlignedBuffer
     * 
     * @param buffer Buffer to free
     */
    static void freeAlignedBuffer(void* buffer, size_t size);

    /**
     * Create a new AF_XDP socket with specified parameters
     * 
     * @param frameSize  Size of each frame in the UMEM (must be power of 2)
     * @param frameCount Number of frames in the UMEM
     * @param headroom   Headroom for each frame
     * @throws std::runtime_error If allocation fails
     */
    AFXDPSocket(int frameSize = 4096, int frameCount = 4096, int headroom = 0);

    /**
     * Destructor
     */
    ~AFXDPSocket();

    // Copy constructor and assignment operator are deleted
    AFXDPSocket(const AFXDPSocket&) = delete;
    AFXDPSocket& operator=(const AFXDPSocket&) = delete;

    // Move constructor and assignment operator
    AFXDPSocket(AFXDPSocket&& other) noexcept;
    AFXDPSocket& operator=(AFXDPSocket&& other) noexcept;

    /**
     * Get direct access to the UMEM buffer
     * 
     * @return Pointer to UMEM buffer
     */
    uint8_t* getUmemBuffer();

    /**
     * Get UMEM buffer size
     * 
     * @return Buffer size in bytes
     */
    size_t getUmemBufferSize() const;

    /**
     * Set up the UMEM area for the socket
     * 
     * @return 0 on success, negative error code on failure
     * @throws std::runtime_error If UMEM setup fails
     */
    int setupUMem();

    /**
     * Bind the socket to a network interface
     *
     * @param ifName  Interface name (e.g., eth0)
     * @param queueId Queue ID
     * @param flags   XDP flags (e.g., XDP_FLAGS_SKB_MODE)
     * @return 0 on success, negative error code on failure
     * @throws std::runtime_error If binding fails
     */
    int bind(const std::string& ifName, int queueId, int flags);

    /**
     * Send a packet
     * 
     * @param offset Offset in the UMEM
     * @param length Length of the packet
     * @return 0 on success, negative error code on failure
     * @throws std::runtime_error If sending fails
     */
    int send(int offset, int length);

    /**
     * Send multiple packets in a batch
     * 
     * @param offsets   Vector of packet offsets in the UMEM
     * @param lengths   Vector of packet lengths
     * @param batchSize Number of packets to send (must be <= vectors size)
     * @return Number of packets queued for transmission, or negative error code
     * @throws std::runtime_error If sending fails
     */
    int sendBatch(const std::vector<int>& offsets, const std::vector<int>& lengths, int batchSize);

    /**
     * Send packets to subscribers with zero-copy
     * 
     * @param offsets   Vector of packet offsets in UMEM
     * @param lengths   Vector of packet lengths
     * @param batchSize Number of packets to send
     * @return Number of packets sent
     * @throws std::runtime_error If sending fails
     */
    int sendBatchToSubscribers(const std::vector<int>& offsets, const std::vector<int>& lengths, int batchSize);

    /**
     * Get the next available TX frame
     * 
     * @return Frame offset or -1 if none available
     */
    int getNextTxFrame();

    /**
     * Poll for TX completions and handle them in batches
     * This should be called regularly to free up TX ring space
     */
    void pollTxCompletions();

    /**
     * Request driver to poll for TX packets (ena-xdp pattern)
     * This is critical for AF_XDP TX to work properly
     */
    void requestDriverPoll();

    /**
     * Reserve TX ring entries (ena-xdp direct access)
     * 
     * @param count Number of entries to reserve
     * @param tx_idx Output parameter for starting index
     * @return Number of entries actually reserved
     */
    int reserveTxRing(int count, uint32_t* tx_idx);

    /**
     * Set TX descriptor directly (ena-xdp pattern)
     * 
     * @param idx Index in TX ring
     * @param addr Frame address in UMEM
     * @param len Packet length
     */
    void setTxDescriptor(uint32_t idx, uint64_t addr, uint32_t len);

    /**
     * Submit TX ring entries (ena-xdp pattern)
     * 
     * @param count Number of entries to submit
     */
    void submitTxRing(int count);

    /**
     * Copy data from one UMEM location to another
     * 
     * @param sourceBuffer Source buffer
     * @param sourceOffset Source offset
     * @param destOffset   Destination offset in this UMEM
     * @param length       Length to copy
     * @throws std::runtime_error If copying fails
     */
    void copyUmemData(const uint8_t* sourceBuffer, int sourceOffset, int destOffset, int length);

    /**
     * Receive packets
     * 
     * @param offsets Vector to store packet offsets
     * @param lengths Vector to store packet lengths
     * @return Number of packets received, or negative error code on failure
     * @throws std::runtime_error If receiving fails
     */
    int receive(std::vector<int>& offsets, std::vector<int>& lengths);
    
    /**
     * Recycle frames after processing packets
     * This must be called after processing packets received from the receive() method
     * to return the buffer frames back to the fill queue
     * 
     * @throws std::runtime_error If recycling fails
     */
    void recycleFrames();

    /**
     * Get the file descriptor for the socket
     * 
     * @return File descriptor, or negative error code on failure
     * @throws std::runtime_error If getting FD fails
     */
    int getFd();

    /**
     * Check if the socket is closed
     * 
     * @return True if the socket is closed, false otherwise
     */
    bool isClosed() const;

    /**
     * Close the socket
     */
    void close();

    /**
     * Load XDP program
     *
     * @param ifName      Interface name
     * @param programPath Path to XDP program file
     * @param nativeMode  True for native mode, false for SKB mode
     * @throws std::runtime_error if loading fails
     */
    static void loadXdpProgram(const std::string& ifName, const std::string& programPath, bool nativeMode);

    /**
     * Unload XDP program
     *
     * @param ifName     Interface name
     * @param nativeMode True for native mode, false for SKB mode
     * @throws std::runtime_error if unloading fails
     */
    static void unloadXdpProgram(const std::string& ifName, bool nativeMode);

    /**
     * Register the socket with the XDP program's map
     *
     * @param queueId Queue ID to register with
     * @return 0 on success, negative error code on failure
     * @throws std::runtime_error if registration fails
     */
    int registerXskMap(int queueId);

    /**
     * Set resource limits required for optimal AF_XDP performance
     * This should be called before creating any sockets
     * 
     * @return 0 on success, negative error code on failure
     * @throws std::runtime_error if setting resource limits fails
     */
    static int setResourceLimits();

private:
    /**
     * Check if socket is open, throw exception if not
     * 
     * @throws std::runtime_error If socket is closed
     */
    void checkOpen() const;
};

#endif // AFXDP_SOCKET_HPP
