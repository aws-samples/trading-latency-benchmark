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

import io.netty.bootstrap.Bootstrap;
import io.netty.channel.*;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.codec.http.HttpClientCodec;
import io.netty.handler.codec.http.HttpObjectAggregator;
import io.netty.handler.codec.http.websocketx.CloseWebSocketFrame;
import io.netty.handler.ssl.OpenSsl;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.SslProvider;
import io.netty.incubator.channel.uring.IOUringSocketChannel;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import javax.net.ssl.*;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.security.*;
import java.security.cert.CertificateException;
import java.time.Duration;
import java.util.Arrays;

import static com.aws.trading.Config.*;

/**
 * Client for connecting to the exchange server via WebSocket and HTTP.
 * Handles SSL/TLS configuration and connection management.
 */
public class ExchangeClient {

    private static final Logger LOGGER = LogManager.getLogger(ExchangeClient.class);
    // Support both TLSv1.2 and TLSv1.3 for better compatibility
    private static final String[] TLS_PROTOCOL_VERSIONS = {"TLSv1.2", "TLSv1.3"};
    private static final int HTTP_TIMEOUT_SECONDS = 10;
    
    {
        LOGGER.info("OpenSSL: available={} version= {}", OpenSsl.isAvailable(), OpenSsl.versionString());
    }
    
    private final HttpClient httpClient;
    private final ExchangeClientLatencyTestHandler handler;
    private final EventLoopGroup workerGroup;
    private final Integer apiToken;
    
    private Channel ch;
    private Bootstrap bootstrap;

    /**
     * Creates a new ExchangeClient with the specified API token and event handlers.
     *
     * @param apiToken The API token for authentication
     * @param handler The WebSocket handler for processing messages
     * @param ioGroup The event loop group for I/O operations
     * @param workerGroup The event loop group for worker operations
     * @throws IOException If an I/O error occurs
     * @throws NoSuchAlgorithmException If a required cryptographic algorithm is not available
     * @throws KeyStoreException If there is an error accessing the keystore
     * @throws CertificateException If there is an error with the certificate
     * @throws UnrecoverableKeyException If the key cannot be recovered
     * @throws KeyManagementException If there is an error with key management
     */
    public ExchangeClient(int apiToken, ExchangeClientLatencyTestHandler handler, 
                         MultithreadEventLoopGroup ioGroup, 
                         MultithreadEventLoopGroup workerGroup) 
            throws IOException, NoSuchAlgorithmException, KeyStoreException, 
                   CertificateException, UnrecoverableKeyException, KeyManagementException {
        
        this.apiToken = apiToken;
        this.handler = handler;
        this.workerGroup = workerGroup;
        
        SslContext sslCtx = null;
        var httpClientBuilder = HttpClient.newBuilder();
        
        if (USE_SSL) {
            LOGGER.info("Configuring SSL/TLS with keystore: {}", KEY_STORE_PATH);
            
            try {
                // Configure SSL context for both HTTP client and WebSocket
                SSLContext sslContext = SSLContext.getInstance("TLS");
                KeyStore keyStore = loadKeyStore();
                
                KeyManagerFactory kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
                kmf.init(keyStore, KEY_STORE_PASSWORD.toCharArray());
                
                TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
                tmf.init(keyStore);
                
                sslContext.init(kmf.getKeyManagers(), tmf.getTrustManagers(), null);
                httpClientBuilder = httpClientBuilder.sslContext(sslContext);
                
                sslCtx = SslContextBuilder.forClient()
                        .sslProvider(SslProvider.OPENSSL_REFCNT)
                        .trustManager(tmf)
                        .keyManager(kmf)
                        .protocols(TLS_PROTOCOL_VERSIONS)
                        .ciphers(Arrays.asList(CIPHERS.split(",")))
                        .build();
                
                LOGGER.info("SSL/TLS configuration completed successfully");
            } catch (Exception e) {
                LOGGER.error("Failed to configure SSL/TLS", e);
                throw e;
            }
        }

        this.bootstrap = configureBootstrap(ioGroup)
                .handler(getChannelInitializer(workerGroup, handler, sslCtx));
                
        this.httpClient = httpClientBuilder
                .connectTimeout(Duration.ofSeconds(HTTP_TIMEOUT_SECONDS))
                .build();
    }

    /**
     * Loads the keystore from the configured path.
     *
     * @return The loaded KeyStore
     * @throws KeyStoreException If there is an error accessing the keystore
     * @throws IOException If an I/O error occurs
     * @throws NoSuchAlgorithmException If a required cryptographic algorithm is not available
     * @throws CertificateException If there is an error with the certificate
     */
    private KeyStore loadKeyStore() throws KeyStoreException, IOException, 
                                          NoSuchAlgorithmException, CertificateException {
        KeyStore keyStore = KeyStore.getInstance("PKCS12");
        try (FileInputStream fis = new FileInputStream(KEY_STORE_PATH)) {
            keyStore.load(fis, KEY_STORE_PASSWORD.toCharArray());
            return keyStore;
        } catch (FileNotFoundException e) {
            LOGGER.error("Keystore file not found: {}", KEY_STORE_PATH, e);
            throw e;
        }
    }

    /**
     * Configures the Netty bootstrap with the specified event loop group.
     *
     * @param workerGroup The event loop group for worker operations
     * @return The configured Bootstrap
     */
    private static Bootstrap configureBootstrap(MultithreadEventLoopGroup workerGroup) {
        return new Bootstrap()
                .group(workerGroup)
                .channel(USE_IOURING ? IOUringSocketChannel.class : NioSocketChannel.class)
                .option(ChannelOption.SO_KEEPALIVE, true)
                .option(ChannelOption.TCP_NODELAY, true); // Added TCP_NODELAY for lower latency
    }

    /**
     * Adds balance to the user account.
     *
     * @param uri The base URI of the exchange
     * @param qt The currency to add balance for
     * @throws RuntimeException If an error occurs during the HTTP request
     */
    public void addBalances(URI uri, String qt) throws RuntimeException {
        try {
            String endpoint = buildBalanceEndpoint(uri, qt);
            
            final HttpRequest request = HttpRequest.newBuilder()
                    .POST(HttpRequest.BodyPublishers.noBody())
                    .uri(URI.create(endpoint))
                    .build();
                    
            LOGGER.info("addBalances Request=> {}", request);
            HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());
            
            if (response.statusCode() >= 200 && response.statusCode() < 300) {
                LOGGER.info("User Created and balances sent for user:{}", apiToken);
            } else {
                LOGGER.warn("Failed to add balances. Status code: {}, Response: {}", 
                           response.statusCode(), response.body());
            }
        } catch (Exception e) {
            LOGGER.error("Error adding balances", e);
            throw new RuntimeException("Failed to add balances", e);
        }
    }

    /**
     * Builds the endpoint URL for adding balances.
     *
     * @param uri The base URI of the exchange
     * @param qt The currency to add balance for
     * @return The complete endpoint URL
     */
    private String buildBalanceEndpoint(URI uri, String qt) {
        return new StringBuilder()
                .append(uri.getScheme()).append("://")
                .append(uri.getHost())
                .append(":").append(uri.getPort())
                .append("/private/account/user/balances/")
                .append(apiToken).append("/").append(qt)
                .append("/").append(100000000)
                .toString();
    }

    /**
     * Connects to the exchange server via WebSocket.
     *
     * @throws InterruptedException If the connection is interrupted
     * @throws ConnectException If the connection fails
     */
    public void connect() throws InterruptedException {
        LOGGER.info("ExchangeClient is connecting via websocket to {}:{}", 
                   handler.uri.getHost(), handler.uri.getPort());
        try {
            this.ch = this.bootstrap.connect(handler.uri.getHost(), handler.uri.getPort()).sync().channel();
            LOGGER.info("Successfully connected to exchange server");
        } catch (Exception e) {
            LOGGER.error("Failed to connect to exchange server", e);
            throw e;
        }
    }

    /**
     * Creates a channel initializer for the Netty pipeline.
     *
     * @param workerGroup The event loop group for worker operations
     * @param handler The WebSocket handler for processing messages
     * @param sslCtx The SSL context for secure connections
     * @return The channel initializer
     */
    private static ChannelInitializer<SocketChannel> getChannelInitializer(
            MultithreadEventLoopGroup workerGroup, 
            ExchangeClientLatencyTestHandler handler, 
            SslContext sslCtx) {
        return new ChannelInitializer<>() {
            @Override
            public void initChannel(SocketChannel channel) throws Exception {
                ChannelPipeline pipeline = channel.pipeline();
                if (null != sslCtx) {
                    pipeline.addLast(sslCtx.newHandler(channel.alloc(), 
                                    handler.uri.getHost(), handler.uri.getPort()));
                }
                pipeline.addLast("http-codec", new HttpClientCodec());
                pipeline.addLast("aggregator", new HttpObjectAggregator(65536));
                pipeline.addLast(workerGroup, "ws-handler", handler);
            }
        };
    }

    /**
     * Closes the WebSocket connection gracefully.
     *
     * @throws InterruptedException If the closing process is interrupted
     */
    public void close() throws InterruptedException {
        LOGGER.info("Closing WebSocket connection");
        try {
            ch.writeAndFlush(new CloseWebSocketFrame());
            ch.closeFuture().sync();
            LOGGER.info("WebSocket connection closed successfully");
        } catch (Exception e) {
            LOGGER.error("Error closing WebSocket connection", e);
            throw e;
        }
    }

    /**
     * Disconnects from the exchange server.
     */
    public void disconnect() {
        LOGGER.info("Disconnecting from exchange server");
        if (this.ch == null) {
            LOGGER.warn("Channel is null, nothing to disconnect");
            return;
        }
        
        try {
            if (this.ch.isOpen()) {
                this.ch.disconnect().await();
                this.ch.closeFuture().await();
                LOGGER.info("Successfully disconnected from exchange server");
            } else {
                LOGGER.info("Channel already closed, no need to disconnect");
            }
        } catch (InterruptedException e) {
            LOGGER.error("Interrupted while disconnecting", e);
            // Restore the interrupted status
            Thread.currentThread().interrupt();
        }
    }
}
