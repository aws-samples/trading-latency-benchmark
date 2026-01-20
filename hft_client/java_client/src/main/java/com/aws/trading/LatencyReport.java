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

import org.HdrHistogram.Histogram;
import org.HdrHistogram.HistogramLogReader;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.stream.Stream;

import static com.aws.trading.Main.printHelpMessage;
import static java.nio.file.Files.newInputStream;

public class LatencyReport {
    private static final Logger LOGGER = LogManager.getLogger(LatencyReport.class);
    private static final String regex = ".*/LowLatencyConnectStack/([^/]+)/home/ec2-user/([^/]+)/histogram.hlog";
    private static final Pattern pattern = Pattern.compile(regex);

    public static void main(String[] args) {
        if (args.length < 2) {
            printHelpMessage();
            System.exit(0);
        }
        
        // Check if we're processing a single file or a directory
        Path path = Paths.get(args[1]);
        if (Files.isRegularFile(path)) {
            // Process a single histogram file
            processSingleHistogramFile(path, args);
        } else {
            // Process a directory of histogram files
            try (Stream<Path> paths = Files.walk(path)) {
                List<Map<String, String>> rows = paths.filter(Files::isRegularFile)
                        .filter(file -> file.toString().endsWith(".hlog"))
                        .map(file -> {
                            LOGGER.info("loading histogram from file {}", file);
                            HistogramLogReader logReader = getHistogramLogReader(file);
                            Histogram histogram = null;
                            while (logReader.hasNext()) {
                                Histogram iter = (Histogram) logReader.nextIntervalHistogram();
                                if (null == histogram) {
                                    histogram = iter;
                                } else {
                                    histogram.add(iter);
                                }
                            }
                            assert histogram != null;
                            final LinkedHashMap<String, String> row = LatencyTools.createLatencyReport(histogram);
                            final Matcher matcher = pattern.matcher(file.toString());
                            if (matcher.matches()) {
                                row.put("source", matcher.group(1));
                                row.put("destination", matcher.group(2));
                            } else {
                                LOGGER.error("Unable to parse the input string: {}", file.toString());
                            }
                            return row;
                        }).collect(Collectors.toList());
                
                // Write results to CSV
                if (!rows.isEmpty()) {
                    String outputFile = args.length > 2 ? args[2] : "output.csv";
                    writeToCSV(rows, outputFile);
                    System.out.println("CSV report written to: " + outputFile);
                } else {
                    System.err.println("No histogram files found in directory: " + path);
                }
            } catch (IOException e) {
                System.err.println("Error processing histogram log files: " + e.getMessage());
            }
        }
    }
    
    private static void processSingleHistogramFile(Path file, String[] args) {
        try {
            LOGGER.info("Processing histogram file: {}", file);
            HistogramLogReader logReader = getHistogramLogReader(file);
            Histogram histogram = null;
            
            while (logReader.hasNext()) {
                Histogram iter = (Histogram) logReader.nextIntervalHistogram();
                if (null == histogram) {
                    histogram = iter;
                } else {
                    histogram.add(iter);
                }
            }
            
            if (histogram != null) {
                // Print formatted report to stdout for shell script to parse
                printFormattedReport(histogram);
                
                // Optionally save CSV if a filename is provided
                if (args.length > 2) {
                    LinkedHashMap<String, String> row = LatencyTools.createLatencyReport(histogram);
                    List<Map<String, String>> rows = new ArrayList<>();
                    rows.add(row);
                    writeToCSV(rows, args[2]);
                    System.out.println("CSV report written to: " + args[2]);
                }
            } else {
                System.err.println("No histogram data found in file: " + file);
            }
        } catch (Exception e) {
            System.err.println("Error processing histogram file: " + e.getMessage());
        }
    }
    
    private static void printFormattedReport(Histogram histogram) {
        // Print summary statistics in a format that can be easily parsed by the shell script
        System.out.println("Latency Report Summary");
        System.out.println("=====================");
        System.out.println("Min latency: " + LatencyTools.formatNanos(histogram.getMinValue()));
        System.out.println("Max latency: " + LatencyTools.formatNanos(histogram.getMaxValue()));
        System.out.println("Mean latency: " + LatencyTools.formatNanos((long)histogram.getMean()));
        System.out.println("Total count: " + histogram.getTotalCount());
        System.out.println();
        
        System.out.println("Percentile Distribution");
        System.out.println("======================");
        for (double percentile : LatencyTools.PERCENTILES) {
            System.out.printf("%.2f%%: %s\n", 
                percentile, 
                LatencyTools.formatNanos(histogram.getValueAtPercentile(percentile)));
        }
    }

    public static void writeToCSV(List<Map<String, String>> data, String fileName) {
        try (FileWriter writer = new FileWriter(fileName)) {
            // Get the header columns from the first row
            List<String> headers = new ArrayList<>(data.get(0).keySet());

            // Write the header row
            writer.write(String.join(",", headers));
            writer.write("\n");

            // Write the data rows
            for (Map<String, String> row : data) {
                List<String> values = new ArrayList<>();
                for (String header : headers) {
                    values.add(row.get(header));
                }
                writer.write(String.join(",", values));
                writer.write("\n");
            }
        } catch (IOException e) {
            LOGGER.error("Error writing to CSV file: {}", e.getMessage());
        }
    }

    public static HistogramLogReader getHistogramLogReader(Path file) throws RuntimeException {
        try {
            return new HistogramLogReader(newInputStream(file));
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }
}
