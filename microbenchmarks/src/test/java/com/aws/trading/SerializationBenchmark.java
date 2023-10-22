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
import io.netty.util.CharsetUtil;
import net.openhft.chronicle.bytes.Bytes;
import net.openhft.chronicle.wire.JSONWire;
import org.openjdk.jmh.annotations.*;
import org.openjdk.jmh.infra.Blackhole;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

@State(Scope.Thread)
public class SerializationBenchmark {
    final Bytes<ByteBuffer> bytes = Bytes.elasticHeapByteBuffer(143).unchecked(true);
    JSONWire json = new JSONWire(bytes, false);


    //FASTEST!!!
    //    # Warmup Iteration   2: 493,484 ns/op
    //    # Warmup Iteration   3: 370,085 ns/op
    //    # Warmup Iteration   4: 364,415 ns/op
    //    # Warmup Iteration   5: 364,112 ns/op
    //    Iteration   1: 424,791 ns/op
    //    Iteration   2: 400,504 ns/op
    //    Iteration   3: 361,697 ns/op
    //    Iteration   4: 502,341 ns/op
    //    Iteration   5: 357,765 ns/op
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public ByteBuf benchmark_bytebuf(Blackhole blackhole){
        var symbol = "BTC_USDT".getBytes(StandardCharsets.UTF_8);
        var type = "LIMIT".getBytes(StandardCharsets.UTF_8);
        var client_id = "123".getBytes(StandardCharsets.UTF_8);
        var side = "BUY".getBytes(StandardCharsets.UTF_8);
        var price = "100".getBytes(StandardCharsets.UTF_8);
        var amount = "100".getBytes(StandardCharsets.UTF_8);
        var time_in_force = "GOOD_TILL_CANCELLED".getBytes(StandardCharsets.UTF_8);
        var bytebuf =  Unpooled.wrappedBuffer(
                Protocol.HEADER,
                symbol, Protocol.SYMBOL_END,
                client_id, Protocol.CLIENT_ID_END,
                side, Protocol.SIDE_END,
                type, Protocol.TYPE_END,
                price, Protocol.PRICE_END,
                amount, Protocol.AMOUNT_END,
                time_in_force, Protocol.TIME_IN_FORCE_END
                );
        blackhole.consume(bytebuf);

        return bytebuf;
    }
//    SECOND
//    # Warmup Iteration   2: 768,473 ns/op
//    # Warmup Iteration   3: 855,847 ns/op
//    # Warmup Iteration   4: 728,553 ns/op
//    # Warmup Iteration   5: 931,837 ns/op
//    Iteration   1: 724,979 ns/op
//    Iteration   2: 724,555 ns/op
//    Iteration   3: 748,855 ns/op
//    Iteration   4: 739,143 ns/op
//    Iteration   5: 737,065 ns/op
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public ByteBuf benchmark_chronicle_wire(Blackhole blackhole) {
        // Do nothing
        bytes.clear();
        var dc = json.writingDocument();
        var wire = dc.wire();
        wire.write("instrument_code").text("BTC_USDT")
                .write("client_id").text("123")
                .write("side").text("BUY")
                .write("type").text("LIMIT")
                .write("price").text("100")
                .write("amount").text("100")
                .write("time_in_force").text("GOOD_TILL_CANCELLED");
        dc.close();
        ByteBuffer buffer = bytes.underlyingObject();
        var bytebuf = Unpooled.wrappedBuffer(buffer);
        blackhole.consume(buffer);
        return bytebuf;
    }

    //SLOW around 1500-2000ns/op
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public ByteBuf benchmark_stringbuilder(Blackhole blackhole){
        final StringBuilder stb = new StringBuilder(250);
        stb.setLength(0);
        var msg = stb.append("{\"type\":\"CREATE_ORDER\",\"order\":{\"instrument_code\":\"")
                .append("BTC_USDT").append("\",\"client_id\":\"")
                .append("123").append("\",").append("\"side\":\"")
                .append("BUY").append("\",\"type\":\"LIMIT\",\"price\":\"")
                .append("100").append("\",\"amount\":\"")
                .append("100").append("\",\"time_in_force\":\"GOOD_TILL_CANCELLED\"}}")
                .toString();
        var bytebuf =  Unpooled.copiedBuffer(msg, CharsetUtil.UTF_8);
        blackhole.consume(bytebuf);
        return bytebuf;
    }

    public static void main(String[] args) {
        try {
            org.openjdk.jmh.Main.main(args);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }
}
