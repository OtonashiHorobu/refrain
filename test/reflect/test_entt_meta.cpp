// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION

#include <refrain/reflect/entt_meta.hpp>

#include <boost/ut.hpp>

#include <string>

namespace entt_smoke {

struct stats {
    int health{};
    std::string name{};
};

} // namespace entt_smoke

REFR_REFLECT(type(entt_smoke::stats), REFR_FIELD(health), REFR_FIELD(name))

namespace {

using namespace boost::ut;
using namespace entt::literals;

suite<"reflect/entt"> entt_meta_tests = [] {
    "reflect/entt meta reads and writes reflected fields"_test = [] {
        refrain::reflect::register_entt_meta<entt_smoke::stats>();
        refrain::reflect::register_entt_meta<entt_smoke::stats>();

        const auto type =
            entt::resolve(entt::type_hash<entt_smoke::stats>::value());
        expect(static_cast<bool>(type));

        const auto health = type.data("health"_hs);
        expect(static_cast<bool>(health));

        entt_smoke::stats object{};
        object.health = 7;
        object.name = "refrain";
        expect(health.get(object).cast<int>() == 7);
        health.set(object, 9);
        expect(object.health == 9);
    };
};

} // namespace

#endif
