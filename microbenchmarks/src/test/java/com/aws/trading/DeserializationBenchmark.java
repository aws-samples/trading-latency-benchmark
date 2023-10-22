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
import com.alibaba.fastjson2.JSONReader;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import net.openhft.chronicle.wire.*;
import org.openjdk.jmh.annotations.*;
import org.openjdk.jmh.infra.Blackhole;

import java.io.IOException;
import java.util.UUID;
import java.util.concurrent.TimeUnit;

@State(Scope.Thread)
public class DeserializationBenchmark {
    final static String jsonString = "{\"type\": \"CREATE_ORDER\",\"order\": {\"instrument_code\": \"BTC_USDT\",\"client_id\": \"123\",\"side\": \"BUY\",\"type\": \"LIMIT\",\"price\": \"100\",\"amount\": \"100\",\"time_in_force\": \"GOOD_TILL_CANCELLED\"}}";
    protected static final ObjectMapper mapper = new ObjectMapper();
    final JSONWire wire = new JSONWire().useTypes(true);


    //    820,276 ±(99.9%) 341,420 ns/op [Average]
//    (min, avg, max) = (722,445, 820,276, 946,898), stdev = 88,666
//    CI (99.9%): [478,856, 1161,696] (assumes normal distribution)
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public void benchmark_deserialization_fastjson(Blackhole blackhole) {
        JSONObject c = JSON.parseObject(jsonString);
        var client_id = c.getJSONObject("order").getString("client_id");
        blackhole.consume(client_id);
    }

    //    1332,781 ±(99.9%) 337,525 ns/op [Average]
//    (min, avg, max) = (1215,345, 1332,781, 1457,736), stdev = 87,654
//    CI (99.9%): [995,256, 1670,306] (assumes normal distribution)
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public void benchmark_deserialization_objectmapper(Blackhole blackhole) throws JsonProcessingException {
        JsonNode tmp = mapper.readTree(jsonString);
        String client_id = tmp.findValue("client_id").textValue();
        blackhole.consume(client_id);
    }

    //SLOWEST around 2000ns/ops
    @Benchmark
    @Fork(value = 1, warmups = 1)
    @BenchmarkMode(Mode.AverageTime)
    @OutputTimeUnit(TimeUnit.NANOSECONDS)
    public void benchmark_deserialization_chronicle(Blackhole blackhole) {
        wire.clear();
        wire.bytes().append(jsonString);
        var client_id = wire
                .read("order")
                .wireIn()
                .readingDocument()
                .wire().read("client_id")
                .text();
        blackhole.consume(client_id);
    }


    public static void main(String[] args) {
        try {
            org.openjdk.jmh.Main.main(args);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }
}
