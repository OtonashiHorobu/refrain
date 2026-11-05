// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION

#include <refrain/reflect/glaze_meta.hpp>

#include <boost/ut.hpp>

#include <string>

namespace glaze_smoke {

struct stats {
    int health{};
    int xp{};
    std::string name{};
};

} // namespace glaze_smoke

REFR_REFLECT(type(glaze_smoke::stats), REFR_FIELD(health), REFR_FIELD(xp),
             REFR_FIELD(name))
REFR_GLAZE_META(glaze_smoke::stats)

namespace {

using namespace boost::ut;

suite<"reflect/glaze"> glaze_meta_tests = [] {
    "reflect/glaze meta writes and reads reflected fields"_test = [] {
        glaze_smoke::stats input{};
        input.health = 7;
        input.xp = 11;
        input.name = "refrain";

        std::string json{};
        expect(!glz::write_json(input, json));
        expect(json.find("\"health\":7") != std::string::npos);
        expect(json.find("\"xp\":11") != std::string::npos);
        expect(json.find("\"name\":\"refrain\"") != std::string::npos);

        glaze_smoke::stats output{};
        expect(!glz::read_json(output, json));
        expect(output.health == 7);
        expect(output.xp == 11);
        expect(output.name == "refrain");
    };
};

} // namespace

#endif
