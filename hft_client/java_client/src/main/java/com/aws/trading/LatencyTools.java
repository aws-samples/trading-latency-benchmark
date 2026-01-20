/*
 * Copyright 2019 Maksim Zheravin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.aws.trading;

import com.alibaba.fastjson2.JSON;
import com.alibaba.fastjson2.JSONWriter;
import org.HdrHistogram.Histogram;

import java.util.Arrays;
import java.util.LinkedHashMap;

public final class LatencyTools {

    public static final double[] PERCENTILES = new double[]{10, 50, 90, 95, 99, 99.9};

    public static String createLatencyReportJson(Histogram histogram) {
        return toJSON(createLatencyReport(histogram));
    }

    public static LinkedHashMap<String, String> createLatencyReport(Histogram histogram) {
        final LinkedHashMap<String, String> fmt = new LinkedHashMap<>();
        Arrays.stream(PERCENTILES).forEach(p ->
                fmt.put(p + "%", formatNanos(histogram.getValueAtPercentile(p)))
        );
        fmt.put("W", formatNanos(histogram.getMaxValue()));
        return fmt;
    }

    public static String toJSON(LinkedHashMap<String, String> fmt){
        return JSON.toJSONString(fmt, JSONWriter.Feature.PrettyFormat);
    }

    public static String toCSV(LinkedHashMap<String, String> fmt){
        StringBuilder sb = new StringBuilder();
        String header = String.join(",", fmt.keySet());
        String values = String.join(",", fmt.values());
        return sb.append(header).append("\n").append(values).toString();
    }

    public static String formatNanos(long ns) {
        // Return raw nanoseconds to preserve full precision
        return ns + "ns";
    }
}
