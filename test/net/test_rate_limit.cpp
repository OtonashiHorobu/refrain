// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/rate_limit.hpp>

#include <boost/ut.hpp>

#include <cstdint>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

suite rate_limit_tests = [] {
    "net/rate limiter allows a one-second burst and refills"_test = [] {
        net::token_bucket bucket{2};
        expect(bucket.enabled());
        expect(bucket.allow(0));
        expect(bucket.allow(0));
        expect(!bucket.allow(0));
        // Half a second refills one token, not two.
        expect(bucket.allow(500'000'000));
        expect(!bucket.allow(500'000'000));
        expect(bucket.allow(1'000'000'000));
        expect(!bucket.allow(1'000'000'000));
    };

    "net/rate limiter disabled always allows"_test = [] {
        net::token_bucket bucket{0};
        expect(!bucket.enabled());
        for (int index{0}; index < 1000; ++index) {
            expect(bucket.allow(index));
        }
        net::token_bucket negative{-1};
        expect(!negative.enabled());
        expect(negative.allow(0));
    };

    "net/idle timeout predicate honors the deadline"_test = [] {
        expect(!net::idle_timed_out(100, 200, 0));
        expect(!net::idle_timed_out(100, 200, 100));
        expect(net::idle_timed_out(100, 300, 100));
    };
};

} // namespace
