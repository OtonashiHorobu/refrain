// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Status phase, clientbound packets.

module refrain::protocol::v777::status::clientbound;
version 777;

/// Server list response: a JSON document carried as one length-prefixed string.
packet status_response = 0 {
    json: string;
}

/// Echo of the client's ping payload.
packet pong_response = 1 {
    time: i64;
}
