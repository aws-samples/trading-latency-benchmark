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

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.net.URISyntaxException;
import java.security.KeyManagementException;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.UnrecoverableKeyException;
import java.security.cert.CertificateException;

/**
 * Main entry point for the trading latency benchmark application.
 * Provides command-line interface to run different types of tests and reports.
 */
public class Main {
    private static final Logger LOGGER = LogManager.getLogger(Main.class);

    /**
     * Main entry point for the application.
     *
     * @param args Command line arguments
     * @throws Exception If an error occurs during execution
     */
    public static void main(String[] args) {
        try {
            if (args.length < 1) {
                printHelpMessage();
                System.exit(0);
            }
            
            String command = args[0].toLowerCase();
            executeCommand(command, args);
        } catch (Exception e) {
            LOGGER.error("Error executing command", e);
            System.err.println("Error: " + e.getMessage());
            printHelpMessage();
            System.exit(1);
        }
    }

    /**
     * Executes the specified command with the given arguments.
     *
     * @param command The command to execute
     * @param args The command line arguments
     * @throws URISyntaxException If there is an error with the URI syntax
     * @throws InterruptedException If the execution is interrupted
     * @throws IOException If an I/O error occurs
     * @throws UnrecoverableKeyException If the key cannot be recovered
     * @throws CertificateException If there is an error with the certificate
     * @throws NoSuchAlgorithmException If a required cryptographic algorithm is not available
     * @throws KeyStoreException If there is an error accessing the keystore
     * @throws KeyManagementException If there is an error with key management
     */
    private static void executeCommand(String command, String[] args) 
            throws URISyntaxException, InterruptedException, IOException, 
                   UnrecoverableKeyException, CertificateException, 
                   NoSuchAlgorithmException, KeyStoreException, KeyManagementException {
        
        switch (command) {
            case "latency-test":
                LOGGER.info("Starting latency test");
                RoundTripLatencyTester.main(args);
                break;
                
            case "ping-latency":
                LOGGER.info("Starting ping latency test");
                EndpointPingClient.main(args);
                break;
                
            case "payload-ping":
                LOGGER.info("Starting payload size ping test");
                PayloadSizePingClient.main(args);
                break;
                
            case "latency-report":
                LOGGER.info("Generating latency report");
                if (args.length < 2) {
                    LOGGER.error("Missing report file path");
                    System.err.println("Error: Missing report file path");
                    printHelpMessage();
                    System.exit(1);
                }
                LatencyReport.main(args);
                break;
                
            case "help":
                printHelpMessage();
                break;
                
            default:
                LOGGER.error("Unknown command: {}", command);
                System.err.println("Unknown command: " + command);
                printHelpMessage();
                System.exit(1);
        }
    }

    /**
     * Prints the help message with usage instructions.
     */
    public static void printHelpMessage() {
        System.out.println("Trading Latency Benchmark Tool");
        System.out.println("==============================");
        System.out.println("Usage: java -jar <jar_file> <command> [<args>]");
        System.out.println("\nCommands:");
        System.out.println("  latency-test    Run round-trip latency test between client and server");
        System.out.println("  ping-latency    Run ping latency test to measure network round-trip time");
        System.out.println("  payload-ping    Run ping test with varying payload sizes to measure impact on latency");
        System.out.println("  latency-report  Generate and print latency report from log file");
        System.out.println("  help            Print this help message");
        System.out.println("\nArguments for latency-report:");
        System.out.println("  <path>          Path to the latency report file (.hlog)");
        System.out.println("\nExamples:");
        System.out.println("  java -jar ExchangeFlow-1.0-SNAPSHOT.jar latency-test");
        System.out.println("  java -jar ExchangeFlow-1.0-SNAPSHOT.jar ping-latency");
        System.out.println("  java -jar ExchangeFlow-1.0-SNAPSHOT.jar payload-ping");
        System.out.println("  java -jar ExchangeFlow-1.0-SNAPSHOT.jar latency-report ./histogram_logs/latency.hlog");
    }
}
