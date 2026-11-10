// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Differential tests for the density sampling paths: the cell lattice of the
// reference's interpolated node, and the per-volume cache behind `cache` nodes.
// The reference implementation below is an independent transcription of the
// reference algorithm, so a change to the sampler that is not bit-identical
// fails here.

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/mth.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

/// An independent implementation of the reference interpolated node. It
/// reproduces the three volume branches, the cell lattice, the chunk-edge
/// clamps and the float order of the y accumulation.
class reference_interpolated {
  public:
    reference_interpolated(const sampler &input, jint cell_size_xz,
                           jint cell_size_y)
        : input_{input}, cell_size_xz_{cell_size_xz}, cell_size_y_{cell_size_y},
          cell_size_xz_inv_{1.0F / static_cast<float>(cell_size_xz)},
          cell_size_y_inv_{1.0F / static_cast<float>(cell_size_y)} {}

    [[nodiscard]] auto sample(const sampler_context &context,
                              const volume &vol) const -> buffer {
        if ((vol.step_block_x() == cell_size_xz_ || vol.size_x() == 1) &&
            (vol.step_block_y() == cell_size_y_ || vol.size_y() == 1) &&
            (vol.step_block_z() == cell_size_xz_ || vol.size_z() == 1) &&
            floor_mod(vol.min_block_x(), cell_size_xz_) == 0 &&
            floor_mod(vol.min_block_y(), cell_size_y_) == 0 &&
            floor_mod(vol.min_block_z(), cell_size_xz_) == 0) {
            buffer direct{vol.size()};
            input_.sample_volume(context, direct, vol);
            return direct;
        }
        if (vol.step_block_x() == 1 && vol.step_block_y() == 1 &&
            vol.step_block_z() == 1) {
            buffer block{vol.size()};
            sample_block_step(context, block, vol);
            return block;
        }
        const volume block_volume{vol.size_x() * vol.step_block_x(),
                                  vol.size_y() * vol.step_block_y(),
                                  vol.size_z() * vol.step_block_z(),
                                  vol.min_block_x(),
                                  vol.min_block_y(),
                                  vol.min_block_z()};
        buffer block_buffer{block_volume.size()};
        sample_block_step(context, block_buffer, block_volume);
        buffer output{vol.size()};
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                for (jint y = 0; y < vol.size_y(); ++y) {
                    output.set(
                        vol.index_unchecked(x, y, z),
                        block_buffer.get(block_volume.index_unchecked(
                            x * vol.step_block_x(), y * vol.step_block_y(),
                            z * vol.step_block_z())));
                }
            }
        }
        return output;
    }

  private:
    void sample_block_step(const sampler_context &context, buffer &output,
                           const volume &vol) const {
        const jint min_cell_x = floor_div(vol.min_block_x(), cell_size_xz_);
        const jint min_cell_y = floor_div(vol.min_block_y(), cell_size_y_);
        const jint min_cell_z = floor_div(vol.min_block_z(), cell_size_xz_);
        const jint max_cell_x = floor_div(vol.max_block_x(), cell_size_xz_);
        const jint max_cell_y = floor_div(vol.max_block_y(), cell_size_y_);
        const jint max_cell_z = floor_div(vol.max_block_z(), cell_size_xz_);
        const jint cell_count_x = max_cell_x - min_cell_x + 1;
        const jint cell_count_y = max_cell_y - min_cell_y + 1;
        const jint cell_count_z = max_cell_z - min_cell_z + 1;
        const volume cell_volume{
            floor_mod(vol.max_block_x(), cell_size_xz_) == 0 ? cell_count_x
                                                             : cell_count_x + 1,
            floor_mod(vol.max_block_y(), cell_size_y_) == 0 ? cell_count_y
                                                            : cell_count_y + 1,
            floor_mod(vol.max_block_z(), cell_size_xz_) == 0 ? cell_count_z
                                                             : cell_count_z + 1,
            min_cell_x * cell_size_xz_,
            min_cell_y * cell_size_y_,
            min_cell_z * cell_size_xz_,
            cell_size_xz_,
            cell_size_y_,
            cell_size_xz_};
        buffer cells{cell_volume.size()};
        input_.sample_volume(context, cells, cell_volume);
        for (jint cell_z = 0; cell_z < cell_count_z; ++cell_z) {
            const jint next_cell_z =
                std::min(cell_z + 1, cell_volume.size_z() - 1);
            for (jint cell_x = 0; cell_x < cell_count_x; ++cell_x) {
                const jint next_cell_x =
                    std::min(cell_x + 1, cell_volume.size_x() - 1);
                float v000 =
                    cells.get(cell_volume.index_unchecked(cell_x, 0, cell_z));
                float v100 = cells.get(
                    cell_volume.index_unchecked(next_cell_x, 0, cell_z));
                float v001 = cells.get(
                    cell_volume.index_unchecked(cell_x, 0, next_cell_z));
                float v101 = cells.get(
                    cell_volume.index_unchecked(next_cell_x, 0, next_cell_z));
                for (jint cell_y = 0; cell_y < cell_count_y; ++cell_y) {
                    const jint next_cell_y =
                        std::min(cell_y + 1, cell_volume.size_y() - 1);
                    const float v010 = cells.get(cell_volume.index_unchecked(
                        cell_x, next_cell_y, cell_z));
                    const float v110 = cells.get(cell_volume.index_unchecked(
                        next_cell_x, next_cell_y, cell_z));
                    const float v011 = cells.get(cell_volume.index_unchecked(
                        cell_x, next_cell_y, next_cell_z));
                    const float v111 = cells.get(cell_volume.index_unchecked(
                        next_cell_x, next_cell_y, next_cell_z));
                    fill_cell(output, vol, cell_volume, cell_x, cell_y, cell_z,
                              v000, v100, v010, v110, v001, v101, v011, v111);
                    v000 = v010;
                    v100 = v110;
                    v001 = v011;
                    v101 = v111;
                }
            }
        }
    }

    void fill_cell(buffer &output, const volume &output_volume,
                   const volume &cell_volume, jint cell_x, jint cell_y,
                   jint cell_z, float v000, float v100, float v010, float v110,
                   float v001, float v101, float v011, float v111) const {
        const jint cell_output_x =
            cell_volume.block_x(cell_x) - output_volume.min_block_x();
        const jint cell_output_y =
            cell_volume.block_y(cell_y) - output_volume.min_block_y();
        const jint cell_output_z =
            cell_volume.block_z(cell_z) - output_volume.min_block_z();
        const jint x0 = std::max(0, -cell_output_x);
        const jint y0 = std::max(0, -cell_output_y);
        const jint z0 = std::max(0, -cell_output_z);
        const jint x1 =
            std::min(cell_size_xz_, output_volume.size_x() - cell_output_x) - 1;
        const jint y1 =
            std::min(cell_size_y_, output_volume.size_y() - cell_output_y) - 1;
        const jint z1 =
            std::min(cell_size_xz_, output_volume.size_z() - cell_output_z) - 1;
        for (jint z = z0; z <= z1; ++z) {
            const jint output_z = cell_output_z + z;
            const float alpha_z = static_cast<float>(z) * cell_size_xz_inv_;
            const float v00 = mth::lerp(alpha_z, v000, v001);
            const float v01 = mth::lerp(alpha_z, v010, v011);
            const float v10 = mth::lerp(alpha_z, v100, v101);
            const float v11 = mth::lerp(alpha_z, v110, v111);
            for (jint x = x0; x <= x1; ++x) {
                const jint output_x = cell_output_x + x;
                const float alpha_x = static_cast<float>(x) * cell_size_xz_inv_;
                const float v_0 = mth::lerp(alpha_x, v00, v10);
                const float v_1 = mth::lerp(alpha_x, v01, v11);
                const float value_step = (v_1 - v_0) * cell_size_y_inv_;
                float value = v_0 + value_step * static_cast<float>(y0);
                std::size_t index = output_volume.index_unchecked(
                    output_x, cell_output_y + y0, output_z);
                for (jint y = y0; y <= y1; ++y) {
                    output.set(index, value);
                    ++index;
                    value += value_step;
                }
            }
        }
    }

    const sampler &input_;
    jint cell_size_xz_;
    jint cell_size_y_;
    float cell_size_xz_inv_;
    float cell_size_y_inv_;
};

auto real_data_root() -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3/data";
}

auto has_real_data() -> bool {
    return std::filesystem::exists(
        real_data_root() + "/minecraft/worldgen/noise_settings/overworld.json");
}

auto load_settings(std::string_view id) -> refrain::result<noise_settings> {
    return load_noise_settings(real_data_root(), id, algorithm::xoroshiro,
                               12345);
}

/// Compares `sampler`'s cell output with the independent reference on one
/// volume. Both sample the same input graph, so the comparison is exact.
void check_cell_volume(const sampler &cell, const sampler &input,
                       jint cell_size_xz, jint cell_size_y, const volume &vol) {
    const sampler_context context;
    buffer actual{vol.size()};
    cell.sample_volume(context, actual, vol);
    const auto expected =
        reference_interpolated{input, cell_size_xz, cell_size_y}.sample(context,
                                                                        vol);
    expect(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        expect(bits(actual.get(i)) == bits(expected.get(i)))
            << "volume mismatch at " << i << " got " << actual.get(i)
            << " expected " << expected.get(i);
    }
}

auto make_synthetic_input() -> sampler_ptr {
    auto source = new_positional_factory(algorithm::xoroshiro, 12345)
                      ->from_hash_of("refrain:density_sampling_test");
    normal_noise_parameters parameters;
    parameters.base_octave = -2;
    parameters.octave_count = 3;
    const normal_noise normal{parameters};
    auto stack = std::make_shared<noise_stack>(normal.create(*source));
    return add(add(gradient_y(-64, 320, 0.0F, 1.0F),
                   density::noise(stack, 0.25, 0.125)),
               mul(gradient_y(0, 128, 1.0F, -1.0F), density::constant(0.5F)));
}

} // namespace

suite<"worldgen/density-sampling"> density_sampling_tests = [] {
    "the cell lattice matches the reference on every volume shape"_test = [] {
        const std::array<volume, 5> volumes{{
            // Aligned: the reference samples the input over the volume itself.
            volume{4, 3, 2, 0, 64, 0, 4, 8, 4},
            // Block step with unaligned edges on all three axes.
            volume{5, 9, 3, -4, 56, 0},
            // Block step whose minimum starts inside a cell.
            volume{7, 11, 4, -3, -63, 2},
            // Non-block step samples a block volume and picks the samples.
            volume{3, 2, 2, -2, 52, 1, 3, 5, 2},
            // A one-block volume still builds its 2x2x2 corner lattice.
            volume{1, 1, 1, 3, 61, 5},
        }};
        for (const auto &vol : volumes) {
            auto cell = interpolated(make_synthetic_input(), 4, 8);
            auto input = make_synthetic_input();
            check_cell_volume(*cell, *input, 4, 8, vol);
        }
    };

    "the cell lattice matches the reference on real dimension densities"_test =
        [] {
            if (!has_real_data()) {
                return;
            }
            struct dimension_case {
                const char *id;
                jint cell_xz;
                jint cell_y;
                jint min_y;
                jint height;
            };
            const std::array<dimension_case, 3> cases{{
                {"minecraft/overworld", 4, 8, -64, 384},
                {"minecraft/nether", 4, 8, 0, 128},
                {"minecraft/end", 8, 4, 0, 128},
            }};
            for (const auto &entry : cases) {
                auto settings = load_settings(entry.id);
                expect(settings.has_value()) << entry.id;
                if (!settings) {
                    continue;
                }
                auto cell = interpolated(settings->router.final_density,
                                         entry.cell_xz, entry.cell_y);
                check_cell_volume(*cell, *settings->router.final_density,
                                  entry.cell_xz, entry.cell_y,
                                  volume{16, 32, 16, 0, entry.min_y, 0});
                check_cell_volume(*cell, *settings->router.final_density,
                                  entry.cell_xz, entry.cell_y,
                                  volume{5, 9, 3, -4, entry.min_y + 8, 0});
            }
        };

    "an affine density is reproduced exactly by the cell lattice"_test = [] {
        // Every corner value and every cell step is exactly representable, so
        // linear interpolation returns the per-block value bit for bit. This
        // is the case where the reference guarantees equality.
        auto cell = interpolated(gradient_y(0, 128, 0.0F, 128.0F), 4, 8);
        auto input = gradient_y(0, 128, 0.0F, 128.0F);
        const volume vol{6, 9, 4, 1, 19, 3};
        const sampler_context context;
        buffer values{vol.size()};
        cell->sample_volume(context, values, vol);
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                for (jint y = 0; y < vol.size_y(); ++y) {
                    const jint block_y = vol.block_y(y);
                    expect(bits(values.get(vol.index_unchecked(x, y, z))) ==
                           bits(input->sample_value(context, vol.block_x(x),
                                                    block_y, vol.block_z(z))))
                        << "affine value differs at y=" << block_y;
                }
            }
        }
    };

    "a nonlinear density is not reproduced by the cell lattice"_test = [] {
        // The cell lattice is an approximation: the reference guarantees the
        // lattice arithmetic, not equality with a per-block sample. This is
        // why the final density cannot be sampled on a lattice as a whole.
        auto settings = load_settings("minecraft/overworld");
        if (!settings) {
            return;
        }
        const volume chunk{16, 384, 16, 0, -64, 0};
        const sampler_context context;
        buffer per_block{chunk.size()};
        settings->router.final_density->sample_volume(context, per_block,
                                                      chunk);
        const auto latticed =
            reference_interpolated{*settings->router.final_density, 4, 8}
                .sample(context, chunk);
        std::size_t differences = 0;
        std::size_t sign_changes = 0;
        for (std::size_t i = 0; i < per_block.size(); ++i) {
            if (bits(per_block.get(i)) != bits(latticed.get(i))) {
                ++differences;
            }
            if ((per_block.get(i) > 0.0F) != (latticed.get(i) > 0.0F)) {
                ++sign_changes;
            }
        }
        expect(differences > 0U)
            << "the lattice unexpectedly reproduced every value";
        expect(sign_changes > 0U)
            << "the lattice would not change any generated block";
    };

    "cache nodes return the values of the uncached graph"_test = [] {
        auto make_input = [] { return make_synthetic_input(); };
        const std::array<volume, 4> volumes{{
            volume{16, 32, 16, 0, -64, 0},
            volume{5, 9, 3, -4, 56, 0, 4, 8, 4},
            volume{3, 2, 2, -2, 52, 1, 3, 5, 2},
            volume{1, 32, 1, 7, -64, 9},
        }};
        for (const auto &vol : volumes) {
            auto cached = cache(make_input());
            auto plain = make_input();
            const sampler_context context;
            buffer cached_values{vol.size()};
            buffer plain_values{vol.size()};
            cached->sample_volume(context, cached_values, vol);
            plain->sample_volume(context, plain_values, vol);
            expect(cached_values.size() == plain_values.size());
            for (std::size_t i = 0; i < cached_values.size(); ++i) {
                expect(bits(cached_values.get(i)) == bits(plain_values.get(i)))
                    << "cached value differs at " << i;
            }
        }
    };

    "a cached volume is resampled when the grid changes"_test = [] {
        // A cache keeps one volume. Sampling another grid and returning to the
        // first must reproduce the uncached values, not a stale buffer.
        auto cached = cache(make_synthetic_input());
        auto plain = make_synthetic_input();
        const volume first{16, 8, 16, 0, -64, 0};
        const volume second{16, 12, 16, 16, -64, 0};
        const sampler_context context;
        const std::array<const volume *, 4> sequence{{
            &first,
            &second,
            &first,
            &second,
        }};
        for (const auto *vol : sequence) {
            buffer cached_values{vol->size()};
            buffer plain_values{vol->size()};
            cached->sample_volume(context, cached_values, *vol);
            plain->sample_volume(context, plain_values, *vol);
            for (std::size_t i = 0; i < cached_values.size(); ++i) {
                expect(bits(cached_values.get(i)) == bits(plain_values.get(i)))
                    << "cached value differs at " << i;
            }
        }
    };

    "a cache node leaves the scalar path untouched"_test = [] {
        auto cached = cache(make_synthetic_input());
        auto plain = make_synthetic_input();
        const sampler_context context;
        for (const auto &point : std::array<std::array<jint, 3>, 3>{
                 {{{0, -64, 0}}, {{5, 0, -3}}, {{-7, 63, 11}}}}) {
            expect(bits(cached->sample_value(context, point[0], point[1],
                                             point[2])) ==
                   bits(plain->sample_value(context, point[0], point[1],
                                            point[2])));
        }
    };

    "a shared subgraph samples the values of a duplicated one"_test = [] {
        // A referenced document is compiled once and referenced from several
        // places. Sampling through the shared pointer must equal sampling two
        // independent copies of the same expression.
        auto shared = make_synthetic_input();
        auto shared_graph = add(shared, shared);
        auto duplicated_graph =
            add(make_synthetic_input(), make_synthetic_input());
        const volume vol{9, 13, 7, -2, -40, 1};
        const sampler_context context;
        buffer shared_values{vol.size()};
        buffer duplicated_values{vol.size()};
        shared_graph->sample_volume(context, shared_values, vol);
        duplicated_graph->sample_volume(context, duplicated_values, vol);
        for (std::size_t i = 0; i < shared_values.size(); ++i) {
            expect(bits(shared_values.get(i)) == bits(duplicated_values.get(i)))
                << "shared value differs at " << i;
        }
    };

    "the sampled cell buffer is reused across chunks"_test = [] {
        // The same context across chunks: the cache entry changes size with
        // the grid but keeps producing the uncached values.
        auto cached = cache(make_synthetic_input());
        auto plain = make_synthetic_input();
        const sampler_context context;
        const std::array<volume, 3> volumes{{
            volume{16, 40, 16, 0, -64, 0},
            volume{16, 40, 16, 16, -64, 0},
            volume{16, 24, 16, 32, 0, 0},
        }};
        for (const auto &vol : volumes) {
            buffer cached_values{vol.size()};
            buffer plain_values{vol.size()};
            cached->sample_volume(context, cached_values, vol);
            plain->sample_volume(context, plain_values, vol);
            for (std::size_t i = 0; i < cached_values.size(); ++i) {
                expect(bits(cached_values.get(i)) == bits(plain_values.get(i)))
                    << "cached value differs at " << i;
            }
        }
    };
};
