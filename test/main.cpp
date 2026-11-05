// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <boost/ut.hpp>

auto main(int argc, char **argv) -> int {
    auto cfg = boost::ut::run_cfg{
        .report_errors = true,
        .argc = argc,
        .argv = const_cast<const char **>(argv),
    };
    const auto failures = boost::ut::cfg<boost::ut::override>.run(cfg);
    return failures ? 1 : 0;
}
