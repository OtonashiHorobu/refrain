// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace refrain::command {

/// Vanilla permission ladder. The order is meaningful: `has_permission()`
/// compares the source's level with the requested one.
enum class permission_level : std::uint8_t {
    everyone = 0,
    moderator = 1,
    gamemode = 2,
    admin = 3,
    owner = 4,
};

/// Named constants so registration reads as `.require(level::gamemode)`.
namespace level {

inline constexpr permission_level everyone = permission_level::everyone;
inline constexpr permission_level moderator = permission_level::moderator;
inline constexpr permission_level gamemode = permission_level::gamemode;
inline constexpr permission_level admin = permission_level::admin;
inline constexpr permission_level owner = permission_level::owner;

} // namespace level

/// Identity hook that is independent of the protocol and the world layer. The
/// key is opaque to the command module: a server stores whatever stable value
/// it uses for the player (a hash of the profile id, a session id, ...). The
/// name is only display text and must not be used as an identity key.
struct source_identity {
    std::string_view name{};
    std::uint64_t key{0};
    bool player{false};
};

/// A command source: who runs a command, what they may use, and where feedback
/// goes. The interface deliberately mentions no network or world types so a
/// caller can implement console, player, command-block or test sources without
/// linking those layers.
class source {
  public:
    source() = default;
    virtual ~source() = default;
    source(const source &) = delete;
    source &operator=(const source &) = delete;

    [[nodiscard]] virtual auto identity() const noexcept -> source_identity = 0;
    [[nodiscard]] virtual auto permission() const noexcept
        -> permission_level = 0;

    /// Permission predicate used by `.require(level)` and by tree/suggestion
    /// filtering. Override for non-ladder permission models.
    [[nodiscard]] virtual auto
    has_permission(permission_level required) const noexcept -> bool {
        return permission() >= required;
    }

    /// Deliver one feedback line. Implementations must not throw.
    virtual void reply(std::string_view message) = 0;

    [[nodiscard]] auto name() const noexcept -> std::string_view {
        return identity().name;
    }
};

using reply_sink = std::function<void(std::string_view)>;

/// Console source. Console commands traditionally run at the highest level.
class console_source final : public source {
  public:
    explicit console_source(permission_level required = level::owner,
                            reply_sink sink = {});

    [[nodiscard]] auto identity() const noexcept -> source_identity override;
    [[nodiscard]] auto permission() const noexcept -> permission_level override;
    void reply(std::string_view message) override;

  private:
    permission_level permission_;
    reply_sink sink_;
};

/// Player source implemented without the network or world layers. The caller
/// supplies the already-resolved permission level and an opaque identity key.
class player_source final : public source {
  public:
    player_source(std::string name, std::uint64_t identity_key,
                  permission_level required = level::everyone,
                  reply_sink sink = {});

    [[nodiscard]] auto identity() const noexcept -> source_identity override;
    [[nodiscard]] auto permission() const noexcept -> permission_level override;
    void reply(std::string_view message) override;

  private:
    std::string name_;
    std::uint64_t identity_key_;
    permission_level permission_;
    reply_sink sink_;
};

} // namespace refrain::command
