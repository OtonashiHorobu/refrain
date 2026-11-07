// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Login phase, serverbound.

module refrain::protocol::v777::login::serverbound;
version 777;

packet hello = 0 {
    #[max(16)] name: string;
    profile_high: i64;
    profile_low: i64;
}

packet key = 1 {
    keybytes: bytes;
    encrypted_challenge: bytes;
}

packet login_acknowledged = 3 {
}
