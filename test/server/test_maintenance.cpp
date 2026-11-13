// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Autosave failure reporting. A world that cannot be written fails on every
// autosave, and one line per autosave buries the first one; the tracker decides
// which repeats reach the operator and keeps the counters the console reports.

#include <refrain/server/maintenance_service.hpp>

#include <boost/ut.hpp>

#include <string>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

using refrain::server::write_failure_tracker;

suite<"server/maintenance"> maintenance_tests = [] {
    "the first write failure is reported once"_test = [] {
        write_failure_tracker tracker{4};
        expect(tracker.consecutive() == 0U);
        expect(tracker.record("cannot open file"));
        expect(tracker.consecutive() == 1U);
        expect(tracker.last_error() == "cannot open file");
        // The same message again is not worth another line.
        expect(!tracker.record("cannot open file"));
        expect(tracker.consecutive() == 2U);
    };

    "a changed failure message is reported even while it repeats"_test = [] {
        write_failure_tracker tracker{4};
        expect(tracker.record("cannot open file"));
        expect(tracker.record("disk is full"));
        expect(tracker.last_error() == "disk is full");
        expect(!tracker.record("disk is full"));
    };

    "a persistent failure resurfaces on its reporting interval"_test = [] {
        write_failure_tracker tracker{4};
        expect(tracker.record("no space"));
        // 2, 3 and 5 to 7 are suppressed; the interval reports again.
        expect(!tracker.record("no space"));
        expect(!tracker.record("no space"));
        expect(tracker.record("no space"));
        expect(tracker.consecutive() == 4U);
        expect(!tracker.record("no space"));
        expect(!tracker.record("no space"));
        expect(!tracker.record("no space"));
        expect(tracker.record("no space"));
        expect(tracker.consecutive() == 8U);
    };

    "a success clears the counters and the last message"_test = [] {
        write_failure_tracker tracker{4};
        expect(tracker.record("no space"));
        expect(!tracker.record("no space"));
        expect(tracker.consecutive() == 2U);
        tracker.record_success();
        expect(tracker.consecutive() == 0U);
        expect(tracker.last_error().empty());
        // The next failure is a first failure again and is reported.
        expect(tracker.record("no space"));
    };

    "a zero interval still reports the first failure"_test = [] {
        write_failure_tracker tracker{0};
        expect(tracker.record("cannot open file"));
        expect(tracker.consecutive() == 1U);
    };
};

} // namespace
