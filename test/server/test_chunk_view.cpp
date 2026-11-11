// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_view.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::server::chunk_coordinate;
using refrain::server::chunk_distance;
using refrain::server::chunks_outside_view;

suite chunk_view_tests = [] {
    "server/chunk distance is the larger axis delta"_test = [] {
        expect(chunk_distance({0, 0}, {3, -5}) == 5);
        expect(chunk_distance({0, 0}, {0, 0}) == 0);
        expect(chunk_distance({-2, 7}, {1, 4}) == 3);
    };

    "server/chunk distance saturates at the int32 limit"_test = [] {
        constexpr auto low{std::numeric_limits<std::int32_t>::min()};
        constexpr auto high{std::numeric_limits<std::int32_t>::max()};
        expect(chunk_distance({low, 0}, {high, 0}) ==
               std::numeric_limits<std::int32_t>::max());
    };

    "server/viewport keeps every chunk on its boundary"_test = [] {
        const std::vector<chunk_coordinate> tracked{
            {-2, -2}, {-2, 2}, {2, -2}, {2, 2}, {0, 0}};
        expect(chunks_outside_view(tracked, {0, 0}, 2).empty());
    };

    "server/viewport forgets the chunks a shift leaves behind"_test = [] {
        const std::vector<chunk_coordinate> tracked{
            {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}};
        const auto outside{chunks_outside_view(tracked, {1, 0}, 2)};
        expect(outside.size() == 1U);
        expect(outside[0] == chunk_coordinate{-2, 0});
    };

    "server/viewport forgets the ring a radius shrink leaves behind"_test = [] {
        const std::vector<chunk_coordinate> tracked{
            {0, 0}, {1, 0}, {2, 0}, {0, 2}, {2, 2}};
        const auto outside{chunks_outside_view(tracked, {0, 0}, 1)};
        expect(outside.size() == 3U);
        expect(outside[0] == chunk_coordinate{0, 2});
        expect(outside[1] == chunk_coordinate{2, 0});
        expect(outside[2] == chunk_coordinate{2, 2});
    };

    "server/viewport orders the forgotten chunks by x then z"_test = [] {
        const std::vector<chunk_coordinate> tracked{
            {5, 5}, {5, 1}, {1, 5}, {1, 1}};
        const auto outside{chunks_outside_view(tracked, {0, 0}, 0)};
        expect(outside.size() == 4U);
        expect(outside[0] == chunk_coordinate{1, 1});
        expect(outside[1] == chunk_coordinate{1, 5});
        expect(outside[2] == chunk_coordinate{5, 1});
        expect(outside[3] == chunk_coordinate{5, 5});
    };

    "server/viewport handles negative centres"_test = [] {
        const std::vector<chunk_coordinate> tracked{{-3, -3}, {-1, -1}};
        const auto outside{chunks_outside_view(tracked, {-1, -1}, 1)};
        expect(outside.size() == 1U);
        expect(outside[0] == chunk_coordinate{-3, -3});
    };

    "server/viewport treats a negative radius as zero"_test = [] {
        const std::vector<chunk_coordinate> tracked{{0, 0}, {1, 1}};
        const auto outside{chunks_outside_view(tracked, {0, 0}, -1)};
        expect(outside.size() == 1U);
        expect(outside[0] == chunk_coordinate{1, 1});
    };
};

} // namespace
