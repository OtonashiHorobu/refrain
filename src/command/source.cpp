// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/source.hpp>

#include <utility>

namespace refrain::command {

console_source::console_source(permission_level required, reply_sink sink)
    : permission_{required}, sink_{std::move(sink)} {}

console_source::console_source(permission_level required,
                               player_name_source online, reply_sink sink)
    : permission_{required}, online_{std::move(online)},
      sink_{std::move(sink)} {}

auto console_source::identity() const noexcept -> source_identity {
    return source_identity{std::string_view{"console"}, 0U, false};
}

auto console_source::permission() const noexcept -> permission_level {
    return permission_;
}

void console_source::reply(std::string_view message) {
    if (sink_) {
        sink_(message);
    }
}

auto console_source::online_players() const -> std::vector<std::string> {
    return online_ ? online_() : std::vector<std::string>{};
}

player_source::player_source(std::string name, std::uint64_t identity_key,
                             permission_level required, reply_sink sink,
                             player_name_source online)
    : name_{std::move(name)}, identity_key_{identity_key},
      permission_{required}, sink_{std::move(sink)},
      online_{std::move(online)} {}

auto player_source::identity() const noexcept -> source_identity {
    return source_identity{name_, identity_key_, true};
}

auto player_source::permission() const noexcept -> permission_level {
    return permission_;
}

void player_source::reply(std::string_view message) {
    if (sink_) {
        sink_(message);
    }
}

auto player_source::online_players() const -> std::vector<std::string> {
    return online_ ? online_() : std::vector<std::string>{};
}

} // namespace refrain::command
