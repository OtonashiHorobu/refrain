// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/carving_mask.hpp>

#include <boost/ut.hpp>

#include <vector>

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/carving-mask"> carving_mask_tests = [] {
    "runs are visited per column bottom to top"_test = [] {
        carving_mask mask{-2, 3};
        expect(mask.empty());
        mask.carve(1, 0, 2);
        mask.carve(1, 1, 2);
        mask.carve(1, 2, 2);
        expect(!mask.empty());
        expect(mask.count() == 3U);
        expect(mask.test(1, 0, 2));
        expect(!mask.test(1, -1, 2));

        struct run {
            jint x;
            jint z;
            jint bottom;
            jint top;
        };
        std::vector<run> runs;
        mask.visit([&runs](jint x, jint z, jint bottom, jint top) {
            runs.push_back({x, z, bottom, top});
        });
        expect(runs.size() == 1U);
        if (!runs.empty()) {
            expect(runs[0].x == 1);
            expect(runs[0].z == 2);
            expect(runs[0].bottom == 0);
            expect(runs[0].top == 2);
        }
    };

    "holes split runs and bounds are ignored"_test = [] {
        carving_mask mask{-2, 3};
        mask.carve(0, -2, 0);
        mask.carve(0, 0, 0);
        mask.carve(16, 0, 0);
        mask.carve(0, 4, 0);
        expect(mask.count() == 2U);
        std::vector<std::pair<jint, jint>> runs;
        mask.visit([&runs](jint, jint, jint bottom, jint top) {
            runs.emplace_back(bottom, top);
        });
        expect(runs.size() == 2U);
        if (runs.size() == 2U) {
            expect(runs[0].first == -2 && runs[0].second == -2);
            expect(runs[1].first == 0 && runs[1].second == 0);
        }
    };

    "adjacent columns stay separate"_test = [] {
        carving_mask mask{0, 1};
        mask.carve(0, 0, 0);
        mask.carve(0, 1, 0);
        mask.carve(0, 0, 1);
        expect(mask.count() == 3U);
        int columns = 0;
        mask.visit([&columns](jint, jint, jint, jint) { ++columns; });
        expect(columns == 2);
    };
};
