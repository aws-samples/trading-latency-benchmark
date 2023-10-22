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

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.handler.codec.http.websocketx.TextWebSocketFrame;
import io.netty.util.CharsetUtil;
import io.netty.util.internal.PlatformDependent;

import java.nio.charset.StandardCharsets;
import java.util.Base64;

public class ExchangeProtocolImpl implements ExchangeProtocol {
    public static final byte[] AUTH_MSG_HEADER = "{\"type\":\"AUTHENTICATE\",\"api_token\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] HEADER = "{\"type\":\"CREATE_ORDER\",\"order\":{\"instrument_code\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] SYMBOL_END = "\",\"client_id\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] CLIENT_ID_END = "\",\"side\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] buySide = "BUY".getBytes(StandardCharsets.UTF_8);
    final static byte[] sellSide = "SELL".getBytes(StandardCharsets.UTF_8);
    final static byte[] SIDE_END = "\",\"type\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] dummyType = "LIMIT".getBytes(StandardCharsets.UTF_8);
    final static byte[] TYPE_END = "\",\"price\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] dummyBuyPrice = "1".getBytes(StandardCharsets.UTF_8);
    final static byte[] dummySellPrice = "2".getBytes(StandardCharsets.UTF_8);
    final static byte[] PRICE_END = "\",\"amount\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] dummyAmount = "1".getBytes(StandardCharsets.UTF_8);
    final static byte[] AMOUNT_END = "\",\"time_in_force\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] dummyTimeInForce = "GOOD_TILL_CANCELLED".getBytes(StandardCharsets.UTF_8);
    final static byte[] TIME_IN_FORCE_END = "\"}}".getBytes(StandardCharsets.UTF_8);
    final static byte[] CANCEL_ORDER_HEADER = "{\"type\":\"CANCEL_ORDER\",\"client_id\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] CANCEL_ORDER_CLIENT_ID_END = "\",\"instrument_code\":\"".getBytes(StandardCharsets.UTF_8);
    final static byte[] MSG_END =    "\"}".getBytes(StandardCharsets.UTF_8);
    final static byte[] SUBSCRIBE_MSG = "{\"type\":\"SUBSCRIBE\",\"channels\":[{\"name\":\"ORDERS\"}]}".getBytes(StandardCharsets.UTF_8);

    static byte[] randomBytes(int size) {
        byte[] bytes = new byte[size];
        PlatformDependent.threadLocalRandom().nextBytes(bytes);
        return bytes;
    }
    static String base64(byte[] data) {
        if (PlatformDependent.javaVersion() >= 8) {
            return Base64.getEncoder().encodeToString(data);
        } else {
            ByteBuf encodedData = Unpooled.wrappedBuffer(data);

            String encodedString;
            try {
                ByteBuf encoded = io.netty.handler.codec.base64.Base64.encode(encodedData);

                try {
                    encodedString = encoded.toString(CharsetUtil.UTF_8);
                } finally {
                    encoded.release();
                }
            } finally {
                encodedData.release();
            }

            return encodedString;
        }
    }

    public TextWebSocketFrame createBuyOrder(String pair, String clientId) {
        return new TextWebSocketFrame(Unpooled.wrappedBuffer(
                ExchangeProtocolImpl.HEADER,
                pair.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.SYMBOL_END,
                clientId.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.CLIENT_ID_END,
                ExchangeProtocolImpl.buySide, ExchangeProtocolImpl.SIDE_END,
                ExchangeProtocolImpl.dummyType, ExchangeProtocolImpl.TYPE_END,
                ExchangeProtocolImpl.dummyBuyPrice, ExchangeProtocolImpl.PRICE_END,
                ExchangeProtocolImpl.dummyAmount, ExchangeProtocolImpl.AMOUNT_END,
                ExchangeProtocolImpl.dummyTimeInForce, ExchangeProtocolImpl.TIME_IN_FORCE_END
        ));
    }

    public ByteBuf createSellOrder(String pair, String clientId) {
        return Unpooled.wrappedBuffer(
                ExchangeProtocolImpl.HEADER,
                pair.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.SYMBOL_END,
                clientId.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.CLIENT_ID_END,
                ExchangeProtocolImpl.sellSide, ExchangeProtocolImpl.SIDE_END,
                ExchangeProtocolImpl.dummyType, ExchangeProtocolImpl.TYPE_END,
                ExchangeProtocolImpl.dummySellPrice, ExchangeProtocolImpl.PRICE_END,
                ExchangeProtocolImpl.dummyAmount, ExchangeProtocolImpl.AMOUNT_END,
                ExchangeProtocolImpl.dummyTimeInForce, ExchangeProtocolImpl.TIME_IN_FORCE_END
        );
    }

    public ByteBuf createOrder(String pair, String type, String uuid, String side, String price, String qty) {
        return Unpooled.wrappedBuffer(
                ExchangeProtocolImpl.HEADER,
                pair.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.SYMBOL_END,
                uuid.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.CLIENT_ID_END,
                side.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.SIDE_END,
                type.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.TYPE_END,
                price.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.PRICE_END,
                qty.getBytes(StandardCharsets.UTF_8), ExchangeProtocolImpl.AMOUNT_END,
                ExchangeProtocolImpl.dummyTimeInForce, ExchangeProtocolImpl.TIME_IN_FORCE_END
        );
    }

    public TextWebSocketFrame createCancelOrder(String pair, String clientid) {
        return new TextWebSocketFrame(Unpooled.wrappedBuffer(
                ExchangeProtocolImpl.CANCEL_ORDER_HEADER,
                clientid.getBytes(StandardCharsets.UTF_8),
                ExchangeProtocolImpl.CANCEL_ORDER_CLIENT_ID_END,
                pair.getBytes(StandardCharsets.UTF_8),
                ExchangeProtocolImpl.MSG_END
        ));
    }

}
