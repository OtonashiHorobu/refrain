// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Payload types shared by several protocol stages.

module refrain::protocol::v777::common;
version 777;

/// One entry of the client/server known-pack exchange.
struct known_pack {
    pack_namespace: string;
    id: string;
    version: string;
}

enum chat_visibility : u32 {
    full = 0,
    system = 1,
    hidden = 2,
}

enum humanoid_arm : u32 {
    left = 0,
    right = 1,
}

enum particle_status : u32 {
    all = 0,
    decreased = 1,
    minimal = 2,
}

/// The client's display and accessibility settings. The same payload is sent
/// during configuration and again from the play stage when settings change.
struct client_information_data {
    #[max(16)] language: string;
    view_distance: u8;
    visibility: chat_visibility;
    chat_colors: bool;
    model_customization: u8;
    main_hand: humanoid_arm;
    text_filtering: bool;
    allows_listing: bool;
    particles: particle_status;
}

/// Client acknowledgement state for a pushed resource pack.
enum resource_pack_action : u32 {
    successfully_loaded = 0,
    declined = 1,
    failed_download = 2,
    accepted = 3,
    downloaded = 4,
    invalid_url = 5,
    failed_reload = 6,
    discarded = 7,
}
