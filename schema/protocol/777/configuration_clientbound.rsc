// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Configuration phase, clientbound. Packets whose payload carries NBT
// (registry data, tags, disconnect components) keep hand-written codecs.

module refrain::protocol::v777::configuration::clientbound;
version 777;
import refrain::protocol::v777::wire;

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

// Generated protocol layouts.

packet cookie_request = 0 {
    key: identifier;
}

packet reset_chat = 6 {
}

packet resource_pack_pop = 8 {
    id: optional<uuid>;
}

packet resource_pack_push = 9 {
    id: uuid;
    #[max(32767)] url: string;
    #[max(40)] hash: string;
    required: bool;
    prompt: optional<text_component>;
}

packet post_effects = 10 {
    post_effects: array<identifier>;
}

packet transfer = 12 {
    #[max(32767)] host: string;
    #[varint] port: i32;
}

packet clear_dialog = 18 {
}

packet code_of_conduct = 20 {
    #[max(32767)] code_of_conduct: string;
}
