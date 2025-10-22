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
package com.aws.trading;

import io.netty.channel.Channel;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.socket.nio.NioSocketChannel;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.lang.reflect.Method;

/**
 * Factory for creating Netty transport channels with automatic platform-specific optimization.
 * This class uses reflection to detect and use the best available native transport without
 * causing ClassNotFoundException on platforms where native libraries are not available.
 * 
 * Transport priority:
 * 1. io_uring (Linux only, requires kernel 5.1+, best performance)
 * 2. epoll (Linux only, excellent performance)
 * 3. kqueue (macOS/BSD only, excellent performance)
 * 4. NIO (Java standard, works everywhere, fallback)
 */
public class NettyTransportFactory {
    
    private static final Logger LOGGER = LogManager.getLogger(NettyTransportFactory.class);
    
    // In Netty 4.2, io_uring graduated from incubator to stable
    private static final String IOURING_CHANNEL_CLASS = "io.netty.channel.uring.IOUringSocketChannel";
    private static final String IOURING_EVENTLOOP_CLASS = "io.netty.channel.uring.IOUring";
    private static final String EPOLL_CHANNEL_CLASS = "io.netty.channel.epoll.EpollSocketChannel";
    private static final String EPOLL_AVAILABILITY_CLASS = "io.netty.channel.epoll.Epoll";
    private static final String KQUEUE_CHANNEL_CLASS = "io.netty.channel.kqueue.KQueueSocketChannel";
    private static final String KQUEUE_AVAILABILITY_CLASS = "io.netty.channel.kqueue.KQueue";
    
    private static TransportType detectedTransport = null;
    
    /**
     * Enum representing available transport types
     */
    public enum TransportType {
        IOURING("io_uring", "Linux kernel 5.1+"),
        EPOLL("epoll", "Linux"),
        KQUEUE("kqueue", "macOS/BSD"),
        NIO("NIO", "All platforms");
        
        private final String name;
        private final String platform;
        
        TransportType(String name, String platform) {
            this.name = name;
            this.platform = platform;
        }
        
        public String getName() {
            return name;
        }
        
        public String getPlatform() {
            return platform;
        }
    }
    
    /**
     * Detects and returns the best available transport for the current platform.
     * This method is called once and cached for subsequent calls.
     * 
     * @param useIoUring Whether to prefer io_uring if available
     * @return The detected transport type
     */
    public static synchronized TransportType detectTransport(boolean useIoUring) {
        if (detectedTransport != null) {
            return detectedTransport;
        }
        
        LOGGER.info("Detecting best available Netty transport...");
        LOGGER.info("OS: {}, Arch: {}", System.getProperty("os.name"), System.getProperty("os.arch"));
        
        // Try io_uring first (Linux only, best performance)
        if (useIoUring && isTransportAvailable(IOURING_EVENTLOOP_CLASS)) {
            detectedTransport = TransportType.IOURING;
            LOGGER.info("Selected transport: {} ({})", detectedTransport.getName(), detectedTransport.getPlatform());
            return detectedTransport;
        }
        
        // Try epoll (Linux only)
        if (isTransportAvailable(EPOLL_AVAILABILITY_CLASS)) {
            detectedTransport = TransportType.EPOLL;
            LOGGER.info("Selected transport: {} ({})", detectedTransport.getName(), detectedTransport.getPlatform());
            return detectedTransport;
        }
        
        // Try kqueue (macOS/BSD)
        if (isTransportAvailable(KQUEUE_AVAILABILITY_CLASS)) {
            detectedTransport = TransportType.KQUEUE;
            LOGGER.info("Selected transport: {} ({})", detectedTransport.getName(), detectedTransport.getPlatform());
            return detectedTransport;
        }
        
        // Fallback to NIO (works everywhere)
        detectedTransport = TransportType.NIO;
        LOGGER.info("Selected transport: {} ({})", detectedTransport.getName(), detectedTransport.getPlatform());
        LOGGER.info("Native transports not available, using standard Java NIO");
        
        return detectedTransport;
    }
    
    /**
     * Creates the appropriate socket channel class based on detected transport.
     * 
     * @param useIoUring Whether to prefer io_uring if available
     * @return The socket channel class
     */
    public static Class<? extends Channel> getSocketChannelClass(boolean useIoUring) {
        TransportType transport = detectTransport(useIoUring);
        
        try {
            switch (transport) {
                case IOURING:
                    return loadChannelClass(IOURING_CHANNEL_CLASS);
                case EPOLL:
                    return loadChannelClass(EPOLL_CHANNEL_CLASS);
                case KQUEUE:
                    return loadChannelClass(KQUEUE_CHANNEL_CLASS);
                case NIO:
                default:
                    return NioSocketChannel.class;
            }
        } catch (Exception e) {
            LOGGER.warn("Failed to load channel class for {}, falling back to NIO", transport, e);
            detectedTransport = TransportType.NIO;
            return NioSocketChannel.class;
        }
    }
    
    /**
     * Checks if a specific transport is available by attempting to load its availability class
     * and calling its isAvailable() method via reflection.
     * 
     * @param availabilityClassName The fully qualified class name to check
     * @return true if the transport is available, false otherwise
     */
    private static boolean isTransportAvailable(String availabilityClassName) {
        try {
            Class<?> availabilityClass = Class.forName(availabilityClassName, true, 
                    NettyTransportFactory.class.getClassLoader());
            Method isAvailableMethod = availabilityClass.getMethod("isAvailable");
            Boolean isAvailable = (Boolean) isAvailableMethod.invoke(null);
            
            if (Boolean.TRUE.equals(isAvailable)) {
                LOGGER.debug("Transport {} is available", availabilityClassName);
                return true;
            } else {
                LOGGER.debug("Transport {} is not available on this platform", availabilityClassName);
                return false;
            }
        } catch (ClassNotFoundException e) {
            LOGGER.debug("Transport class {} not found in classpath", availabilityClassName);
            return false;
        } catch (NoClassDefFoundError e) {
            LOGGER.debug("Transport class {} dependencies not found", availabilityClassName);
            return false;
        } catch (UnsatisfiedLinkError e) {
            LOGGER.debug("Native library for {} not found", availabilityClassName);
            return false;
        } catch (Exception e) {
            LOGGER.warn("Error checking availability of {}: {}", availabilityClassName, e.getMessage());
            return false;
        }
    }
    
    /**
     * Loads a channel class by name using reflection.
     * 
     * @param className The fully qualified channel class name
     * @return The channel class
     * @throws ClassNotFoundException if the class cannot be found
     */
    @SuppressWarnings("unchecked")
    private static Class<? extends Channel> loadChannelClass(String className) throws ClassNotFoundException {
        return (Class<? extends Channel>) Class.forName(className, true, 
                NettyTransportFactory.class.getClassLoader());
    }
    
    /**
     * Gets a descriptive name of the currently selected transport.
     * 
     * @return The transport name
     */
    public static String getTransportName() {
        if (detectedTransport == null) {
            return "Not yet detected";
        }
        return String.format("%s (%s)", detectedTransport.getName(), detectedTransport.getPlatform());
    }
}
