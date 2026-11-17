// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The entity selector parser and resolver: every selector family, every
// modelled filter, the parse errors with their byte offsets, the argument-form
// cardinality rules, completion and the resolution order.

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;

class selector_source final : public command::source {
  public:
    [[nodiscard]] auto identity() const noexcept
        -> command::source_identity override {
        return command::source_identity{std::string_view{"selector"}, 7U, true};
    }
    [[nodiscard]] auto permission() const noexcept
        -> command::permission_level override {
        return command::level::owner;
    }
    void reply(std::string_view) override {}
    [[nodiscard]] auto online_players() const
        -> std::vector<std::string> override {
        return {"Alice", "Bob"};
    }
};

[[nodiscard]] auto parse_selector(std::string_view input, bool single = false,
                                  bool players_only = false)
    -> std::expected<command::entity_selector, command::argument_error> {
    command::reader in{input};
    const command::entity_selector_parser parser{single, players_only};
    auto value{parser.parse(in)};
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    if (!in.at_end()) {
        return std::unexpected{command::make_argument_error(
            "parser stopped before the end of the input", in.cursor())};
    }
    const auto *selector{std::get_if<command::entity_selector>(&*value)};
    if (selector == nullptr) {
        return std::unexpected{
            command::make_argument_error("the parser returned another type")};
    }
    return std::move(*selector);
}

/// Run `input` through a dispatcher so the parse error carries the reference's
/// `<reason> at position <byte>` form.
[[nodiscard]] auto dispatch_error(std::string_view input) -> std::string {
    command::dispatcher dispatcher;
    dispatcher.literal("select").then(
        dispatcher.argument("target", command::entity_selector_arg()));
    selector_source source;
    auto outcome{dispatcher.execute(input, source)};
    return outcome ? std::string{} : outcome.error().message;
}

[[nodiscard]] auto has_suggestion(const command::suggestions &offered,
                                  std::string_view text) -> bool {
    for (const auto &entry : offered.entries) {
        if (entry.text == text) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto suggest(std::string_view remaining) -> command::suggestions {
    const command::entity_selector_parser parser{};
    selector_source source;
    command::suggestion_builder builder{0, remaining.size()};
    parser.suggest(remaining, source, builder);
    return std::move(builder).build();
}

[[nodiscard]] auto candidates() -> std::vector<command::selector_candidate> {
    return {
        command::selector_candidate{
            .name = "Alice",
            .identity = 1U,
            .player = true,
            .has_position = true,
            .x = 0.0,
            .y = 64.0,
            .z = 0.0,
            .mode = command::game_mode::creative,
        },
        command::selector_candidate{
            .name = "Bob",
            .identity = 2U,
            .player = true,
            .has_position = true,
            .x = 10.0,
            .y = 64.0,
            .z = 0.0,
            .mode = command::game_mode::survival,
        },
        command::selector_candidate{
            .name = "Carol",
            .identity = 3U,
            .player = true,
            .has_position = true,
            .x = 10.0,
            .y = 70.0,
            .z = 0.0,
            .mode = command::game_mode::survival,
        },
    };
}

struct resolution {
    std::vector<std::size_t> matches{};
    std::optional<command::selector_error> error{};
};

[[nodiscard]] auto resolve(std::string_view selector_text,
                           const std::vector<command::selector_candidate> &pool,
                           std::optional<command::selector_position> origin =
                               command::selector_position{
                                   .x = 0.0, .y = 64.0, .z = 0.0},
                           std::optional<std::uint64_t> self = std::nullopt,
                           std::uint64_t seed = 0U) -> resolution {
    auto selector{parse_selector(selector_text)};
    expect(selector.has_value()) << std::string{selector_text};
    if (!selector) {
        return resolution{.error = command::selector_error{}};
    }
    command::selector_request request{};
    request.candidates = pool;
    request.source_position = origin;
    request.self_identity = self;
    request.random_seed = seed;
    auto matches{command::resolve_selector(*selector, request)};
    if (!matches) {
        return resolution{.error = std::move(matches.error())};
    }
    return resolution{.matches = std::move(*matches)};
}

const suite selector_parser_tests = [] {
    "selector parser reads every family and the bare name form"_test = [] {
        const auto name{parse_selector("Alice")};
        expect(name.has_value());
        if (name) {
            expect(name->kind == command::selector_kind::name);
            expect(name->name == "Alice");
            expect(name->max_results == 1);
            expect(!name->includes_entities);
        }
        const auto nearest{parse_selector("@p")};
        expect(nearest.has_value());
        if (nearest) {
            expect(nearest->kind == command::selector_kind::nearest_player);
            expect(nearest->max_results == 1);
            expect(!nearest->includes_entities);
            expect(nearest->order == command::selector_order::nearest);
        }
        const auto all{parse_selector("@a")};
        expect(all.has_value());
        if (all) {
            expect(all->kind == command::selector_kind::all_players);
            expect(all->max_results ==
                   std::numeric_limits<std::int32_t>::max());
            expect(all->order == command::selector_order::arbitrary);
        }
        const auto random{parse_selector("@r")};
        expect(random.has_value());
        if (random) {
            expect(random->kind == command::selector_kind::random_player);
            expect(random->order == command::selector_order::random);
        }
        const auto self{parse_selector("@s")};
        expect(self.has_value());
        if (self) {
            expect(self->kind == command::selector_kind::self);
            expect(self->current_entity);
            expect(self->includes_entities);
        }
        const auto entities{parse_selector("@e")};
        expect(entities.has_value());
        if (entities) {
            expect(entities->kind == command::selector_kind::all_entities);
            expect(entities->includes_entities);
        }
        const auto nearest_entity{parse_selector("@n")};
        expect(nearest_entity.has_value());
        if (nearest_entity) {
            expect(nearest_entity->kind ==
                   command::selector_kind::nearest_entity);
            expect(nearest_entity->order == command::selector_order::nearest);
        }
    };

    "selector parser reads the modelled filters"_test = [] {
        const auto one{parse_selector(
            "@e[type=minecraft:player,limit=2,distance=1..10,x=1.5,y=64,"
            "z=-2,dx=1,dy=2,dz=3,sort=furthest,name=!Alice,gamemode="
            "survival]")};
        expect(one.has_value());
        if (!one) {
            return;
        }
        expect(one->includes_entities == false); // type=minecraft:player
        expect(one->max_results == 2);
        expect(one->limited);
        expect(one->order == command::selector_order::furthest);
        expect(one->distance.has_value());
        if (one->distance) {
            expect(one->distance->minimum == 1.0);
            expect(one->distance->maximum == 10.0);
        }
        expect(one->x == 1.5);
        expect(one->y == 64.0);
        expect(one->z == -2.0);
        expect(one->delta_x == 1.0);
        expect(one->delta_y == 2.0);
        expect(one->delta_z == 3.0);
        expect(one->names.size() == 1U);
        if (!one->names.empty()) {
            expect(one->names.front().name == "Alice");
            expect(one->names.front().inverted);
        }
        expect(one->gamemodes.size() == 1U);
        if (!one->gamemodes.empty()) {
            expect(one->gamemodes.front().mode == command::game_mode::survival);
            expect(!one->gamemodes.front().inverted);
        }
        expect(one->type.has_value());
        if (one->type) {
            expect(one->type->id == "minecraft:player");
            expect(!one->type->inverted);
        }
    };

    "selector parser accepts whitespace, an empty list and a trailing comma"_test =
        [] {
            expect(parse_selector("@p[]").has_value());
            expect(parse_selector("@p[ normalize ]").has_value() == false);
            expect(parse_selector("@a[ limit = 2 ]").has_value());
            expect(parse_selector("@a[limit=2,]").has_value());
            expect(parse_selector("@a[ name = \"Alice Bob\" ]").has_value());
            expect(parse_selector("@a[x=-1.5]").has_value());
            expect(parse_selector("@a[distance=..5]").has_value());
            expect(parse_selector("@a[distance=5..]").has_value());
            expect(parse_selector("@a[distance=5]").has_value());
            expect(parse_selector("@e[type=!minecraft:player]").has_value());
            expect(parse_selector("@a[gamemode=CREATIVE]").has_value());
        };

    "selector parser reports malformed input at its byte position"_test = [] {
        struct negative_case {
            std::string_view input;
            std::string_view reason;
            std::size_t offset{0};
        };
        const std::vector<negative_case> cases{
            {"", "expected a player name", 0},
            {"@", "expected an entity selector type", 1},
            {"@x", "unknown entity selector '@x'", 1},
            {"@p[", "expected the end of the selector options", 3},
            {"@p[limit", "expected a value for the selector option", 3},
            {"@p[foo=1]", "unknown entity selector option 'foo'", 3},
            {"@p[tag=x]", "not supported", 3},
            {"@p[scores={a=1}]", "not supported", 3},
            {"@p[nbt={a:1}]", "not supported", 3},
            {"@p[advancements={a=true}]", "not supported", 3},
            {"@p[predicate=a]", "not supported", 3},
            {"@p[team=a]", "not supported", 3},
            {"@p[level=1]", "not supported", 3},
            {"@p[x_rotation=1]", "not supported", 3},
            {"@p[limit=0]", "the selector limit must be at least 1", 9},
            {"@p[limit=]", "expected an integer", 9},
            {"@p[limit=1,limit=2]", "isn't applicable here", 11},
            {"@p[x=1,x=2]", "isn't applicable here", 7},
            {"@p[distance=-1]", "the distance cannot be negative", 12},
            {"@p[distance=5..1]", "larger than its maximum", 12},
            {"@p[distance=]", "expected a selector range", 12},
            {"@p[sort=sideways]", "unknown sort mode 'sideways'", 8},
            {"@p[sort=nearest,sort=furthest]", "isn't applicable here", 16},
            {"@p[gamemode=builder]", "unknown game mode 'builder'", 12},
            {"@p[gamemode=creative,gamemode=survival]", "isn't applicable here",
             21},
            {"@p[name=A,name=B]", "isn't applicable here", 10},
            {"@p[type=#minecraft:player]", "not an entity type tag", 8},
            {"@p[type=]", "invalid entity type ''", 8},
            {"@s[limit=1]", "isn't applicable here", 3},
            {"@s[sort=nearest]", "isn't applicable here", 3},
            {"ThisNameIsWayTooLong", "expected a player name", 0},
            {"dd12be42-52a9-4a91-a8a1-11c01849e498", "uuid", 0},
        };
        for (const auto &[input, reason, offset] : cases) {
            const auto parsed{parse_selector(input)};
            expect(!parsed.has_value()) << std::string{input};
            if (parsed) {
                continue;
            }
            expect(parsed.error().message.find(reason) != std::string::npos)
                << std::string{input} << ": " << parsed.error().message;
            expect(parsed.error().offset == offset)
                << std::string{input} << ": " << parsed.error().offset;
        }
    };

    "selector parser enforces the argument-form cardinality rules"_test = [] {
        const auto single_entity{parse_selector("@a", true, false)};
        expect(!single_entity.has_value());
        if (!single_entity) {
            expect(single_entity.error().message.find(
                       "Only one entity is allowed") != std::string::npos)
                << single_entity.error().message;
            expect(single_entity.error().offset == 0U);
        }
        const auto single_player{parse_selector("@e", true, true)};
        expect(!single_player.has_value());
        if (!single_player) {
            expect(single_player.error().message.find(
                       "Only one player is allowed") != std::string::npos)
                << single_player.error().message;
        }
        // A limit of one makes a multi-result selector acceptable.
        expect(parse_selector("@a[limit=1]", true, false).has_value());
        expect(parse_selector("@p", true, false).has_value());

        const auto entities_for_players{parse_selector("@e", false, true)};
        expect(!entities_for_players.has_value());
        if (!entities_for_players) {
            expect(entities_for_players.error().message.find(
                       "Only players may be affected") != std::string::npos)
                << entities_for_players.error().message;
        }
        // The reference exempts `@s` and a selector narrowed to players.
        expect(parse_selector("@s", false, true).has_value());
        expect(parse_selector("@e[type=minecraft:player]", false, true)
                   .has_value());
        expect(parse_selector("Alice", false, true).has_value());
    };

    "selector parser reports its argument kind and form to the tree"_test = [] {
        const command::entity_selector_parser parser{true, true};
        const auto descriptor{parser.descriptor()};
        expect(descriptor.kind == command::parser_kind::entity_selector);
        expect(descriptor.entity_single);
        expect(descriptor.entity_players_only);
        expect(command::to_string(command::parser_kind::entity_selector) ==
               "entity_selector");
    };

    "selector parser suggests families, filters and filter values"_test = [] {
        const auto empty{suggest("")};
        expect(has_suggestion(empty, "@p"));
        expect(has_suggestion(empty, "@a"));
        expect(has_suggestion(empty, "@r"));
        expect(has_suggestion(empty, "@s"));
        expect(has_suggestion(empty, "@e"));
        expect(has_suggestion(empty, "@n"));
        expect(has_suggestion(empty, "Alice"));
        expect(has_suggestion(empty, "Bob"));

        const auto at{suggest("@")};
        expect(has_suggestion(at, "@p"));
        expect(!has_suggestion(at, "Alice"));

        const auto family{suggest("@e")};
        expect(has_suggestion(family, "@e"));
        expect(has_suggestion(family, "@e["));

        const auto keys{suggest("@e[")};
        for (const auto *key :
             {"name=", "distance=", "x=", "y=", "z=", "dx=", "dy=", "dz=",
              "limit=", "sort=", "gamemode=", "type="}) {
            expect(has_suggestion(keys, std::string{"@e["} + key)) << key;
        }

        const auto partial_key{suggest("@e[lim")};
        expect(has_suggestion(partial_key, "@e[limit="));

        const auto sort_values{suggest("@e[sort=n")};
        expect(has_suggestion(sort_values, "@e[sort=nearest"));

        const auto modes{suggest("@e[gamemode=cre")};
        expect(has_suggestion(modes, "@e[gamemode=creative"));

        const auto types{suggest("@e[type=mine")};
        expect(has_suggestion(types, "@e[type=minecraft:player"));

        const auto names{suggest("@e[name=Al")};
        expect(has_suggestion(names, "@e[name=Alice"));
    };
};

const suite selector_resolution_tests = [] {
    "resolve_selector picks the nearest player and honours the origin override"_test =
        [] {
            const auto pool{candidates()};
            const auto nearest{resolve("@p", pool)};
            expect(nearest.matches == std::vector<std::size_t>{0U});
            const auto overridden{resolve("@p[x=10,y=64,z=0]", pool)};
            expect(overridden.matches == std::vector<std::size_t>{1U});
            const auto nearest_entity{resolve("@n", pool)};
            expect(nearest_entity.matches == std::vector<std::size_t>{0U});
        };

    "resolve_selector lists, orders and limits the candidates"_test = [] {
        const auto pool{candidates()};
        expect(resolve("@a", pool).matches ==
               std::vector<std::size_t>({0U, 1U, 2U}));
        expect(resolve("@e", pool).matches ==
               std::vector<std::size_t>({0U, 1U, 2U}));
        expect(resolve("@a[sort=nearest]", pool).matches ==
               std::vector<std::size_t>({0U, 1U, 2U}));
        expect(resolve("@a[sort=furthest]", pool).matches ==
               std::vector<std::size_t>({2U, 1U, 0U}));
        expect(resolve("@a[sort=furthest,limit=1]", pool).matches ==
               std::vector<std::size_t>{2U});
        expect(resolve("@a[limit=2]", pool).matches ==
               std::vector<std::size_t>({0U, 1U}));
        // Nearest ties keep the candidate order, which is a stable sort of the
        // ascending-entity-id enumeration.
        auto tied{candidates()};
        tied.push_back(command::selector_candidate{
            .name = "Dave",
            .identity = 4U,
            .player = true,
            .has_position = true,
            .x = 0.0,
            .y = 64.0,
            .z = 10.0,
            .mode = command::game_mode::survival,
        });
        const auto ordered{resolve("@a[sort=nearest]", tied)};
        expect(ordered.matches == std::vector<std::size_t>({0U, 1U, 3U, 2U}));
    };

    "resolve_selector applies the distance and box filters"_test = [] {
        const auto pool{candidates()};
        expect(resolve("@a[distance=..10]", pool).matches ==
               std::vector<std::size_t>({0U, 1U}));
        expect(resolve("@a[distance=1..]", pool).matches ==
               std::vector<std::size_t>({1U, 2U}));
        expect(resolve("@a[distance=10..10]", pool).matches ==
               std::vector<std::size_t>{1U});
        // The source origin is overridden per axis.
        expect(resolve("@a[distance=..1,x=10,y=64,z=0]", pool).matches ==
               std::vector<std::size_t>{1U});
        // `dx`/`dz` form a one-block box widened by the reference's rule.
        expect(resolve("@a[dx=1,dz=1]", pool).matches ==
               std::vector<std::size_t>{0U});
        expect(resolve("@a[dx=-2,dz=1]", pool).matches ==
               std::vector<std::size_t>{0U});
        // The box can be anchored without any source position.
        const auto anchored{
            resolve("@a[x=10,y=64,z=0,dx=1,dz=1]", pool, std::nullopt)};
        expect(anchored.matches == std::vector<std::size_t>{1U});
    };

    "resolve_selector applies the name and game mode filters"_test = [] {
        const auto pool{candidates()};
        expect(resolve("@a[name=Bob]", pool).matches ==
               std::vector<std::size_t>{1U});
        expect(resolve("@a[name=!Bob]", pool).matches ==
               std::vector<std::size_t>({0U, 2U}));
        expect(resolve("@p[gamemode=survival]", pool).matches ==
               std::vector<std::size_t>{1U});
        expect(resolve("@a[gamemode=creative]", pool).matches ==
               std::vector<std::size_t>{0U});
        expect(resolve("@a[gamemode=!survival]", pool).matches ==
               std::vector<std::size_t>{0U});
        expect(resolve("@a[name=Bob,gamemode=creative]", pool).matches.empty());
    };

    "resolve_selector evaluates the entity type filter and refuses unmodelled "
    "types"_test = [] {
        const auto pool{candidates()};
        expect(resolve("@e[type=minecraft:player]", pool).matches ==
               std::vector<std::size_t>({0U, 1U, 2U}));
        expect(resolve("@e[type=!minecraft:player]", pool).matches.empty());
        const auto unsupported{resolve("@e[type=minecraft:zombie]", pool)};
        expect(unsupported.error.has_value());
        if (unsupported.error) {
            expect(unsupported.error->failure ==
                   command::selector_error::kind::unsupported_entity_type);
            expect(unsupported.error->message.find("minecraft:zombie") !=
                   std::string::npos)
                << unsupported.error->message;
        }
    };

    "resolve_selector applies @s to the source entity only"_test = [] {
        const auto pool{candidates()};
        expect(
            resolve("@s", pool,
                    command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                    2U)
                .matches == std::vector<std::size_t>{1U});
        expect(resolve("@s", pool).matches.empty());
        expect(
            resolve("@s", pool,
                    command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                    99U)
                .matches.empty());
        expect(
            resolve("@s[name=Bob]", pool,
                    command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                    1U)
                .matches.empty());
        expect(
            resolve("@s[distance=..1]", pool,
                    command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                    2U)
                .matches.empty());
    };

    "resolve_selector matches a bare name without regard to case"_test = [] {
        const auto pool{candidates()};
        expect(resolve("Alice", pool).matches == std::vector<std::size_t>{0U});
        expect(resolve("alice", pool).matches == std::vector<std::size_t>{0U});
        expect(resolve("Nobody", pool).matches.empty());
        expect(resolve("@a", {}).matches.empty());
    };

    "resolve_selector fails closed when it cannot evaluate a position"_test =
        [] {
            const auto pool{candidates()};
            const auto no_origin{resolve("@p", pool, std::nullopt)};
            expect(no_origin.error.has_value());
            if (no_origin.error) {
                expect(no_origin.error->failure ==
                       command::selector_error::kind::no_source_position);
                expect(no_origin.error->message.find("no position") !=
                       std::string::npos)
                    << no_origin.error->message;
            }
            // A selector that needs no origin still resolves.
            expect(resolve("@a", pool, std::nullopt).matches ==
                   std::vector<std::size_t>({0U, 1U, 2U}));
            expect(resolve("@e[limit=2]", pool, std::nullopt).matches ==
                   std::vector<std::size_t>({0U, 1U}));
            expect(
                resolve("@a[sort=random]", pool, std::nullopt).matches.size() ==
                3U);

            auto positioned{candidates()};
            positioned.push_back(command::selector_candidate{
                .name = "Ghost",
                .identity = 9U,
                .player = true,
                .has_position = false,
                .mode = command::game_mode::survival,
            });
            const auto missing{resolve("@a[distance=..5]", positioned)};
            expect(missing.error.has_value());
            if (missing.error) {
                expect(
                    missing.error->failure ==
                    command::selector_error::kind::missing_candidate_position);
                expect(missing.error->message.find("Ghost") !=
                       std::string::npos)
                    << missing.error->message;
            }
            const auto sort_missing{resolve("@a[sort=nearest]", positioned)};
            expect(sort_missing.error.has_value());
        };

    "resolve_selector draws @r and sort=random from the request seed"_test =
        [] {
            const auto pool{candidates()};
            const auto first{resolve(
                "@r", pool,
                command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                std::nullopt, 42U)};
            const auto again{resolve(
                "@r", pool,
                command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                std::nullopt, 42U)};
            expect(first.matches.size() == 1U);
            expect(first.matches == again.matches);
            // Different seeds reach more than one candidate: the draw is a real
            // choice and not a constant.
            std::vector<std::size_t> seen{};
            for (std::uint64_t seed{0}; seed < 32U; ++seed) {
                const auto draw{resolve(
                    "@r", pool,
                    command::selector_position{.x = 0.0, .y = 64.0, .z = 0.0},
                    std::nullopt, seed)};
                if (draw.matches.size() != 1U) {
                    continue;
                }
                bool known{false};
                for (const auto value : seen) {
                    known = known || value == draw.matches.front();
                }
                if (!known) {
                    seen.push_back(draw.matches.front());
                }
            }
            expect(seen.size() > 1U) << seen.size();
            const auto limited{resolve("@r[limit=2]", pool)};
            expect(limited.matches.size() == 2U);
            const auto shuffled{resolve("@a[sort=random]", pool)};
            expect(shuffled.matches.size() == 3U);
        };

    "dispatcher reports a selector parse error with the command byte position"_test =
        [] {
            const auto message{dispatch_error("select @p[foo=1]")};
            expect(message.find("unknown entity selector option 'foo'") !=
                   std::string::npos)
                << message;
            expect(message.find("at position 10") != std::string::npos)
                << message;
        };
};

} // namespace
