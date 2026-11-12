// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Recording gateway for subsystem tests. It implements the same outbound
// surface as the live listener without opening a socket, and feeds the
// container events a test needs next.

#pragma once

#include <refrain/server/play_gateway.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace refrain::test_support {

struct sent_body {
    std::uint64_t connection{0};
    std::vector<std::byte> body{};
};

struct sent_chunk_batch {
    std::uint64_t connection{0};
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
    std::vector<std::vector<std::byte>> packets{};
    bool update_center{false};
};

struct forgotten_chunk {
    std::uint64_t connection{0};
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
};

struct sent_container_content {
    std::uint64_t connection{0};
    std::int32_t container_id{0};
    std::int32_t state_id{0};
    std::vector<refrain::net::item_stack> items{};
    refrain::net::item_stack carried{};
};

struct sent_container_slot {
    std::uint64_t connection{0};
    std::int32_t container_id{0};
    std::int32_t state_id{0};
    std::int16_t slot{0};
    refrain::net::item_stack item{};
};

struct sent_cursor_item {
    std::uint64_t connection{0};
    refrain::net::item_stack item{};
};

class fake_play_gateway final : public refrain::server::play_gateway {
  public:
    explicit fake_play_gateway(bool available = true) : available_{available} {}

    [[nodiscard]] auto available() const -> bool override { return available_; }

    void send_raw(std::uint64_t connection,
                  std::span<const std::byte> body) override {
        sent.push_back(sent_body{
            .connection = connection,
            .body = std::vector<std::byte>{body.begin(), body.end()},
        });
    }

    [[nodiscard]] auto active_connections() const
        -> std::vector<std::uint64_t> override {
        return active;
    }

    [[nodiscard]] auto backlog(std::uint64_t connection) const
        -> std::size_t override {
        static_cast<void>(connection);
        return 0;
    }

    void send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                          std::int32_t chunk_z,
                          std::span<const std::vector<std::byte>> packets,
                          bool update_center) override {
        sent_chunk_batch batch{
            .connection = connection,
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .packets = std::vector<std::vector<std::byte>>{packets.begin(),
                                                           packets.end()},
            .update_center = update_center,
        };
        chunk_batches.push_back(std::move(batch));
    }

    void send_forget_chunk(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z) override {
        forgotten.push_back(forgotten_chunk{
            .connection = connection,
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
        });
    }

    [[nodiscard]] auto take_creative_slots()
        -> std::vector<refrain::net::creative_slot_event> override {
        auto drained{std::move(creative_slots)};
        creative_slots.clear();
        return drained;
    }

    [[nodiscard]] auto take_container_closes()
        -> std::vector<refrain::net::container_close_event> override {
        auto drained{std::move(container_closes)};
        container_closes.clear();
        return drained;
    }

    [[nodiscard]] auto take_container_clicks()
        -> std::vector<refrain::net::container_click_event> override {
        auto drained{std::move(container_clicks)};
        container_clicks.clear();
        return drained;
    }

    void
    send_container_content(std::uint64_t connection, std::int32_t container_id,
                           std::int32_t state_id,
                           std::span<const refrain::net::item_stack> items,
                           const refrain::net::item_stack &carried) override {
        container_content.push_back(sent_container_content{
            .connection = connection,
            .container_id = container_id,
            .state_id = state_id,
            .items = std::vector<refrain::net::item_stack>{items.begin(),
                                                           items.end()},
            .carried = carried,
        });
    }

    void
    send_container_set_slot(std::uint64_t connection, std::int32_t container_id,
                            std::int32_t state_id, std::int16_t slot,
                            const refrain::net::item_stack &item) override {
        container_slots.push_back(sent_container_slot{
            .connection = connection,
            .container_id = container_id,
            .state_id = state_id,
            .slot = slot,
            .item = item,
        });
    }

    void send_cursor_item(std::uint64_t connection,
                          const refrain::net::item_stack &item) override {
        cursor_items.push_back(sent_cursor_item{
            .connection = connection,
            .item = item,
        });
    }

    void set_available(bool value) { available_ = value; }

    std::vector<std::uint64_t> active{};
    std::vector<sent_body> sent{};
    std::vector<sent_chunk_batch> chunk_batches{};
    std::vector<forgotten_chunk> forgotten{};
    std::vector<refrain::net::creative_slot_event> creative_slots{};
    std::vector<refrain::net::container_close_event> container_closes{};
    std::vector<refrain::net::container_click_event> container_clicks{};
    std::vector<sent_container_content> container_content{};
    std::vector<sent_container_slot> container_slots{};
    std::vector<sent_cursor_item> cursor_items{};

  private:
    bool available_{true};
};

} // namespace refrain::test_support
