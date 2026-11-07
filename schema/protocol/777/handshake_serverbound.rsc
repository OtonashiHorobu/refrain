// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Handshake phase, serverbound.

module refrain::protocol::v777::handshake::serverbound;
version 777;

packet intention = 0 {
    #[varint] protocol: u32;
    host: string;
    port: u16;
    #[varint] next: u32;
}
