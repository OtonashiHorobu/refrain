// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/play_gateway.hpp>

namespace refrain::server {

server_gateway::server_gateway(refrain::net::server *listener)
    : listener_(listener) {}

void server_gateway::reset(refrain::net::server *listener) {
    listener_ = listener;
}

auto server_gateway::available() const -> bool { return listener_ != nullptr; }

void server_gateway::send_raw(std::uint64_t connection,
                              std::span<const std::byte> body) {
    if (listener_ != nullptr) {
        listener_->send_raw(connection, body);
    }
}

void server_gateway::send_raw_many(std::span<const std::uint64_t> connections,
                                   std::span<const std::byte> body) {
    if (listener_ != nullptr) {
        listener_->send_raw_many(connections, body);
    }
}

auto server_gateway::active_connections() const -> std::vector<std::uint64_t> {
    if (listener_ == nullptr) {
        return {};
    }
    return listener_->active_connections();
}

auto server_gateway::backlog(std::uint64_t connection) const -> std::size_t {
    if (listener_ == nullptr) {
        return 0;
    }
    return listener_->backlog(connection);
}

void server_gateway::send_chunk_batch(
    std::uint64_t connection, std::int32_t chunk_x, std::int32_t chunk_z,
    std::span<const std::vector<std::byte>> packets, bool update_center) {
    if (listener_ != nullptr) {
        listener_->send_chunk_batch(connection, chunk_x, chunk_z, packets,
                                    update_center);
    }
}

void server_gateway::send_forget_chunk(std::uint64_t connection,
                                       std::int32_t chunk_x,
                                       std::int32_t chunk_z) {
    if (listener_ != nullptr) {
        listener_->send_forget_chunk(connection, chunk_x, chunk_z);
    }
}

auto server_gateway::take_creative_slots()
    -> std::vector<refrain::net::creative_slot_event> {
    if (listener_ == nullptr) {
        return {};
    }
    return listener_->take_creative_slots();
}

auto server_gateway::take_container_closes()
    -> std::vector<refrain::net::container_close_event> {
    if (listener_ == nullptr) {
        return {};
    }
    return listener_->take_container_closes();
}

auto server_gateway::take_container_clicks()
    -> std::vector<refrain::net::container_click_event> {
    if (listener_ == nullptr) {
        return {};
    }
    return listener_->take_container_clicks();
}

void server_gateway::send_container_content(
    std::uint64_t connection, std::int32_t container_id, std::int32_t state_id,
    std::span<const refrain::net::item_stack> items,
    const refrain::net::item_stack &carried) {
    if (listener_ != nullptr) {
        listener_->send_container_content(connection, container_id, state_id,
                                          items, carried);
    }
}

void server_gateway::send_container_set_slot(
    std::uint64_t connection, std::int32_t container_id, std::int32_t state_id,
    std::int16_t slot, const refrain::net::item_stack &item) {
    if (listener_ != nullptr) {
        listener_->send_container_set_slot(connection, container_id, state_id,
                                           slot, item);
    }
}

void server_gateway::send_cursor_item(std::uint64_t connection,
                                      const refrain::net::item_stack &item) {
    if (listener_ != nullptr) {
        listener_->send_cursor_item(connection, item);
    }
}

} // namespace refrain::server
