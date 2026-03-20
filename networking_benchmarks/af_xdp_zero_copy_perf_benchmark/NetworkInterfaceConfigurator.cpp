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

#include "NetworkInterfaceConfigurator.hpp"
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

bool NetworkInterfaceConfigurator::hasRootPrivileges() {
    return getuid() == 0;
}

int NetworkInterfaceConfigurator::configureForXdp(const std::string& interfaceName) {
    if (!hasRootPrivileges()) {
        std::cerr << "Warning: Not running as root, network interface configuration may fail" << std::endl;
    }
    
    // Check if interface exists
    std::string checkCmd = "ip link show " + interfaceName + " > /dev/null 2>&1";
    if (executeCommand(checkCmd) != 0) {
        throw std::runtime_error("Interface " + interfaceName + " does not exist");
    }
    
    // Enable multicast on the interface
    std::string multicastCmd = "ip link set " + interfaceName + " multicast on";
    if (executeCommand(multicastCmd) != 0) {
        std::cerr << "Warning: Failed to enable multicast on " << interfaceName << std::endl;
    }
    
    // Enable multicast forwarding
    std::string forwardingCmd = "sysctl -w net.ipv4.conf." + interfaceName + ".mc_forwarding=1 > /dev/null 2>&1";
    if (executeCommand(forwardingCmd) != 0) {
        std::cerr << "Warning: Failed to enable multicast forwarding on " << interfaceName << std::endl;
    }
    
    // Determine driver type and recommended headroom
    std::string driverName = getDriverName(interfaceName);
    int headroom = determineHeadroom(driverName);
    
    std::cout << "Interface " << interfaceName << " uses driver " << driverName
              << ", recommended headroom: " << headroom << std::endl;
    
    return headroom;
}

std::string NetworkInterfaceConfigurator::getDriverName(const std::string& interfaceName) {
    std::string command = "ethtool -i " + interfaceName + " 2>/dev/null | grep '^driver:' | cut -d: -f2 | tr -d ' '";
    std::string output = executeCommandWithOutput(command);
    
    if (output.empty()) {
        std::cerr << "Warning: Failed to determine driver for " << interfaceName << std::endl;
        return "unknown";
    }
    
    // Remove trailing newline
    if (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }
    
    return output;
}

int NetworkInterfaceConfigurator::determineHeadroom(const std::string& driverName) {
    // Common driver-specific headroom values
    if (driverName == "i40e" || driverName == "ixgbe" || driverName == "ixgbevf") {
        return 256; // Intel drivers typically use 256B headroom
    } else if (driverName == "ena") {
        return 0;   // Amazon ENA driver
    } else if (driverName == "mlx5_core") {
        return 192; // Mellanox drivers
    } else if (driverName == "ice") {
        return 128; // Intel E810 driver
    } else if (driverName == "e1000" || driverName == "e1000e") {
        return 32;  // Old Intel driver
    } else {
        return 0;   // Default to 0 for unknown drivers
    }
}

void NetworkInterfaceConfigurator::optimizeForXdp(const std::string& interfaceName) {
    if (!hasRootPrivileges()) {
        std::cerr << "Warning: Not running as root, network optimization may fail" << std::endl;
        return;
    }
    
    // Increase ring buffer sizes if possible
    std::string ringBufferCmd = "ethtool -G " + interfaceName + " rx 4096 > /dev/null 2>&1";
    if (executeCommand(ringBufferCmd) != 0) {
        // This is normal for some drivers
    }
    
    // Disable GRO, GSO, TSO, and LRO if possible
    std::vector<std::string> offloadCmds = {
        "ethtool -K " + interfaceName + " gro off > /dev/null 2>&1",
        "ethtool -K " + interfaceName + " gso off > /dev/null 2>&1",
        "ethtool -K " + interfaceName + " tso off > /dev/null 2>&1",
        "ethtool -K " + interfaceName + " lro off > /dev/null 2>&1"
    };
    
    for (const auto& cmd : offloadCmds) {
        executeCommand(cmd);
        // Ignore failures as some settings may not be supported
    }
    
    std::cout << "Applied basic XDP optimizations to " << interfaceName << std::endl;
}

int NetworkInterfaceConfigurator::executeCommand(const std::string& command) {
    int result = std::system(command.c_str());
    if (result == -1) {
        return -1;
    }
    return WEXITSTATUS(result);
}

std::string NetworkInterfaceConfigurator::executeCommandWithOutput(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
    pclose(pipe);
    return result;
}
