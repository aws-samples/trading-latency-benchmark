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
import io.netty.incubator.channel.uring.IOUringSocketChannel;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;

import static com.aws.trading.Config.USE_IOURING;

public class ExchangeClient {
    private static final Logger LOGGER = LogManager.getLogger(ExchangeClient.class);
    private final HttpClient httpClient;
    private final ExchangeClientLatencyTestHandler handler;
    private final EventLoopGroup workerGroup;
    private Integer apiToken;
    Channel ch;
    private Bootstrap bootstrap;


    public ExchangeClient(int apiToken, ExchangeClientLatencyTestHandler handler, MultithreadEventLoopGroup ioGroup, MultithreadEventLoopGroup workerGroup) {
        this.apiToken = apiToken;
        this.handler = handler;
        this.bootstrap = configureBootstrap(ioGroup).handler(getChannelInitializer(workerGroup, handler));
        this.workerGroup = workerGroup;
        this.httpClient = HttpClient
                .newBuilder()
                .connectTimeout(Duration.ofSeconds(10))
                .build();
    }

    private static Bootstrap configureBootstrap(MultithreadEventLoopGroup workerGroup) {
        return new Bootstrap()
                .group(workerGroup)
                .channel(USE_IOURING ? IOUringSocketChannel.class : NioSocketChannel.class)
                .option(ChannelOption.SO_KEEPALIVE, true);
    }

    public void addBalances(URI uri, String qt) throws RuntimeException {
        try {
            String endpoint =
                    new StringBuilder().append("http://")
                            .append(uri.getHost())
                            .append(":").append(uri.getPort())
                            .append("/private/account/user/balances/")
                            .append(apiToken).append("/").append(qt)
                            .append("/").append(100000000).toString();

            final HttpRequest request = HttpRequest.newBuilder()
                    .POST(HttpRequest.BodyPublishers.noBody())
                    .uri(URI.create(endpoint))
                    .build();
            LOGGER.info("addBalances Request=> {}", request);
            HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());
            LOGGER.info("addBalances Response=> {}", response);
            LOGGER.info("User Created and balances sent for user:{}", apiToken);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }


    public void connect() throws InterruptedException {
        LOGGER.info("ExchangeClient is connecting via websocket to {}:{}", handler.uri.getHost(), handler.uri.getPort());
        this.ch = this.bootstrap.connect(handler.uri.getHost(), handler.uri.getPort()).sync().channel();
    }

    private static ChannelInitializer<SocketChannel> getChannelInitializer(MultithreadEventLoopGroup workerGroup, ExchangeClientLatencyTestHandler handler) {
        return new ChannelInitializer<>() {
            @Override
            public void initChannel(SocketChannel channel) throws Exception {
                ChannelPipeline pipeline = channel.pipeline();
                pipeline.addLast("http-codec", new HttpClientCodec());
                pipeline.addLast("aggregator", new HttpObjectAggregator(65536));
                pipeline.addLast(workerGroup, "ws-handler", handler);
            }
        };
    }

    public void close() throws InterruptedException {
        //System.out.println("WebSocket Client sending close");
        ch.writeAndFlush(new CloseWebSocketFrame());
        ch.closeFuture().sync();
        //group.shutdownGracefully();
    }

    public void disconnect() {
        LOGGER.info("disconnecting...");
        try {
            if (this.ch.isOpen()) {
                this.ch.disconnect().await();
                this.ch.closeFuture().await();
            }
        } catch (InterruptedException e) {
            LOGGER.error(e);
        }
    }
}

