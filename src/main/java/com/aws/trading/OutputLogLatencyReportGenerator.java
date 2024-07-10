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

import org.HdrHistogram.HistogramLogReader;
import org.HdrHistogram.SingleWriterRecorder;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;

import static com.aws.trading.Main.printHelpMessage;
import static java.nio.file.Files.newInputStream;
import static java.util.stream.Collectors.toList;

public class OutputLogLatencyReportGenerator {
    private static final Logger LOGGER = LogManager.getLogger(OutputLogLatencyReportGenerator.class);
    private static final Pattern LOG_PATTERN = Pattern.compile(
            ".*?(\\d+\\.\\d+\\.\\d+\\.\\d+)\\s*.*?\\s*Latency:\\s*(\\d+(\\.\\d+)?)(ms|µs).*",
            Pattern.CASE_INSENSITIVE
    );
    private static final Pattern pattern = Pattern.compile(".*/LowLatencyConnectStack/([^/]+)/home/ec2-user/output.log");

    public static void main(String[] args) {
        if (args.length < 2) {
            printHelpMessage();
            System.exit(0);
        }
        try (Stream<Path> paths = Files.walk(Paths.get(args[1]))) {
            LatencyReport.writeToCSV(paths.filter(Files::isRegularFile)
                            .filter(path -> path.toString().endsWith("output.log"))
                            .flatMap(file -> {
                                Map<String, SingleWriterRecorder> histograms = new HashMap<>();
                                final Matcher m = pattern.matcher(file.toString());
                                if (m.matches()) {
                                    var source = m.group(1);
                                    try {
                                        Files.readAllLines(file).forEach(line -> {
                                            Matcher matcher = LOG_PATTERN.matcher(line);
                                            if (matcher.matches()) {
                                                var destination = matcher.group(1);
                                                final SingleWriterRecorder histogram = histograms.computeIfAbsent(source + "<->" + destination, k -> new SingleWriterRecorder(Long.MAX_VALUE, 2));
                                                double latency = Double.parseDouble(matcher.group(2));
                                                String unit = matcher.group(4);
                                                long latencyInNanoseconds;
                                                if (unit.equals("ms")) {
                                                    latencyInNanoseconds = (long) (latency * 1000 * 1000);
                                                } else {
                                                    latencyInNanoseconds = (long) latency * 1000;
                                                }
                                                histogram.recordValue(latencyInNanoseconds);
                                            }
                                        });
                                        return histograms.entrySet().stream().map(entry->{
                                            var routeArr = entry.getKey().split("<->");
                                            final LinkedHashMap<String, String> row = LatencyTools.createLatencyReport(entry.getValue().getIntervalHistogram());
                                            row.put("source", routeArr[0]);
                                            row.put("destination", routeArr[1]);
                                            return row;
                                        });
                                    } catch (IOException e) {
                                        return null;
                                    }
                                } else {
                                    return null;
                                }
                            }).filter(Objects::nonNull)
                            .collect(toList()),
                    "output.csv");

        } catch (IOException e) {
            System.err.println("Error processing histogram log files: " + e.getMessage());
        }
    }
}
