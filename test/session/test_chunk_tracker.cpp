// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/chunk_tracker.hpp>

#include <boost/ut.hpp>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace session = refrain::session;

suite chunk_tracker_tests = [] {
    "session/chunk tracker emits viewport changes only"_test = [] {
        session::chunk_tracker tracker{};
        expect(tracker.update(1, 4, 6, 3));
        auto requests{tracker.take_requests()};
        expect(requests.size() == 1U);
        if (!requests.empty()) {
            expect(requests.front().connection == 1U);
            expect(requests.front().radius == 3);
        }
        expect(!tracker.update(1, 4, 6, 3));
        expect(tracker.take_requests().empty());
        expect(tracker.update(1, 5, 6, 3));
        requests = tracker.take_requests();
        expect(requests.size() == 1U);
        expect(tracker.remove(1));
        expect(!tracker.update(0, 0, 0, 3));
    };
};

} // namespace
