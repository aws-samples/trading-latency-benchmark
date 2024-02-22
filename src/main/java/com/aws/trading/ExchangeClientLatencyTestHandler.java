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

import com.alibaba.fastjson2.JSON;
import com.alibaba.fastjson2.JSONObject;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelPromise;
import io.netty.handler.codec.http.FullHttpResponse;
import io.netty.handler.codec.http.HttpHeaders;
import io.netty.handler.codec.http.websocketx.*;
import io.netty.handler.ssl.SslContext;
import io.netty.handler.ssl.SslContextBuilder;
import io.netty.handler.ssl.util.InsecureTrustManagerFactory;
import io.netty.util.CharsetUtil;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import javax.net.ssl.SSLException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Random;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

import static com.aws.trading.Config.COIN_PAIRS;
import static com.aws.trading.Config.USE_SSL;
import static com.aws.trading.RoundTripLatencyTester.printResults;

public class ExchangeClientLatencyTestHandler extends ChannelInboundHandlerAdapter {
    private static final Logger LOGGER = LogManager.getLogger(ExchangeClientLatencyTestHandler.class);
    private final WebSocketClientHandshaker handshaker;
    private final int apiToken;
    private final int test_size;
    public final URI uri;
    private final ExchangeProtocol protocol;

    private ChannelPromise handshakeFuture;
    private final ConcurrentHashMap<String, Long> orderSentTimeMap;
    private final ConcurrentHashMap<String, Long> cancelSentTimeMap;
    private long orderResponseCount = 0;
    private final SingleWriterRecorder hdrRecorderForAggregation;
    private long testStartTime = 0;
    private final Random random = new Random();

    public ExchangeClientLatencyTestHandler(ExchangeProtocol protocol, URI uri, int apiToken, int test_size) {
        this.uri = uri;
        this.protocol = protocol;
        var header = HttpHeaders.EMPTY_HEADERS;
        this.handshaker = WebSocketClientHandshakerFactory.newHandshaker(
                uri, WebSocketVersion.V13, null, false, header, 1280000);
        this.apiToken = apiToken;
        this.orderSentTimeMap = new ConcurrentHashMap<>(test_size);
        this.cancelSentTimeMap = new ConcurrentHashMap<>(test_size);
        this.test_size = test_size;
        this.hdrRecorderForAggregation = new SingleWriterRecorder(Long.MAX_VALUE, 2);
    }

    @Override
    public void handlerAdded(final ChannelHandlerContext ctx) throws Exception {
        this.handshakeFuture = ctx.newPromise();
    }

    @Override
    public void channelActive(final ChannelHandlerContext ctx) throws Exception {
        LOGGER.info("channel is active, starting websocket handshaking...");
        handshaker.handshake(ctx.channel());
    }

    @Override
    public void channelInactive(final ChannelHandlerContext ctx) throws Exception {
        LOGGER.info("Websocket client disconnected");
    }

    @Override
    public void channelRead(ChannelHandlerContext ctx, Object msg) throws Exception {
        final Channel ch = ctx.channel();
        if (!handshaker.isHandshakeComplete()) {
            LOGGER.info("Websocket client is connected");
            var m = (FullHttpResponse) msg;
            handshaker.finishHandshake(ch, m);
            LOGGER.info("Websocket client is authenticating for {}", this.apiToken);
            //success, authenticate
            var channel = ctx.channel();
            channel.write(authMessage());
            channel.flush();
            handshakeFuture.setSuccess();
            return;
        }

        if (msg instanceof FullHttpResponse) {
            final FullHttpResponse response = (FullHttpResponse) msg;
            throw new Exception("Unexpected FullHttpResponse (getStatus=" + response.getStatus() + ", content="
                    + response.content().toString(CharsetUtil.UTF_8) + ')');
        }
        final WebSocketFrame frame = (WebSocketFrame) msg;
        if (frame instanceof TextWebSocketFrame) {
            this.onTextWebSocketFrame(ctx, (TextWebSocketFrame) frame);
        } else if (frame instanceof PongWebSocketFrame) {
        } else if (frame instanceof CloseWebSocketFrame) {
            LOGGER.info("received CloseWebSocketFrame, closing the channel");
            ch.close();
        } else if (frame instanceof BinaryWebSocketFrame) {
            LOGGER.info(frame.content().toString());
        }

    }

    private TextWebSocketFrame subscribeMessage() {
        return new TextWebSocketFrame(Unpooled.wrappedBuffer(ExchangeProtocolImpl.SUBSCRIBE_MSG));
    }

    private TextWebSocketFrame authMessage() {
        return new TextWebSocketFrame(Unpooled.wrappedBuffer(
                ExchangeProtocolImpl.AUTH_MSG_HEADER,
                Integer.toString(this.apiToken).getBytes(StandardCharsets.UTF_8),
                ExchangeProtocolImpl.MSG_END)
        );
    }

    @Override
    public void exceptionCaught(final ChannelHandlerContext ctx, final Throwable cause) throws Exception {
        LOGGER.error(cause);
        if (!handshakeFuture.isDone()) {
            handshakeFuture.setFailure(cause);
        }
        ctx.close();
    }

    public ChannelPromise getHandshakeFuture() {
        return handshakeFuture;
    }

    private void onTextWebSocketFrame(ChannelHandlerContext ctx, TextWebSocketFrame textFrame) throws InterruptedException {
        long eventReceiveTime = System.nanoTime();
        ByteBuf buf = textFrame.content();

        final byte[] bytes;
        int offset = 0;
        final int length = buf.readableBytes();
        if (buf.hasArray()) {
            bytes = buf.array();
            offset = buf.arrayOffset();
        } else {
            bytes = new byte[length];
            buf.getBytes(buf.readerIndex(), bytes);
        }
        buf.clear();
        buf.release();

        JSONObject parsedObject = JSON.parseObject(bytes, offset, bytes.length - offset, StandardCharsets.UTF_8);
        Object type = parsedObject.getString("type");

        if ("BOOKED".equals(type) || type.equals("DONE")) {
            //LOGGER.info("eventTime: {}, received ACK: {}",eventReceiveTime, parsedObject);
            String clientId = parsedObject.getString("client_id");
            if (type.equals("BOOKED")) {
                if (calculateRoundTrip(eventReceiveTime, clientId, orderSentTimeMap)) return;
                var pair = parsedObject.getString("instrument_code");
                sendCancelOrder(ctx, clientId, pair);
            } else {
                if (calculateRoundTrip(eventReceiveTime, clientId, cancelSentTimeMap)) return;
                sendOrder(ctx);
            }
            if (orderResponseCount % test_size == 0) {
                printResults(hdrRecorderForAggregation, test_size);
            }
        } else if ("AUTHENTICATED".equals(type)) {
            LOGGER.info("{}", parsedObject);
            ctx.channel().writeAndFlush(subscribeMessage());
        } else if ("SUBSCRIPTIONS".equals(type)) {
            LOGGER.info("{}", parsedObject);
            this.testStartTime = System.nanoTime();
            sendOrder(ctx);
        } else {
            LOGGER.error("Unhandled object {}", parsedObject);
        }
    }

    private void sendCancelOrder(ChannelHandlerContext ctx, String clientId, String pair) {
        TextWebSocketFrame cancelOrder = protocol.createCancelOrder(pair, clientId);
        //LOGGER.info("Sending cancel order seq: {}, order: {}", sequence, cancelOrder.toString(StandardCharsets.UTF_8));
        try {
            ctx.channel().write(cancelOrder, ctx.channel().voidPromise()).await();
        } catch (InterruptedException e) {
            LOGGER.error(e);
        }
        var cancelSentTime = System.nanoTime();
        //LOGGER.info("cancel sent time for clientId: {} - {}",clientId, cancelSentTime);
        this.cancelSentTimeMap.put(clientId, cancelSentTime);
        ctx.channel().flush();
        orderResponseCount += 1;
    }

    private boolean calculateRoundTrip(long eventReceiveTime, String clientId, ConcurrentHashMap<String, Long> cancelSentTimeMap) {
        long roundTripTime;
        Long cancelSentTime = cancelSentTimeMap.remove(clientId);
        if (null == cancelSentTime || eventReceiveTime < cancelSentTime) {
            LOGGER.error("no order sent time found for order {}", clientId);
            return true;
        }
        roundTripTime = eventReceiveTime - cancelSentTime;
        //LOGGER.info("round trip time for client id {}: {} = {} - {}", clientId, roundTripTime, eventReceiveTime, cancelSentTime);
        if (roundTripTime > 0) {
            //LOGGER.info("recording round trip time");
            hdrRecorderForAggregation.recordValue(roundTripTime);
        }
        return false;
    }

    void sendOrder(ChannelHandlerContext ch) throws InterruptedException {

        var pair = COIN_PAIRS.get(random.nextInt(COIN_PAIRS.size()));
        var clientId = UUID.randomUUID().toString();
        var order = protocol.createBuyOrder(pair, clientId);
        ch.write(order, ch.voidPromise()).await();
        var time = System.nanoTime();
        //LOGGER.info("sending order: {}, time: {}", clientId, time);
        orderSentTimeMap.put(clientId, time);
        //LOGGER.info("sending pair, clientId: {}, {}", pair, clientId);
        ch.flush();
        orderResponseCount += 1;
    }
}
