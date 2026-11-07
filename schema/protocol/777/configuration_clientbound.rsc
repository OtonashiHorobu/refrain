// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Configuration phase, clientbound. Packets whose payload carries NBT
// (registry data, tags, disconnect components) keep hand-written codecs.

module refrain::protocol::v777::configuration::clientbound;
version 777;

import refrain::protocol::v777::common;

packet custom_payload = 1 {
    channel: identifier;
    data: bytes;
}

packet finish_configuration = 3 {
}

packet keep_alive = 4 {
    id: i64;
}

packet ping = 5 {
    #[fixed] id: i32;
}

packet update_enabled_features = 13 {
    features: array<identifier>;
}

packet select_known_packs = 15 {
    packs: array<known_pack>;
}
