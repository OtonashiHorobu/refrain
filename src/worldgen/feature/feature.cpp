// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature.hpp>

namespace refrain::worldgen::feature {

auto simple_block_feature::place(feature_context &context, worldgen_random &,
                                 const block_pos &origin) const -> bool {
    if (state_ == no_block_state) {
        return false;
    }
    (void)schedule_tick_;
    return context.placement.region->set_block_state(origin.x, origin.y,
                                                     origin.z, state_);
}

} // namespace refrain::worldgen::feature
