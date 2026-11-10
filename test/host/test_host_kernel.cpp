// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:host.kernel` adapter tests: worker jobs, cancellation handles and
// the process-local event bus.

#include <refrain/app/host_context.hpp>
#include <refrain/host/kernel_api.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace host = refrain::host;
namespace app = refrain::app;
namespace mod = refrain::mod;

std::atomic<int> g_job_calls{0};

void job_callback(void *userdata) {
    auto *counter{static_cast<std::atomic<int> *>(userdata)};
    ++*counter;
    ++g_job_calls;
}

struct event_sink {
    std::atomic<int> calls{0};
    std::string payload{};
};

void event_callback(void *userdata, void *payload, std::uint64_t size) {
    auto *sink{static_cast<event_sink *>(userdata)};
    ++sink->calls;
    sink->payload.assign(static_cast<const char *>(payload),
                         static_cast<std::size_t>(size));
}

const suite host_kernel_tests = [] {
    "host/kernel jobs run and wait"_test = [] {
        mod::host owner{};
        app::host_context context{owner};
        const auto api{host::make_kernel_provider(context)};

        std::atomic<int> counter{0};
        g_job_calls = 0;
        const auto job{api.job_submit(2, &job_callback, &counter)};
        expect(job.has_value());
        if (job) {
            expect(api.job_wait(*job).has_value());
            expect(counter.load() == 1);
            expect(g_job_calls.load() == 1);
            // Waiting again finds no active handle.
            const auto again{api.job_wait(*job)};
            expect(!again.has_value());
            expect(again.error() ==
                   refrain::host::kernel::kernel_error::not_found);
        }

        const auto invalid_priority{api.job_submit(9, &job_callback, &counter)};
        expect(!invalid_priority.has_value());
        expect(invalid_priority.error() ==
               refrain::host::kernel::kernel_error::invalid_argument);

        const auto missing_callback{api.job_submit(2, nullptr, &counter)};
        expect(!missing_callback.has_value());
        expect(missing_callback.error() ==
               refrain::host::kernel::kernel_error::invalid_argument);
    };

    "host/kernel events publish to subscribers"_test = [] {
        mod::host owner{};
        app::host_context context{owner};
        const auto api{host::make_kernel_provider(context)};

        event_sink sink{};
        const auto subscription{
            api.event_subscribe(42, &event_callback, &sink)};
        expect(subscription.has_value());
        if (!subscription) {
            return;
        }

        const std::array<char, 3> payload{'a', 'b', 'c'};
        api.event_publish(42, const_cast<char *>(payload.data()),
                          payload.size());
        expect(sink.calls.load() == 1);
        expect(sink.payload == "abc");

        // A different event type does not reach the subscriber.
        api.event_publish(7, const_cast<char *>(payload.data()),
                          payload.size());
        expect(sink.calls.load() == 1);

        expect(api.event_unsubscribe(*subscription).has_value());
        api.event_publish(42, const_cast<char *>(payload.data()),
                          payload.size());
        expect(sink.calls.load() == 1);

        const auto again{api.event_unsubscribe(*subscription)};
        expect(!again.has_value());
        expect(again.error() == refrain::host::kernel::kernel_error::not_found);

        const auto null_callback{api.event_subscribe(42, nullptr, &sink)};
        expect(!null_callback.has_value());
        expect(null_callback.error() ==
               refrain::host::kernel::kernel_error::invalid_argument);
    };
};

} // namespace
