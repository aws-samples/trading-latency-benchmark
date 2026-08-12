/*
 * ControlPort.hpp — single source of truth for the control-protocol UDP port.
 *
 * Defaults to 12345 (production). Override with the AFXDP_CONTROL_PORT env var
 * so integration tests can run on a non-production port without colliding with
 * a live replicator.service (which binds 12345 on INADDR_ANY). Used by the
 * replicator (server), the echo-mode echo server, rtt and
 * replicator_ctl (clients) so all four always agree on the port.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 */
#pragma once

#include <cstdint>
#include <cstdlib>

static constexpr uint16_t AFXDP_DEFAULT_CONTROL_PORT = 12345;

// Resolve the control port: AFXDP_CONTROL_PORT env if valid (1..65535), else default.
inline uint16_t afxdp_control_port() {
    const char* e = getenv("AFXDP_CONTROL_PORT");
    if (e && *e) {
        long p = strtol(e, nullptr, 10);
        if (p > 0 && p < 65536) {
            return static_cast<uint16_t>(p);
        }
    }
    return AFXDP_DEFAULT_CONTROL_PORT;
}
