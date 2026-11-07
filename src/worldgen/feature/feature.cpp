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

auto disk_feature::place(feature_context &context, worldgen_random &random,
                         const block_pos &origin) const -> bool {
    jint top = origin.y + half_height_;
    const jint bottom = origin.y - half_height_ - 1;
    const jint radius = radius_->sample(random);
    bool placed_any = false;

    for (jint x = origin.x - radius; x <= origin.x + radius; ++x) {
        const jint xd = x - origin.x;
        for (jint z = origin.z - radius; z <= origin.z + radius; ++z) {
            const jint zd = z - origin.z;
            if (xd * xd + zd * zd > radius * radius) {
                continue;
            }
            bool placed_above = false;
            for (jint y = top; y > bottom; --y) {
                if (!target_->test(*context.placement.region,
                                   *context.placement.states,
                                   block_pos{.x = x, .y = y, .z = z})) {
                    placed_above = false;
                    continue;
                }
                const auto state = state_;
                if (state == no_block_state) {
                    continue;
                }
                if (!context.placement.region->set_block_state(x, y, z,
                                                               state)) {
                    continue;
                }
                if (!placed_above) {
                    context.placement.chunk->mark_post_processing(x, y + 1, z);
                }
                placed_any = true;
                placed_above = true;
            }
        }
    }
    return placed_any;
}

} // namespace refrain::worldgen::feature
