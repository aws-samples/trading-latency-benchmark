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

import java.io.IOException;
import java.net.URISyntaxException;

public class Main {

    public static void main(String[] args) throws URISyntaxException, InterruptedException, IOException {
        //parse commands from args
        if (args.length < 1) {
            printHelpMessage();
            System.exit(0);
        }
        String command = args[0];
        if ("latency-test".equals(command)) {
            RoundTripLatencyTester.main(args);
        } else if ("latency-report".equals(command)) {
            LatencyReport.main(args);
        } else if ("help".equals(command)) {
            printHelpMessage();
        } else {
            printHelpMessage();
            System.exit(0);
        }
    }

    public static void printHelpMessage() {
        System.out.println("Usage: java -jar <jar_file> <command> [<args>]");
        System.out.println("Commands:");
        System.out.println("latency-test: run latency test");
        System.out.println("latency-report: print latency report");
        System.out.println("<args> for latency-report:");
        System.out.println("<path to latency report file>");
        System.out.println("help: print this message");
        System.out.println("exit: exit the program");
    }
}
