// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: fbc9968bc3817b27

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/schema/protocol/777/common.hpp>

namespace refrain::protocol::v777::configuration::clientbound {

struct custom_payload {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "custom_payload";

    ::refrain::data::resource_location channel;
    std::vector<std::byte> data;
};

struct finish_configuration {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "finish_configuration";

};

struct keep_alive {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "keep_alive";

    std::int64_t id;
};

struct ping {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "ping";

    // attributes: #[fixed]
    std::int32_t id;
};

struct update_enabled_features {
    static constexpr std::uint16_t packet_id = 0xD;
    static constexpr std::string_view packet_name = "update_enabled_features";

    std::vector<::refrain::data::resource_location> features;
};

struct select_known_packs {
    static constexpr std::uint16_t packet_id = 0xF;
    static constexpr std::string_view packet_name = "select_known_packs";

    std::vector<::refrain::protocol::v777::common::known_pack> packs;
};

struct cookie_request {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "cookie_request";

    ::refrain::data::resource_location key;
};

struct disconnect {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "disconnect";

    ::refrain::net::text_component payload;
};

struct reset_chat {
    static constexpr std::uint16_t packet_id = 0x6;
    static constexpr std::string_view packet_name = "reset_chat";

};

struct registry_data {
    static constexpr std::uint16_t packet_id = 0x7;
    static constexpr std::string_view packet_name = "registry_data";

    ::refrain::data::resource_location payload;
};

struct resource_pack_pop {
    static constexpr std::uint16_t packet_id = 0x8;
    static constexpr std::string_view packet_name = "resource_pack_pop";

    std::optional<::refrain::net::uuid> id;
};

struct resource_pack_push {
    static constexpr std::uint16_t packet_id = 0x9;
    static constexpr std::string_view packet_name = "resource_pack_push";

    ::refrain::net::uuid id;
    // attributes: #[max(32767)]
    std::string url;
    // attributes: #[max(40)]
    std::string hash;
    bool required;
    std::optional<::refrain::net::text_component> prompt;
};

struct post_effects {
    static constexpr std::uint16_t packet_id = 0xA;
    static constexpr std::string_view packet_name = "post_effects";

    std::vector<::refrain::data::resource_location> post_effects;
};

struct transfer {
    static constexpr std::uint16_t packet_id = 0xC;
    static constexpr std::string_view packet_name = "transfer";

    // attributes: #[max(32767)]
    std::string host;
    // attributes: #[varint]
    std::int32_t port;
};

struct clear_dialog {
    static constexpr std::uint16_t packet_id = 0x12;
    static constexpr std::string_view packet_name = "clear_dialog";

};

struct code_of_conduct {
    static constexpr std::uint16_t packet_id = 0x14;
    static constexpr std::string_view packet_name = "code_of_conduct";

    // attributes: #[max(32767)]
    std::string code_of_conduct;
};
[[nodiscard]] inline auto encode_custom_payload(::refrain::net::writer &out, const custom_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.channel); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data.size()));
    out.write_bytes(std::span<const std::byte>{value.data });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_payload(::refrain::net::reader &in, custom_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.channel = std::move(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_finish_configuration(::refrain::net::writer &out, const finish_configuration &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_finish_configuration(::refrain::net::reader &in, finish_configuration &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_keep_alive(::refrain::net::writer &out, const keep_alive &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_keep_alive(::refrain::net::reader &in, keep_alive &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ping(::refrain::net::writer &out, const ping &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ping(::refrain::net::reader &in, ping &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_enabled_features(::refrain::net::writer &out, const update_enabled_features &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.features.size()));
    for (const auto &item_0 : value.features) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_enabled_features(::refrain::net::reader &in, update_enabled_features &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.features.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.features.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_select_known_packs(::refrain::net::writer &out, const select_known_packs &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.packs.size()));
    for (const auto &item_0 : value.packs) {
      if (auto written = ::refrain::protocol::v777::common::encode_known_pack(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_select_known_packs(::refrain::net::reader &in, select_known_packs &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.packs.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::common::known_pack value_0{};
      if (auto decoded = ::refrain::protocol::v777::common::decode_known_pack(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.packs.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_cookie_request(::refrain::net::writer &out, const cookie_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.key); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_cookie_request(::refrain::net::reader &in, cookie_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_disconnect(::refrain::net::writer &out, const disconnect &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_disconnect(::refrain::net::reader &in, disconnect &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_reset_chat(::refrain::net::writer &out, const reset_chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_reset_chat(::refrain::net::reader &in, reset_chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_registry_data(::refrain::net::writer &out, const registry_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_registry_data(::refrain::net::reader &in, registry_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.payload = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_resource_pack_pop(::refrain::net::writer &out, const resource_pack_pop &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.id.has_value());
    if (value.id.has_value()) {
      if (auto written = out.write_uuid(value.id.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_resource_pack_pop(::refrain::net::reader &in, resource_pack_pop &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::uuid value_0{};
      auto value_1 = in.read_uuid();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.id = std::move(value_0);
    } else {
      value.id.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_resource_pack_push(::refrain::net::writer &out, const resource_pack_push &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.id); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.url, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.hash, 40); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.required);
  }
  {
    out.write_bool(value.prompt.has_value());
    if (value.prompt.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.prompt.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_resource_pack_push(::refrain::net::reader &in, resource_pack_push &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.url = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(40);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hash = std::move(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.required = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.prompt = std::move(value_0);
    } else {
      value.prompt.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_post_effects(::refrain::net::writer &out, const post_effects &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.post_effects.size()));
    for (const auto &item_0 : value.post_effects) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_post_effects(::refrain::net::reader &in, post_effects &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.post_effects.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.post_effects.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_transfer(::refrain::net::writer &out, const transfer &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.host, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.port));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_transfer(::refrain::net::reader &in, transfer &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.host = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.port = static_cast<decltype(value.port)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_clear_dialog(::refrain::net::writer &out, const clear_dialog &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_clear_dialog(::refrain::net::reader &in, clear_dialog &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_code_of_conduct(::refrain::net::writer &out, const code_of_conduct &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.code_of_conduct, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_code_of_conduct(::refrain::net::reader &in, code_of_conduct &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.code_of_conduct = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::configuration::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_CONFIGURATION_CLIENTBOUND_HPP
