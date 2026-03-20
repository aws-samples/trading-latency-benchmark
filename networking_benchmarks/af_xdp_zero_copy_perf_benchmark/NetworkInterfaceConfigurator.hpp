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

#ifndef NETWORK_INTERFACE_CONFIGURATOR_HPP
#define NETWORK_INTERFACE_CONFIGURATOR_HPP

#include <string>

/**
 * Helper class to configure network interfaces for XDP
 */
class NetworkInterfaceConfigurator {
public:
    /**
     * Check if the current process has root privileges
     * 
     * @return true if running as root, false otherwise
     */
    static bool hasRootPrivileges();

    /**
     * Configure a network interface for XDP compatibility
     * 
     * @param interfaceName Name of the interface to configure
     * @return Recommended headroom value for the interface
     * @throws std::runtime_error If configuration fails
     */
    static int configureForXdp(const std::string& interfaceName);

    /**
     * Apply advanced XDP settings to optimize performance
     * 
     * @param interfaceName Name of the interface
     * @throws std::runtime_error If configuration fails
     */
    static void optimizeForXdp(const std::string& interfaceName);

private:
    /**
     * Get the driver name for a network interface
     * 
     * @param interfaceName Name of the interface
     * @return Driver name
     */
    static std::string getDriverName(const std::string& interfaceName);

    /**
     * Determine the recommended headroom value for XDP based on the driver
     * 
     * @param driverName Name of the network driver
     * @return Recommended headroom value
     */
    static int determineHeadroom(const std::string& driverName);

    /**
     * Execute a system command and return the exit code
     * 
     * @param command Command to execute
     * @return Exit code (0 for success)
     */
    static int executeCommand(const std::string& command);

    /**
     * Execute a system command and capture output
     * 
     * @param command Command to execute
     * @return Output from the command
     */
    static std::string executeCommandWithOutput(const std::string& command);
};

#endif // NETWORK_INTERFACE_CONFIGURATOR_HPP
