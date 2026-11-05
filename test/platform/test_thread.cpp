// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/thread.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <fstream>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "platform/thread id is non-zero"_test = [] {
        expect(refrain::platform::current_thread_id() != 0U);
    };

    "platform/thread name is visible to the OS"_test = [] {
        const auto named{
            refrain::platform::set_current_thread_name("refrain-test")};
#if defined(__linux__)
        const auto id{refrain::platform::current_thread_id()};
        expect(named);
        // The kernel keeps a 15-byte comm string; the test name fits.
        std::ifstream comm_file{fmt::format("/proc/self/task/{}/comm", id)};
        std::string comm{};
        std::getline(comm_file, comm);
        expect(comm == "refrain-test") << comm;
#else
        // Other platforms may not implement OS-visible thread names yet; the
        // call must still be safe and return a value.
        static_cast<void>(named);
#endif
    };

    "platform/thread affinity is best-effort and observable"_test = [] {
#if defined(__linux__)
        cpu_set_t original;
        CPU_ZERO(&original);
        if (::sched_getaffinity(0, sizeof(original), &original) != 0) {
            return;
        }
        std::uint32_t cpu{0};
        bool found{false};
        for (int index{0}; index < CPU_SETSIZE; ++index) {
            if (CPU_ISSET(index, &original) != 0) {
                cpu = static_cast<std::uint32_t>(index);
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }
        const std::uint32_t cpus[]{cpu};
        if (!refrain::platform::set_current_thread_affinity(cpus)) {
            return; // sandboxed runners may forbid affinity changes
        }
        expect(::sched_getcpu() == static_cast<int>(cpu));
        static_cast<void>(::sched_setaffinity(0, sizeof(original), &original));
#else
        static_cast<void>(refrain::platform::set_current_thread_affinity({}));
#endif
    };

    "platform/thread priority is best-effort"_test = [] {
        using refrain::platform::thread_priority;
        if (!refrain::platform::set_current_thread_priority(
                thread_priority::normal)) {
            return; // restricted runner
        }
        // Lowering priority is normally allowed without privilege, but raising
        // it back requires CAP_SYS_NICE. Restoring an unprivileged thread is
        // therefore best-effort and must not be asserted.
        if (refrain::platform::set_current_thread_priority(
                thread_priority::background)) {
            static_cast<void>(refrain::platform::set_current_thread_priority(
                thread_priority::normal));
        }
    };
};

} // namespace
