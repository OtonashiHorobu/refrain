// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Gateway that tallies the packets a subsystem emitted instead of keeping
// their bodies. A storm test that retained every body would measure the
// recorder rather than the subsystem.

#pragma once

#include <refrain/net/base.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/play_gateway.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace refrain::test_support {

class counting_play_gateway final : public refrain::server::play_gateway {
  public:
    [[nodiscard]] auto available() const -> bool override { return true; }

    void send_raw(std::uint64_t connection,
                  std::span<const std::byte> body) override {
        last_connection = connection;
        bytes += body.size();
        tally(body);
    }

    /// A shared body reaches every connection of the cohort, so its packets
    /// are tallied once and the cohort size is recorded separately.
    void send_raw_many(std::span<const std::uint64_t> connections,
                       std::span<const std::byte> body) override {
        ++shared_sends;
        shared_recipients += connections.size();
        shared_bytes += body.size();
        bytes += body.size() * connections.size();
        // The counters describe delivered packets, so a shared body counts
        // once per recipient.
        for (std::size_t index{0}; index < connections.size(); ++index) {
            tally(body);
        }
    }

    [[nodiscard]] auto active_connections() const
        -> std::vector<std::uint64_t> override {
        return {};
    }

  private:
    void tally(std::span<const std::byte> body) {
        namespace play_cb = protocol::current::play::clientbound;
        net::reader in{body};
        while (!in.empty()) {
            const auto packet_id{in.read_varint()};
            if (!packet_id) {
                ++malformed;
                return;
            }
            if (*packet_id == play_cb::player_info_update::packet_id) {
                const auto actions{in.read_u8()};
                const auto count{in.read_varint()};
                if (!actions || !count) {
                    ++malformed;
                    return;
                }
                // The broadcaster encodes each player-list update as its own
                // body, so the entry count is all this tally needs.
                ++info_packets;
                info_entries += *count;
                return;
            }
            if (*packet_id == play_cb::player_info_remove::packet_id) {
                ++info_remove_packets;
                return;
            }
            if (*packet_id == play_cb::add_entity::packet_id) {
                play_cb::add_entity value{};
                if (!play_cb::decode_add_entity(in, value)) {
                    ++malformed;
                    return;
                }
                ++spawn_packets;
                continue;
            }
            if (*packet_id == play_cb::remove_entities::packet_id) {
                play_cb::remove_entities value{};
                if (!play_cb::decode_remove_entities(in, value)) {
                    ++malformed;
                    return;
                }
                ++remove_packets;
                continue;
            }
            if (*packet_id == play_cb::teleport_entity::packet_id) {
                play_cb::teleport_entity value{};
                if (!play_cb::decode_teleport_entity(in, value)) {
                    ++malformed;
                    return;
                }
                ++move_packets;
                continue;
            }
            if (*packet_id == play_cb::move_entity_pos::packet_id) {
                play_cb::move_entity_pos value{};
                if (!play_cb::decode_move_entity_pos(in, value)) {
                    ++malformed;
                    return;
                }
                ++move_packets;
                continue;
            }
            if (*packet_id == play_cb::move_entity_pos_rot::packet_id) {
                play_cb::move_entity_pos_rot value{};
                if (!play_cb::decode_move_entity_pos_rot(in, value)) {
                    ++malformed;
                    return;
                }
                ++move_packets;
                continue;
            }
            if (*packet_id == play_cb::move_entity_rot::packet_id) {
                play_cb::move_entity_rot value{};
                if (!play_cb::decode_move_entity_rot(in, value)) {
                    ++malformed;
                    return;
                }
                ++move_packets;
                continue;
            }
            ++other_packets;
            return;
        }
    }

  public:
    [[nodiscard]] auto backlog(std::uint64_t) const -> std::size_t override {
        return 0;
    }
    void send_chunk_batch(std::uint64_t, std::int32_t, std::int32_t,
                          std::span<const std::vector<std::byte>>,
                          bool) override {}
    void send_forget_chunk(std::uint64_t, std::int32_t, std::int32_t) override {
    }
    [[nodiscard]] auto take_creative_slots()
        -> std::vector<net::creative_slot_event> override {
        return {};
    }
    [[nodiscard]] auto take_container_closes()
        -> std::vector<net::container_close_event> override {
        return {};
    }
    [[nodiscard]] auto take_container_clicks()
        -> std::vector<net::container_click_event> override {
        return {};
    }
    void send_container_content(std::uint64_t, std::int32_t, std::int32_t,
                                std::span<const net::item_stack>,
                                const net::item_stack &) override {}
    void send_container_set_slot(std::uint64_t, std::int32_t, std::int32_t,
                                 std::int16_t,
                                 const net::item_stack &) override {}
    void send_cursor_item(std::uint64_t, const net::item_stack &) override {}

    void reset() {
        info_packets = 0;
        info_entries = 0;
        info_remove_packets = 0;
        spawn_packets = 0;
        remove_packets = 0;
        move_packets = 0;
        other_packets = 0;
        malformed = 0;
        bytes = 0;
        last_connection = 0;
        shared_sends = 0;
        shared_recipients = 0;
        shared_bytes = 0;
    }

    std::size_t info_packets{0};
    std::size_t info_entries{0};
    std::size_t info_remove_packets{0};
    std::size_t spawn_packets{0};
    std::size_t remove_packets{0};
    std::size_t move_packets{0};
    std::size_t other_packets{0};
    std::size_t malformed{0};
    std::size_t bytes{0};
    std::uint64_t last_connection{0};
    /// Shared-body sends and the connections they reached.
    std::size_t shared_sends{0};
    std::size_t shared_recipients{0};
    std::size_t shared_bytes{0};
};

} // namespace refrain::test_support
