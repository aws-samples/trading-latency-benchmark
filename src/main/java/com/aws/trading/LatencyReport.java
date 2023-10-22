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

import java.io.FileInputStream;
import java.io.IOException;
import java.nio.file.Path;

import static com.aws.trading.Main.printHelpMessage;

public class LatencyReport {
    private static final Logger LOGGER = LogManager.getLogger(LatencyReport.class);

    public static void main(String[] args) {
        if (args.length < 2) {
            printHelpMessage();
            System.exit(0);
        }
        try {
            //read path to histogram file from args and load it
            var path = Path.of(args[1]);
            LOGGER.info("loading histogram from file {}", path.toFile());
            HistogramLogReader logReader = new HistogramLogReader(new FileInputStream(path.toFile()));
            Histogram histogram = null;
            while (logReader.hasNext()){
                Histogram iter = (Histogram) logReader.nextIntervalHistogram();
                if(null == histogram){
                    histogram = iter;
                }else{
                    histogram.add(iter);
                }
            }
            assert histogram != null;
            LOGGER.info("Percentiles: \n {}", LatencyTools.createLatencyReportJson(histogram));
        } catch (IOException e) {
            LOGGER.error(e);
        }
    }
}
