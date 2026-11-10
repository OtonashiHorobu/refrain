// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Status phase, serverbound packets.

module refrain::protocol::v777::status::serverbound;
version 777;

/// Empty status request.
packet status_request = 0 {
}

/// Latency probe payload.
packet ping_request = 1 {
    time: i64;
}
