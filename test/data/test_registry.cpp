// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/resource_location.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::data::blake3;
using refrain::data::registry;
using refrain::data::registry_id;
using refrain::data::resource_location;
using refrain::data::to_hex;

/// Parse a fixture location; a malformed fixture fails the current test.
[[nodiscard]] auto make_location(std::string_view text) -> resource_location {
    auto parsed{resource_location::parse(text)};
    if (!parsed) {
        expect(false) << "malformed test fixture location: " << text;
        return {};
    }
    return *parsed;
}

/// Replicates canonical fingerprint encoding v1 as documented on
/// `registry::fingerprint()` (independent implementation to lock the contract).
[[nodiscard]] auto
expected_fingerprint(std::string_view type_name,
                     std::span<const std::string_view> sorted_texts)
    -> std::string {
    const auto append_u64 = [](std::string &out, std::uint64_t value) {
        for (std::size_t byte{0}; byte < 8U; ++byte) {
            out.push_back(static_cast<char>(value & 0xFFU));
            value >>= 8U;
        }
    };
    std::string encoded;
    encoded.append("refrain.registry.fingerprint.v1");
    encoded.push_back('\0');
    append_u64(encoded, static_cast<std::uint64_t>(type_name.size()));
    encoded.append(type_name);
    append_u64(encoded, static_cast<std::uint64_t>(sorted_texts.size()));
    for (const auto text : sorted_texts) {
        append_u64(encoded, static_cast<std::uint64_t>(text.size()));
        encoded.append(text);
    }
    const auto bytes{
        std::as_bytes(std::span<const char>{encoded.data(), encoded.size()})};
    return to_hex(blake3(bytes));
}

const suite registry_tests = [] {
    "registry/ids are assigned in registration order and stay stable"_test =
        [] {
            registry<int> values{std::string{"test.ids"}};
            expect(values.empty());
            expect(values.size() == 0U);
            expect(values.type_name() == "test.ids");
            expect(!values.is_frozen());

            const auto first{values.add(make_location("minecraft:first"), 10)};
            const auto second{
                values.add(make_location("minecraft:second"), 20)};
            const auto third{values.add(make_location("test:third"), 30)};
            expect(first.has_value());
            expect(second.has_value());
            expect(third.has_value());
            if (!first || !second || !third) {
                return;
            }
            expect(first->value() == 0U);
            expect(second->value() == 1U);
            expect(third->value() == 2U);
            expect(first->valid());

            const auto ids{values.ids()};
            expect(ids.size() == 3U);
            if (ids.size() == 3U) {
                expect(ids[0] == *first);
                expect(ids[1] == *second);
                expect(ids[2] == *third);
            }
            expect(values.size() == 3U);
            expect(!values.empty());

            // Ids survive freeze/adds of the same snapshot.
            expect(values.freeze().has_value());
            expect(values.ids().size() == 3U);
            expect(values.at(*first) != nullptr);
            if (const auto *value{values.at(*first)}) {
                expect(*value == 10);
            }
        };

    "registry/add rejects duplicate locations and keeps the first value"_test =
        [] {
            registry<int> values{std::string{"test.duplicates"}};
            const auto location{make_location("minecraft:stone")};
            expect(values.add(location, 1).has_value());
            const auto duplicate{values.add(location, 2)};
            expect(!duplicate.has_value());
            if (!duplicate) {
                expect(duplicate.error().code ==
                       refrain::error_code::already_exists);
                expect(duplicate.error().message.find("minecraft:stone") !=
                       std::string::npos);
            }
            expect(values.size() == 1U);
            const auto *value{values.find(location)};
            expect(value != nullptr);
            if (value != nullptr) {
                expect(*value == 1);
            }
            // The registry is still usable after a rejected add.
            expect(values.add(make_location("minecraft:dirt"), 3).has_value());
            expect(values.size() == 2U);
        };

    "registry/add rejects empty resource locations"_test = [] {
        registry<int> values{std::string{"test.empty_location"}};
        const auto rejected{values.add(resource_location{}, 1)};
        expect(!rejected.has_value());
        if (!rejected) {
            expect(rejected.error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(values.empty());
        expect(values.add(make_location("minecraft:a"), 1).has_value());
        expect(values.size() == 1U);
    };

    "registry/find and contains only hit registered locations"_test = [] {
        registry<int> values{std::string{"test.find"}};
        const auto stone{make_location("minecraft:stone")};
        const auto grass{make_location("minecraft:grass_block")};
        expect(values.add(stone, 7).has_value());
        expect(values.add(grass, 9).has_value());

        expect(values.contains(stone));
        expect(values.contains(grass));
        expect(!values.contains(make_location("minecraft:diamond")));
        expect(values.find(make_location("minecraft:diamond")) == nullptr);

        const auto *stone_value{values.find(stone)};
        expect(stone_value != nullptr);
        if (stone_value != nullptr) {
            expect(*stone_value == 7);
        }
        // Implicit `minecraft` namespace and explicit form are the same key.
        expect(values.contains(make_location("stone")));
        expect(values.find(make_location("minecraft:stone")) == stone_value);
    };

    "registry/at handles valid ids and every invalid id form"_test = [] {
        registry<int> values{std::string{"test.at"}};
        expect(values.add(make_location("minecraft:a"), 100).has_value());
        expect(values.add(make_location("minecraft:b"), 200).has_value());

        const auto *first{values.at(registry_id{0U})};
        const auto *second{values.at(registry_id{1U})};
        expect(first != nullptr);
        expect(second != nullptr);
        if (first != nullptr && second != nullptr) {
            expect(*first == 100);
            expect(*second == 200);
        }
        expect(values.at(registry_id{2U}) == nullptr);
        expect(values.at(registry_id{99U}) == nullptr);
        expect(values.at(registry_id::invalid()) == nullptr);
        expect(values.at(registry_id{registry_id::invalid_value}) == nullptr);
        expect(values.at(registry_id{}) == nullptr);
    };

    "registry/freeze seals mutations and is idempotent"_test = [] {
        registry<int> values{std::string{"test.freeze"}};
        const auto location{make_location("minecraft:locked")};
        expect(values.add(location, 5).has_value());
        expect(values.freeze().has_value());
        expect(values.is_frozen());
        expect(values.freeze().has_value()); // idempotent

        const auto rejected{values.add(make_location("minecraft:late"), 6)};
        expect(!rejected.has_value());
        if (!rejected) {
            expect(rejected.error().code ==
                   refrain::error_code::permission_denied);
        }
        const auto cleared{values.clear()};
        expect(!cleared.has_value());
        if (!cleared) {
            expect(cleared.error().code ==
                   refrain::error_code::permission_denied);
        }
        expect(values.size() == 1U);
        expect(values.contains(location));
        if (const auto *value{values.find(location)}) {
            expect(*value == 5);
        }
    };

    "registry/clear resets entries before freeze but keeps the type name"_test =
        [] {
            registry<int> values{std::string{"test.clear"}};
            expect(values.add(make_location("minecraft:a"), 1).has_value());
            expect(values.add(make_location("minecraft:b"), 2).has_value());
            expect(values.clear().has_value());
            expect(values.empty());
            expect(values.size() == 0U);
            expect(values.ids().empty());
            expect(values.entries().empty());
            expect(values.type_name() == "test.clear");
            expect(!values.is_frozen());
            expect(values.find(make_location("minecraft:a")) == nullptr);
            expect(values.at(registry_id{0U}) == nullptr);
            expect(values.dump().empty());
            // After a reload the registry re-uses ids from 0 in new order.
            const auto reloaded{values.add(make_location("minecraft:b"), 3)};
            expect(reloaded.has_value());
            if (reloaded) {
                expect(reloaded->value() == 0U);
            }
            expect(values.size() == 1U);
        };

    "registry/copies share the mutable snapshot and the frozen state"_test =
        [] {
            registry<int> original{std::string{"test.copies"}};
            auto copy{original};
            expect(copy.add(make_location("minecraft:a"), 11) ==
                   registry_id{0U});
            expect(original.size() == 1U);
            expect(copy.size() == 1U);
            if (const auto *value{
                    original.find(make_location("minecraft:a"))}) {
                expect(*value == 11);
            }

            // Freezing one copy freezes the shared snapshot for the others.
            expect(copy.freeze().has_value());
            expect(original.is_frozen());
            const auto rejected{original.add(make_location("minecraft:b"), 12)};
            expect(!rejected.has_value());
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::permission_denied);
            }

            auto frozen_copy{original};
            expect(frozen_copy.size() == 1U);
            expect(frozen_copy.is_frozen());
            expect(
                frozen_copy.add(make_location("minecraft:c"), 13).has_value() ==
                false);
            expect(original.size() == 1U);
        };

    "registry/fingerprint is independent of registration order"_test = [] {
        registry<int> forwards{std::string{"test.order"}};
        expect(forwards.add(make_location("minecraft:a"), 1).has_value());
        expect(forwards.add(make_location("minecraft:b"), 2).has_value());
        expect(forwards.add(make_location("test:c"), 3).has_value());

        registry<int> backwards{std::string{"test.order"}};
        expect(backwards.add(make_location("test:c"), 30).has_value());
        expect(backwards.add(make_location("minecraft:b"), 20).has_value());
        expect(backwards.add(make_location("minecraft:a"), 10).has_value());

        expect(forwards.ids()[0].value() == 0U);
        expect(backwards.ids()[0].value() == 0U);
        expect(to_hex(forwards.fingerprint()) ==
               to_hex(backwards.fingerprint()));
        expect(forwards.dump() != backwards.dump()) << "dump keeps id order";
    };

    "registry/fingerprint ignores registered values"_test = [] {
        registry<int> first{std::string{"test.values"}};
        registry<int> second{std::string{"test.values"}};
        expect(first.add(make_location("minecraft:a"), 1).has_value());
        expect(second.add(make_location("minecraft:a"), 999).has_value());
        expect(to_hex(first.fingerprint()) == to_hex(second.fingerprint()));

        // Values are not part of the identity, but the type name is caller
        // chosen: a differently typed registry with the same name/locations
        // fingerprints identically (documented caller responsibility).
        registry<std::string> strings{std::string{"test.values"}};
        expect(strings.add(make_location("minecraft:a"), std::string{"x"})
                   .has_value());
        expect(to_hex(strings.fingerprint()) == to_hex(first.fingerprint()));
    };

    "registry/fingerprint distinguishes locations, type names and emptiness"_test =
        [] {
            registry<int> single{std::string{"test.distinct"}};
            expect(single.add(make_location("minecraft:a"), 1).has_value());
            registry<int> pair{std::string{"test.distinct"}};
            expect(pair.add(make_location("minecraft:a"), 1).has_value());
            expect(pair.add(make_location("minecraft:b"), 2).has_value());
            expect(to_hex(single.fingerprint()) != to_hex(pair.fingerprint()));

            registry<int> other_name{std::string{"test.distinct.other"}};
            expect(other_name.add(make_location("minecraft:a"), 1).has_value());
            expect(to_hex(single.fingerprint()) !=
                   to_hex(other_name.fingerprint()));

            registry<int> empty_a{std::string{"test.distinct"}};
            registry<int> empty_b{std::string{"test.distinct"}};
            expect(empty_a.empty());
            expect(to_hex(empty_a.fingerprint()) ==
                   to_hex(empty_b.fingerprint()));
            expect(to_hex(empty_a.fingerprint()) !=
                   to_hex(single.fingerprint()));

            registry<int> unnamed_a{};
            registry<int> unnamed_b{};
            expect(to_hex(unnamed_a.fingerprint()) ==
                   to_hex(unnamed_b.fingerprint()));
            expect(to_hex(unnamed_a.fingerprint()) !=
                   to_hex(empty_a.fingerprint()));
        };

    "registry/fingerprint matches the documented canonical v1 encoding"_test =
        [] {
            registry<int> values{std::string{"test.encoding"}};
            expect(values.add(make_location("minecraft:b"), 1).has_value());
            expect(values.add(make_location("minecraft:a"), 2).has_value());

            const std::array<std::string_view, 2> sorted{"minecraft:a",
                                                         "minecraft:b"};
            expect(to_hex(values.fingerprint()) ==
                   expected_fingerprint("test.encoding", sorted));

            // Registration order never leaked into the encoding: a reversed
            // build produces the same digest.
            registry<int> reversed{std::string{"test.encoding"}};
            expect(reversed.add(make_location("minecraft:a"), 1).has_value());
            expect(reversed.add(make_location("minecraft:b"), 2).has_value());
            expect(to_hex(reversed.fingerprint()) ==
                   expected_fingerprint("test.encoding", sorted));
        };

    "registry/dump lists id, location and newline per entry in id order"_test =
        [] {
            registry<int> values{std::string{"test.dump"}};
            expect(values.dump().empty());
            expect(values.add(make_location("minecraft:a"), 1).has_value());
            expect(values.add(make_location("test:b/c"), 2).has_value());
            expect(values.dump() == "0\tminecraft:a\n1\ttest:b/c\n");

            std::string all_lines{values.dump()};
            expect(static_cast<std::size_t>(
                       std::count(all_lines.begin(), all_lines.end(), '\n')) ==
                   values.size());
        };

    "registry/dump_with appends a caller-provided payload column"_test = [] {
        registry<int> values{std::string{"test.dump_with"}};
        expect(values.add(make_location("minecraft:a"), 4).has_value());
        expect(values.add(make_location("minecraft:b"), 7).has_value());
        expect(values.dump_with([](int value) {
            return std::to_string(value * 2);
        }) == "0\tminecraft:a\t8\n1\tminecraft:b\t14\n");
        expect(values.dump_with([](const int &) -> std::string_view {
            return "constant";
        }) == "0\tminecraft:a\tconstant\n1\tminecraft:b\tconstant\n");
        // A formatter may also capture state; output stays deterministic.
        std::string prefix{"payload:"};
        expect(values.dump_with([&prefix](int value) {
            return prefix + std::to_string(value);
        }) == "0\tminecraft:a\tpayload:4\n"
              "1\tminecraft:b\tpayload:7\n");
    };

    "registry/empty registry behaves like an empty snapshot"_test = [] {
        registry<int> values{std::string{"test.empty"}};
        expect(values.empty());
        expect(values.size() == 0U);
        expect(values.ids().empty());
        expect(values.entries().empty());
        expect(values.dump().empty());
        expect(!values.is_frozen());
        expect(!values.contains(make_location("minecraft:a")));
        expect(values.find(make_location("minecraft:a")) == nullptr);
        expect(values.at(registry_id{0U}) == nullptr);
        expect(values.freeze().has_value());
        expect(values.is_frozen());
        expect(values.fingerprint() == values.fingerprint());
    };

    "registry/handles a 10k entry registry deterministically"_test = [] {
        constexpr std::size_t count{10'000U};
        registry<int> values{std::string{"test.large"}};
        for (std::size_t index{0}; index < count; ++index) {
            const auto added{values.add(
                make_location("minecraft:item/" + std::to_string(index)),
                static_cast<int>(index))};
            if (!added) {
                expect(false) << "add failed at index " << index;
                return;
            }
            if (added->value() != index) {
                expect(false) << "unexpected id at index " << index;
                return;
            }
        }
        expect(values.size() == count);
        expect(values.ids().size() == count);
        expect(values.freeze().has_value());

        const auto *spot{values.find(make_location("minecraft:item/7777"))};
        expect(spot != nullptr);
        if (spot != nullptr) {
            expect(*spot == 7777);
        }
        const auto *first{values.at(registry_id{0U})};
        const auto *last{
            values.at(registry_id{static_cast<std::uint32_t>(count - 1U)})};
        expect(first != nullptr);
        expect(last != nullptr);
        if (first != nullptr && last != nullptr) {
            expect(*first == 0);
            expect(*last == static_cast<int>(count - 1U));
        }
        const std::string dumped{values.dump()};
        expect(static_cast<std::size_t>(
                   std::count(dumped.begin(), dumped.end(), '\n')) == count);

        registry<int> reversed{std::string{"test.large"}};
        for (std::size_t index{count}; index > 0; --index) {
            expect(reversed
                       .add(make_location("minecraft:item/" +
                                          std::to_string(index - 1U)),
                            static_cast<int>(index - 1U))
                       .has_value());
        }
        expect(reversed.size() == count);
        expect(to_hex(reversed.fingerprint()) == to_hex(values.fingerprint()));
        expect(reversed.dump() != values.dump());
    };

    "registry/entries expose id, location and value in order"_test = [] {
        registry<std::string> values{std::string{"test.entries"}};
        expect(values.add(make_location("minecraft:a"), std::string{"alpha"})
                   .has_value());
        expect(values.add(make_location("test:b"), std::string{"beta"})
                   .has_value());
        const auto entries{values.entries()};
        expect(entries.size() == 2U);
        if (entries.size() == 2U) {
            expect(entries[0].id.value() == 0U);
            expect(entries[0].location != nullptr);
            expect(entries[0].value != nullptr);
            if (entries[0].location != nullptr && entries[0].value != nullptr) {
                expect(entries[0].location->to_string() == "minecraft:a");
                expect(*entries[0].value == "alpha");
            }
            expect(entries[1].id.value() == 1U);
            if (entries[1].location != nullptr && entries[1].value != nullptr) {
                expect(entries[1].location->to_string() == "test:b");
                expect(*entries[1].value == "beta");
            }
            // Frozen snapshots never move entries: refs stay valid.
            expect(values.freeze().has_value());
            const auto frozen_entries{values.entries()};
            expect(frozen_entries.size() == 2U);
            if (frozen_entries.size() == 2U) {
                expect(frozen_entries[0].value == entries[0].value);
                expect(*frozen_entries[0].value == "alpha");
            }
        }
    };

    "registry/supports move-only value types"_test = [] {
        registry<std::unique_ptr<int>> values{std::string{"test.move_only"}};
        expect(
            values.add(make_location("minecraft:a"), std::make_unique<int>(7))
                .has_value());
        const auto *value{values.find(make_location("minecraft:a"))};
        expect(value != nullptr);
        if (value != nullptr) {
            expect(**value == 7);
        }
        expect(
            values.add(make_location("minecraft:b"), std::make_unique<int>(9))
                .has_value());
        const auto entries{values.entries()};
        expect(entries.size() == 2U);
        if (entries.size() == 2U) {
            expect(*entries[1].value != nullptr);
            expect(**entries[1].value == 9);
        }

        // Fingerprint covers locations/type name, never the payload type or
        // values: a plain int registry with the same identity matches.
        registry<int> plain{std::string{"test.move_only"}};
        expect(plain.add(make_location("minecraft:a"), 7).has_value());
        expect(plain.add(make_location("minecraft:b"), 9).has_value());
        expect(to_hex(plain.fingerprint()) == to_hex(values.fingerprint()));
    };

    "registry/registry_id is strongly typed, ordered and hashable"_test = [] {
        expect(!registry_id{}.valid());
        expect(registry_id::invalid() == registry_id{});
        expect(!registry_id{registry_id::invalid_value}.valid());
        expect(registry_id{0U}.valid());
        expect(registry_id{0U}.value() == 0U);
        expect(registry_id{7U}.to_string() == "7");
        expect(registry_id::invalid().to_string() == "invalid");
        expect(registry_id{1U} < registry_id{2U});
        expect(registry_id{2U} > registry_id{1U});
        expect(registry_id{3U} == registry_id{3U});
        expect(registry_id::invalid() > registry_id{0U}) << "sentinel is max";

        std::unordered_map<registry_id, std::string> names;
        names.emplace(registry_id{2U}, "two");
        names.emplace(registry_id{9U}, "nine");
        expect(names.size() == 2U);
        const auto found{names.find(registry_id{9U})};
        expect(found != names.end());
        if (found != names.end()) {
            expect(found->second == "nine");
        }
        expect(names.find(registry_id{3U}) == names.end());

        registry<int> values{std::string{"test.strong_id"}};
        expect(values.add(make_location("minecraft:a"), 1).has_value());
        expect(names.contains(values.ids()[0]) == false);
    };
};

} // namespace
