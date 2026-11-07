// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Configuration phase, serverbound.

module refrain::protocol::v777::configuration::serverbound;
version 777;
import refrain::protocol::v777::wire;

import refrain::protocol::v777::common;

packet client_information = 0 {
    information: client_information_data;
}

packet cookie_response = 1 {
    key: identifier;
    payload: optional<bytes>;
}

packet custom_payload = 2 {
    channel: identifier;
    #[rest] data: bytes;
}

packet finish_configuration = 3 {
}

packet keep_alive = 4 {
    id: i64;
}

packet pong = 5 {
    #[fixed] id: i32;
}

packet resource_pack = 6 {
    id: uuid;
    action: resource_pack_action;
}

packet select_known_packs = 7 {
    packs: array<known_pack>;
}

packet accept_code_of_conduct = 9 {
}

// Generated protocol layouts.

packet custom_click_action = 8 {
    id: identifier;
    payload: nbt;
}
