// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Login phase, clientbound.

module refrain::protocol::v777::login::clientbound;
version 777;

struct profile_property {
    name: string;
    value: string;
    signature: optional<string>;
}

packet disconnect = 0 {
    reason: string;
}

packet hello = 1 {
    #[max(20)] server_id: string;
    public_key: bytes;
    challenge: bytes;
    should_authenticate: bool;
}

packet login_finished = 2 {
    profile_high: i64;
    profile_low: i64;
    #[max(16)] name: string;
    properties: array<profile_property>;
    session_high: i64;
    session_low: i64;
}

packet login_compression = 3 {
    #[varint] threshold: u32;
}
