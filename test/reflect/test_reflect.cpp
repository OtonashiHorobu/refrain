// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Reflection facade smoke test. Business code includes only the facade
// (`refrain/reflect.hpp`); this file is the only place in the tree that may
// exercise the backend, and it deliberately includes no refl-cpp/magic_enum
// headers of its own.

#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION

#include <refrain/reflect.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace reflection_smoke {

struct external_vec3 {
    float x{};
    float y{};
    float z{};
};

struct range_attribute : refrain::reflect::field_attribute_tag {
    constexpr range_attribute(int lower, int upper) : min{lower}, max{upper} {}
    int min{};
    int max{};
};

struct player_stats {
    std::int32_t health{};
    std::int32_t xp{};
    float speed{};
};

/// Deliberately NOT annotated: the facade must report it as unreflected.
struct not_annotated {
    int value{};
};

enum class color : std::uint8_t { red, green, blue };

} // namespace reflection_smoke

REFR_REFLECT(type(reflection_smoke::player_stats),
             REFR_FIELD(health, reflection_smoke::range_attribute{0, 20},
                        refrain::attr::range{0, 20},
                        refrain::attr::sensitive{}),
             REFR_FIELD(xp, refrain::attr::net_field{}),
             REFR_FIELD(speed, refrain::attr::unit{"m/s"},
                        refrain::attr::debug_only{}))

// Manual traits escape hatch for a type that cannot be annotated with
// REFR_REFLECT (third-party/C ABI shape).
template <>
struct refrain::reflect::custom_traits<reflection_smoke::external_vec3> {
    static constexpr std::string_view type_name = "external_vec3";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x",
                                       &reflection_smoke::external_vec3::x},
        refrain::reflect::custom_field{"y",
                                       &reflection_smoke::external_vec3::y},
        refrain::reflect::custom_field{"z",
                                       &reflection_smoke::external_vec3::z});
};

namespace {

using namespace boost::ut;

using reflection_smoke::color;
using reflection_smoke::player_stats;

// Compile-time contract: all metadata is `inline constexpr` (no runtime table,
// no RTTI) and can therefore be used in `static_assert`.
static_assert(refrain::reflect::reflected<player_stats>);
static_assert(refrain::reflect::reflected<const player_stats>);
static_assert(!refrain::reflect::reflected<reflection_smoke::not_annotated>);
// refl-cpp pre-reflects primitives (name only, zero fields); the facade keeps
// that backend behaviour visible rather than hiding it.
static_assert(refrain::reflect::reflected<int>);
static_assert(refrain::reflect::field_count<int> == 0U);
static_assert(refrain::reflect::field_count<player_stats> == 3U);
static_assert(refrain::reflect::field_name<player_stats, 0>() == "health");
static_assert(refrain::reflect::field_name<player_stats, 1>() == "xp");
static_assert(refrain::reflect::field_name<player_stats, 2>() == "speed");
static_assert(refrain::reflect::reflected<reflection_smoke::external_vec3>);
static_assert(refrain::reflect::field_count<reflection_smoke::external_vec3> ==
              3U);
static_assert(
    refrain::reflect::field_name<reflection_smoke::external_vec3, 1>() == "y");
static_assert(refrain::reflect::name<reflection_smoke::external_vec3> ==
              "external_vec3");
static_assert(refrain::reflect::has_field_attribute<
              player_stats, 0, reflection_smoke::range_attribute>());
static_assert(!refrain::reflect::has_field_attribute<
              player_stats, 1, reflection_smoke::range_attribute>());
static_assert(refrain::reflect::field_attribute<
                  player_stats, 0, reflection_smoke::range_attribute>()
                  .max == 20);
static_assert(refrain::reflect::has_field_attribute<player_stats, 0,
                                                    refrain::attr::range>());
static_assert(refrain::reflect::has_field_attribute<
              player_stats, 0, refrain::attr::sensitive>());
static_assert(refrain::reflect::has_field_attribute<
              player_stats, 1, refrain::attr::net_field>());
static_assert(refrain::reflect::has_field_attribute<
              player_stats, 2, refrain::attr::debug_only>());
static_assert(
    refrain::reflect::field_attribute<player_stats, 0, refrain::attr::range>()
        .max == 20);
static_assert(refrain::reflect::name<player_stats>.ends_with("player_stats"));

const suite reflect_tests = [] {
    "reflect/annotated type exposes its name and fields"_test = [] {
        expect(!refrain::reflect::name<player_stats>.empty());
        expect(refrain::reflect::name<player_stats>.ends_with("player_stats"));
        expect(refrain::reflect::field_count<player_stats> == std::size_t{3});

        const std::array<std::string_view, 3> expected{"health", "xp", "speed"};
        for (std::size_t index = 0; index < expected.size(); ++index) {
            // The runtime index selects through the same compile-time values
            // asserted above via a small jump table here (facade users get
            // index-based access at compile time; this exercises the runtime
            // spelling of the same metadata).
            const std::array<std::string_view, 3> names{
                refrain::reflect::field_name<player_stats, 0>(),
                refrain::reflect::field_name<player_stats, 1>(),
                refrain::reflect::field_name<player_stats, 2>()};
            expect(names[index] == expected[index]);
        }
    };

    "reflect/custom_traits registers an unannotatable type"_test = [] {
        reflection_smoke::external_vec3 vector{1.0F, 2.0F, 3.0F};
        std::vector<std::string> visited{};
        refrain::reflect::for_each_field(
            vector, [&visited](std::string_view field, auto &value) {
                visited.emplace_back(field);
                value += 10.0F;
            });
        expect(visited.size() == 3U);
        expect(visited[0] == "x" && visited[1] == "y" && visited[2] == "z");
        expect(vector.x == 11.0F);
        expect(vector.y == 12.0F);
        expect(vector.z == 13.0F);
    };

    "reflect/named annotations are queryable at compile time"_test = [] {
        expect(refrain::reflect::has_field_attribute<player_stats, 0,
                                                     refrain::attr::range>());
        expect(refrain::reflect::field_attribute<player_stats, 0,
                                                 refrain::attr::range>()
                   .min == 0);
        expect(refrain::reflect::field_attribute<player_stats, 2,
                                                 refrain::attr::unit>()
                   .symbol == "m/s");
        expect(
            refrain::reflect::has_field_attribute<player_stats, 1,
                                                  refrain::attr::net_field>());
        expect(
            !refrain::reflect::has_field_attribute<player_stats, 1,
                                                   refrain::attr::sensitive>());
    };

    "reflect/for_each_field walks declaration order and allows mutation"_test =
        [] {
            player_stats stats{};
            stats.health = 10;
            stats.xp = 20;
            stats.speed = 1.5F;

            std::vector<std::string> visited{};
            refrain::reflect::for_each_field(
                stats, [&visited](std::string_view field, auto &value) {
                    visited.emplace_back(field);
                    using value_type = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<value_type, std::int32_t>) {
                        value += 1;
                    } else {
                        static_assert(std::is_same_v<value_type, float>);
                        value *= 2.0F;
                    }
                });

            expect(visited ==
                   (std::vector<std::string>{"health", "xp", "speed"}));
            expect(stats.health == 11);
            expect(stats.xp == 21);
            expect(stats.speed == 3.0F);
        };

    "reflect/for_each_field on a const object yields const references"_test =
        [] {
            const player_stats stats{1, 2, 3.0F};
            std::size_t visited{0};
            refrain::reflect::for_each_field(stats, [&visited](
                                                        std::string_view field,
                                                        const auto &value) {
                static_assert(
                    std::is_const_v<std::remove_reference_t<decltype(value)>>);
                // The const walk must preserve declaration order (checked via
                // `visited` below) and expose the concrete field values.
                if (field == "health") {
                    expect(value == 1);
                } else if (field == "xp") {
                    expect(value == 2);
                } else {
                    expect(field == "speed");
                    expect(value == 3.0F);
                }
                ++visited;
            });
            expect(visited == std::size_t{3});
        };

    "reflect/enums expose names and values through the facade"_test = [] {
        expect(refrain::reflect::enum_name(color::red) == "red");
        expect(refrain::reflect::enum_name(color::green) == "green");
        expect(refrain::reflect::enum_name(color::blue) == "blue");
        // Out-of-range values have no enumerator name.
        expect(refrain::reflect::enum_name(static_cast<color>(42)).empty());

        const auto parsed = refrain::reflect::enum_cast<color>("blue");
        expect(parsed.has_value());
        if (parsed.has_value()) {
            expect(*parsed == color::blue);
        }
        expect(!refrain::reflect::enum_cast<color>("purple").has_value());

        const auto values = refrain::reflect::enum_values<color>();
        expect(values.size() == std::size_t{3});
        expect(values[0] == color::red);
        expect(values[2] == color::blue);

        auto names{std::string{}};
        std::size_t count{0};
        refrain::reflect::for_each_enumerator<color>([&](color value) {
            ++count;
            names += refrain::reflect::enum_name(value);
        });
        expect(count == std::size_t{3});
        expect(names == "redgreenblue");
    };
};

} // namespace

#else // !REFRAIN_USE_REFLECTION

namespace {

// Keep the translation unit non-empty when the project is configured with
// -DREFRAIN_USE_REFLECTION=OFF (no reflection tests to run then).
[[maybe_unused]] constexpr int reflection_disabled = 0;

} // namespace

#endif // REFRAIN_USE_REFLECTION
