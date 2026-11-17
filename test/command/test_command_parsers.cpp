// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class parser_source final : public refrain::command::source {
  public:
    explicit parser_source(refrain::command::permission_level required =
                               refrain::command::level::owner)
        : level_{required} {}

    [[nodiscard]] auto identity() const noexcept
        -> refrain::command::source_identity override {
        return refrain::command::source_identity{std::string_view{"parser"}, 5U,
                                                 true};
    }
    [[nodiscard]] auto permission() const noexcept
        -> refrain::command::permission_level override {
        return level_;
    }
    void reply(std::string_view message) override {
        replies.emplace_back(message);
    }

    std::vector<std::string> replies{};

  private:
    refrain::command::permission_level level_;
};

/// Parse `input` with one parser and require that the parser consumed all of
/// it, which is the contract the dispatcher enforces.
template <typename Parser>
[[nodiscard]] auto parse_all(const Parser &parser, std::string_view input)
    -> std::expected<refrain::command::argument_value,
                     refrain::command::argument_error> {
    refrain::command::reader in{input};
    auto value = parser.parse(in);
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    if (!in.at_end()) {
        return std::unexpected{refrain::command::make_argument_error(
            "parser stopped before the end of the input", in.cursor())};
    }
    return std::move(*value);
}

template <typename Parser>
[[nodiscard]] auto parse_ok(const Parser &parser, std::string_view input)
    -> bool {
    const auto parsed = parse_all(parser, input);
    return parsed.has_value();
}

/// Register `name` as a one-argument literal command and run `input` through
/// the dispatcher, so parse errors are reported with the reference's
/// `<reason> at position <byte>` form.
template <typename Parser>
[[nodiscard]] auto dispatch_error(Parser parser, std::string_view name,
                                  std::string_view input, parser_source &source)
    -> std::string {
    refrain::command::dispatcher dispatcher;
    dispatcher.literal(name).then(
        dispatcher.argument("value", std::move(parser))
            .executes([](refrain::command::context &) { return 1; }));
    auto outcome = dispatcher.execute(input, source);
    if (outcome) {
        return {};
    }
    return outcome.error().message;
}

[[maybe_unused]] const suite command_parser_tests = [] {
    "bool parser accepts exactly the two literals"_test = [] {
        const refrain::command::bool_parser parser{};
        const auto yes = parse_all(parser, "true");
        expect(yes.has_value());
        if (yes) {
            const auto *value = std::get_if<bool>(&*yes);
            expect(value != nullptr && *value);
        }
        const auto no = parse_all(parser, "false");
        expect(no.has_value() && std::get<bool>(*no) == false);
        const auto equal = parse_all(parser, "TRUE");
        expect(!equal.has_value());
        if (!equal) {
            expect(equal.error().message.find("not a valid boolean") !=
                   std::string::npos);
        }
        expect(!parse_all(parser, "").has_value());
    };

    "integer parser enforces bounds and the 64-bit range"_test = [] {
        const auto bounded = refrain::command::integer_arg(-5, 5);
        const auto inside = parse_all(bounded, "-3");
        expect(inside.has_value());
        if (inside) {
            expect(std::get<std::int64_t>(*inside) == -3);
        }
        const auto below = parse_all(bounded, "-6");
        expect(!below.has_value());
        if (!below) {
            expect(below.error().message.find("below the minimum") !=
                   std::string::npos);
        }
        const auto above = parse_all(bounded, "6");
        expect(above.has_value() == false);
        if (!above) {
            expect(above.error().message.find("above the maximum") !=
                   std::string::npos);
        }
        const auto long_parser = refrain::command::long_arg();
        const auto biggest = parse_all(long_parser, "9223372036854775807");
        expect(biggest.has_value());
        expect(!parse_all(long_parser, "9223372036854775808").has_value());
        expect(!parse_all(long_parser, "3.5").has_value());
    };

    "number parsers enforce their domain and precision"_test = [] {
        const auto ratio = refrain::command::double_arg(0.0, 1.0);
        const auto half = parse_all(ratio, "0.5");
        expect(half.has_value());
        if (half) {
            expect(std::get<double>(*half) == 0.5);
        }
        expect(!parse_all(ratio, "1.5").has_value());
        expect(!parse_all(ratio, "nan").has_value());
        expect(!parse_all(ratio, "1e999").has_value());

        const auto single = refrain::command::float_arg();
        expect(parse_all(single, "1.5").has_value());
        expect(!parse_all(single, "1e39").has_value());
    };

    "resource location parser canonicalizes the default namespace"_test = [] {
        const auto parser = refrain::command::resource_location_arg();
        const auto plain = parse_all(parser, "stone");
        expect(plain.has_value());
        if (plain) {
            expect(std::get<refrain::data::resource_location>(*plain)
                       .to_string() == "minecraft:stone");
        }
        expect(!parse_all(parser, "Bad").has_value());
        expect(!parse_all(parser, "minecraft:block//stone").has_value());
    };

    "block position parser reads three coordinates and their form"_test = [] {
        const auto parser = refrain::command::block_position_arg();

        const auto absolute = parse_all(parser, "1 2 -3");
        expect(absolute.has_value());
        if (absolute) {
            const auto &pos =
                std::get<refrain::command::block_position>(*absolute);
            expect(pos.x == 1.0 && pos.y == 2.0 && pos.z == -3.0);
            expect(!pos.relative_x && !pos.relative_y && !pos.relative_z);
            expect(!pos.local);
        }

        const auto relative = parse_all(parser, "~1 ~ ~-2.5");
        expect(relative.has_value());
        if (relative) {
            const auto &pos =
                std::get<refrain::command::block_position>(*relative);
            expect(pos.relative_x && pos.relative_y && pos.relative_z);
            expect(pos.x == 1.0 && pos.y == 0.0 && pos.z == -2.5);
        }

        const auto local = parse_all(parser, "^ ^1 ^-2");
        expect(local.has_value());
        if (local) {
            const auto &pos =
                std::get<refrain::command::block_position>(*local);
            expect(pos.local);
            expect(pos.x == 0.0 && pos.y == 1.0 && pos.z == -2.0);
        }

        expect(!parse_all(parser, "1 2").has_value());
        expect(!parse_all(parser, "1 2 3 4").has_value());
        expect(!parse_all(parser, "^ 1 2").has_value());
        expect(!parse_all(parser, "1.5 2 3").has_value());
        const auto mixed = parse_all(parser, "^ ~ ~");
        expect(!mixed.has_value());
        if (!mixed) {
            expect(mixed.error().message.find("cannot mix") !=
                   std::string::npos);
        }
    };

    "rotation parser reads yaw and pitch"_test = [] {
        const auto parser = refrain::command::rotation_arg();
        const auto absolute = parse_all(parser, "90 -45");
        expect(absolute.has_value());
        if (absolute) {
            const auto &rot = std::get<refrain::command::rotation>(*absolute);
            expect(rot.yaw == 90.0 && rot.pitch == -45.0);
            expect(!rot.relative_yaw && !rot.relative_pitch);
        }
        const auto relative = parse_all(parser, "~ ~1.5");
        expect(relative.has_value());
        if (relative) {
            const auto &rot = std::get<refrain::command::rotation>(*relative);
            expect(rot.relative_yaw && rot.relative_pitch);
            expect(rot.pitch == 1.5);
        }
        expect(!parse_all(parser, "90").has_value());
        expect(!parse_all(parser, "90 45 1").has_value());
    };

    "player name parser reads a bare name and rejects selectors"_test = [] {
        const auto parser = refrain::command::player_name_arg();
        const auto name = parse_all(parser, "Steve");
        expect(name.has_value());
        if (name) {
            expect(std::get<std::string_view>(*name) == "Steve");
        }
        expect(!parse_all(parser, "").has_value());
        const auto selector = parse_all(parser, "@a");
        expect(!selector.has_value());
        if (!selector) {
            expect(selector.error().message.find("selectors") !=
                   std::string::npos);
        }
    };

    "gamemode parser returns the typed mode"_test = [] {
        const auto parser = refrain::command::gamemode_arg();
        const auto creative = parse_all(parser, "Creative");
        expect(creative.has_value());
        if (creative) {
            expect(std::get<refrain::command::game_mode>(*creative) ==
                   refrain::command::game_mode::creative);
        }
        const auto numeric = parse_all(parser, "2");
        expect(numeric.has_value());
        if (numeric) {
            expect(std::get<refrain::command::game_mode>(*numeric) ==
                   refrain::command::game_mode::adventure);
        }
        const auto invalid = parse_all(parser, "builder");
        expect(!invalid.has_value());
        if (!invalid) {
            expect(invalid.error().message.find("unknown game mode") !=
                   std::string::npos);
        }
    };

    "time parser applies the reference unit factors and minimum"_test = [] {
        const auto parser = refrain::command::time_arg(0);
        const auto ticks = parse_all(parser, "20t");
        expect(ticks.has_value());
        if (ticks) {
            expect(std::get<std::int64_t>(*ticks) == 20);
        }
        const auto seconds = parse_all(parser, "1s");
        expect(seconds.has_value());
        if (seconds) {
            expect(std::get<std::int64_t>(*seconds) == 20);
        }
        const auto days = parse_all(parser, "1d");
        expect(days.has_value());
        if (days) {
            expect(std::get<std::int64_t>(*days) == 24'000);
        }
        const auto bare = parse_all(parser, "100");
        expect(bare.has_value());
        if (bare) {
            expect(std::get<std::int64_t>(*bare) == 100);
        }
        const auto rounded = parse_all(parser, "0.5s");
        expect(rounded.has_value());
        if (rounded) {
            expect(std::get<std::int64_t>(*rounded) == 10);
        }
        const auto unit = parse_all(parser, "20x");
        expect(!unit.has_value());
        if (!unit) {
            expect(unit.error().message.find("unknown time unit") !=
                   std::string::npos);
        }
        const auto bounded = refrain::command::time_arg(5);
        const auto too_low = parse_all(bounded, "4t");
        expect(!too_low.has_value());
        if (!too_low) {
            expect(too_low.error().message.find("below the minimum") !=
                   std::string::npos);
        }
        const auto negative_parser = refrain::command::time_arg(
            std::numeric_limits<std::int32_t>::min());
        const auto negative = parse_all(negative_parser, "-1s");
        expect(negative.has_value());
        if (negative) {
            expect(std::get<std::int64_t>(*negative) == -20);
        }
    };

    "time parser accepts the world clock markers"_test = [] {
        const auto parser = refrain::command::time_arg(
            0, refrain::command::world_time_markers());
        const auto day = parse_all(parser, "day");
        expect(day.has_value());
        if (day) {
            expect(std::get<std::int64_t>(*day) == 1'000);
        }
        const auto noon = parse_all(parser, "NOON");
        expect(noon.has_value());
        if (noon) {
            expect(std::get<std::int64_t>(*noon) == 6'000);
        }
        const auto midnight = parse_all(parser, "midnight");
        expect(midnight.has_value());
        if (midnight) {
            expect(std::get<std::int64_t>(*midnight) == 18'000);
        }
    };

    "dimension parser canonicalizes dimension ids"_test = [] {
        const auto parser = refrain::command::dimension_arg();
        const auto full = parse_all(parser, "minecraft:the_end");
        expect(full.has_value());
        if (full) {
            expect(
                std::get<refrain::data::resource_location>(*full).to_string() ==
                "minecraft:the_end");
        }
        const auto bare = parse_all(parser, "nether");
        expect(bare.has_value());
        if (bare) {
            expect(
                std::get<refrain::data::resource_location>(*bare).to_string() ==
                "minecraft:nether");
        }
        expect(!parse_all(parser, "Not a Dimension").has_value());
    };

    "uuid parser round trips the canonical form"_test = [] {
        const auto parser = refrain::command::uuid_arg();
        const auto parsed =
            parse_all(parser, "dd12be42-52a9-4a91-a8a1-11c01849e498");
        expect(parsed.has_value());
        if (parsed) {
            expect(std::get<refrain::command::uuid>(*parsed).to_string() ==
                   "dd12be42-52a9-4a91-a8a1-11c01849e498");
        }
        const auto upper =
            parse_all(parser, "DD12BE42-52A9-4A91-A8A1-11C01849E498");
        expect(upper.has_value());
        if (upper) {
            expect(std::get<refrain::command::uuid>(*upper).to_string() ==
                   "dd12be42-52a9-4a91-a8a1-11c01849e498");
        }
        expect(!parse_all(parser, "dd12be42-52a9-4a91-a8a1").has_value());
        expect(!parse_all(parser, "zz12be42-52a9-4a91-a8a1-11c01849e498")
                    .has_value());
    };

    "parse failures reach the dispatcher with the reference position"_test =
        [] {
            parser_source source;
            const auto position =
                dispatch_error(refrain::command::block_position_arg(), "pos",
                               "pos 1 2", source);
            expect(position.find("expected a complete block position") !=
                   std::string::npos);
            expect(position.find("at position 4") != std::string::npos);

            const auto rotation = dispatch_error(
                refrain::command::rotation_arg(), "rot", "rot 90", source);
            expect(rotation.find("expected a complete rotation") !=
                   std::string::npos);
            expect(rotation.find("at position 4") != std::string::npos);

            const auto mode =
                dispatch_error(refrain::command::gamemode_arg(), "gamemode",
                               "gamemode builder", source);
            expect(mode.find("unknown game mode") != std::string::npos);
            expect(mode.find("at position 9") != std::string::npos);

            const auto uuid_error = dispatch_error(
                refrain::command::uuid_arg(), "uuid", "uuid nonsense", source);
            expect(uuid_error.find("not a valid uuid") != std::string::npos);
            expect(uuid_error.find("at position 5") != std::string::npos);

            const auto time_error = dispatch_error(
                refrain::command::time_arg(0), "time", "time 5x", source);
            expect(time_error.find("unknown time unit") != std::string::npos);
        };
};

} // namespace
