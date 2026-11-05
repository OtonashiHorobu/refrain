// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/memory.hpp>

#include <boost/ut.hpp>

#if REFRAIN_WITH_MIMALLOC
#include <mimalloc.h>
#endif

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

namespace {

constexpr auto max_alignment{
    static_cast<std::size_t>(alignof(std::max_align_t))};

[[nodiscard]] auto is_aligned(const std::byte *pointer, std::size_t alignment)
    -> bool {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

[[nodiscard]] auto address_of(const std::byte *pointer) -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

struct alignas(32) payload {
    int value{};
};

/// std::pmr resource that records how the arena talks to its upstream.
class counting_resource final : public std::pmr::memory_resource {
  public:
    [[nodiscard]] auto allocations() const noexcept -> std::size_t {
        return allocations_;
    }

    [[nodiscard]] auto deallocations() const noexcept -> std::size_t {
        return deallocations_;
    }

  private:
    [[nodiscard]] auto do_allocate(std::size_t bytes, std::size_t alignment)
        -> void * override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *pointer, std::size_t bytes,
                       std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] auto
    do_is_equal(const std::pmr::memory_resource &other) const noexcept
        -> bool override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

} // namespace

namespace {

using namespace boost::ut;

// A namespace-scope destructor must still be able to use the process-wide
// default resource. The mimalloc adapter is intentionally
// immortal (never destroyed during static destruction), so this allocation at
// process exit must not touch a destroyed resource object.
struct default_resource_destruction_probe {
    ~default_resource_destruction_probe() noexcept {
        try {
            auto *resource = refrain::core::memory::default_resource();
            void *pointer = resource->allocate(16, 8);
            resource->deallocate(pointer, 16, 8);
        } catch (...) {
            // The probe checks object lifetime, not allocation success.
        }
    }
};
[[maybe_unused]] default_resource_destruction_probe g_resource_probe{};

// Boost.UT 2.3.1 executes `_test` assignments immediately through the active
// runner; the supported multi-translation-unit form is one `suite` variable
// per test file, which main.cpp drives through cfg<override>.run().
const suite memory_tests = [] {
    "memory/allocate basic block and natural alignment"_test = [] {
        refrain::core::arena arena{};
        const auto storage{arena.allocate(128)};
        expect(storage.has_value());
        expect(storage->size() == 128U);
        expect(is_aligned(storage->data(), max_alignment));
        expect(arena.used_bytes() == 128U);
        expect(arena.block_count() == 1U);
        expect(arena.block_size() == refrain::core::arena::default_block_size);

        (*storage)[0] = std::byte{0x2a};
        expect(std::to_integer<unsigned int>((*storage)[0]) == 0x2aU);
    };

    "memory/alignment requests up to block_alignment"_test = [] {
        refrain::core::arena arena{};
        for (const std::size_t alignment :
             {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8},
              std::size_t{16}, std::size_t{32},
              refrain::core::arena::block_alignment}) {
            const auto storage{arena.allocate(1, alignment)};
            expect(storage.has_value());
            expect(storage->size() == 1U);
            expect(is_aligned(storage->data(), alignment));
        }
        expect(arena.block_count() == 1U);
    };

    "memory/zero byte allocation is empty and free"_test = [] {
        refrain::core::arena arena{};
        const auto storage{arena.allocate(0)};
        expect(storage.has_value());
        expect(storage->empty());
        expect(arena.used_bytes() == 0U);
        expect(arena.block_count() == 0U);
    };

    "memory/crosses blocks when the current block is full"_test = [] {
        refrain::core::arena arena{1024};
        expect(arena.block_size() == 1024U);

        const auto first{arena.allocate(768)};
        expect(first.has_value());
        expect(arena.block_count() == 1U);

        const auto second{arena.allocate(768)};
        expect(second.has_value());
        expect(arena.block_count() == 2U);
        expect(first->data() != second->data());

        // The two blocks are disjoint allocations.
        const auto first_end{address_of(first->data()) + first->size()};
        const auto second_end{address_of(second->data()) + second->size()};
        expect(first_end <= address_of(second->data()) ||
               second_end <= address_of(first->data()));

        (*first)[0] = std::byte{0x01};
        (*second)[0] = std::byte{0x02};
        expect(std::to_integer<unsigned int>((*first)[0]) == 0x01U);
        expect(std::to_integer<unsigned int>((*second)[0]) == 0x02U);
    };

    "memory/large allocation gets a dedicated block"_test = [] {
        refrain::core::arena arena{8192};
        constexpr std::size_t big_size{1U << 20U};
        const auto big{arena.allocate(big_size)};
        expect(big.has_value());
        expect(big->size() == big_size);
        expect(is_aligned(big->data(), max_alignment));
        expect(arena.block_count() == 1U);
        expect(arena.used_bytes() == big_size);
        expect(arena.block_size() == 8192U);

        const auto small{arena.allocate(16, 64)};
        expect(small.has_value());
        expect(is_aligned(small->data(), 64));
        expect(arena.used_bytes() == big_size + 16U);
    };

    "memory/mark and release_to rewind blocks"_test = [] {
        refrain::core::arena arena{512};
        // alignment 1 keeps the arithmetic exact; natural alignment is covered
        // by the dedicated alignment test.
        expect(arena.allocate(200, 1).has_value());
        expect(arena.allocate(200, 1).has_value());

        const auto mark{arena.get_mark()};
        expect(mark.block == 0U);
        expect(mark.used == 400U);

        expect(arena.allocate(200, 1).has_value());
        expect(arena.block_count() == 2U);
        const auto grown_used{arena.used_bytes()};

        arena.release_to(mark);
        expect(arena.block_count() == 1U);
        expect(arena.used_bytes() == 400U);
        expect(arena.get_mark().used == mark.used);
        expect(grown_used > arena.used_bytes());

        expect(arena.allocate(100, 1).has_value());
        expect(arena.block_count() == 1U);
        expect(arena.used_bytes() == 500U);
    };

    "memory/release_to honours nested marks"_test = [] {
        refrain::core::arena arena{4096};
        expect(arena.allocate(64).has_value());
        const auto outer{arena.get_mark()};

        expect(arena.allocate(128).has_value());
        const auto inner{arena.get_mark()};
        expect(inner.used > outer.used);

        expect(arena.allocate(256).has_value());
        arena.release_to(inner);
        expect(arena.used_bytes() == inner.used);

        arena.release_to(outer);
        expect(arena.used_bytes() == outer.used);
        expect(arena.block_count() == 1U);
    };

    "memory/release_to after reset rewinds to the retained block"_test = [] {
        refrain::core::arena arena{512};
        expect(arena.allocate(100).has_value());
        const auto mark{arena.get_mark()};
        expect(arena.allocate(100).has_value());
        arena.reset();
        arena.release_to(mark);
        expect(arena.block_count() == 1U);
        expect(arena.used_bytes() == 100U);
    };

    "memory/release_to ignores marks past the last block"_test = [] {
        refrain::core::arena arena{512};
        expect(arena.allocate(100).has_value());
        const refrain::core::arena::mark stale{5, 0};
        arena.release_to(stale);
        expect(arena.block_count() == 1U);
        expect(arena.used_bytes() == 100U);
    };

    "memory/reset keeps only the retained block"_test = [] {
        refrain::core::arena arena{256};
        expect(arena.allocate(200).has_value());
        expect(arena.allocate(200).has_value());
        expect(arena.allocate(200).has_value());
        expect(arena.block_count() == 3U);

        arena.reset();
        expect(arena.used_bytes() == 0U);
        expect(arena.block_count() == 1U);

        expect(arena.allocate(256).has_value());
        expect(arena.allocate(1).has_value());
        expect(arena.block_count() == 2U);
        expect(arena.used_bytes() == 257U);
    };

    "memory/release_all frees every block"_test = [] {
        refrain::core::arena arena{256};
        expect(arena.allocate(200).has_value());
        expect(arena.allocate(200).has_value());
        expect(arena.block_count() == 2U);

        arena.release_all();
        expect(arena.block_count() == 0U);
        expect(arena.used_bytes() == 0U);
        expect(arena.get_mark().block == 0U);
        expect(arena.get_mark().used == 0U);

        expect(arena.allocate(128).has_value());
        expect(arena.block_count() == 1U);
    };

    "memory/scoped_arena_mark rewinds on scope exit"_test = [] {
        refrain::core::arena arena{4096};
        expect(arena.allocate(64).has_value());
        const auto base{arena.used_bytes()};

        {
            const refrain::core::scoped_arena_mark guard{arena};
            expect(guard.get_mark().used == base);
            expect(arena.allocate(512).has_value());
            expect(arena.used_bytes() == base + 512U);
        }

        expect(arena.used_bytes() == base);
        expect(arena.block_count() == 1U);
    };

    "memory/oversized request reports out_of_memory"_test = [] {
        refrain::core::arena arena{};
        const auto too_big{
            arena.allocate(std::numeric_limits<std::size_t>::max())};
        expect(!too_big.has_value());
        expect(too_big.error().code == refrain::error_code::out_of_memory);
        expect(arena.block_count() == 0U);
        expect(arena.used_bytes() == 0U);
    };

    "memory/invalid alignment is rejected"_test = [] {
        refrain::core::arena arena{};
        const auto not_power_of_two{arena.allocate(8, 3)};
        expect(!not_power_of_two.has_value());
        expect(not_power_of_two.error().code ==
               refrain::error_code::invalid_argument);

        const auto above_block_alignment{arena.allocate(8, 128)};
        expect(!above_block_alignment.has_value());
        expect(above_block_alignment.error().code ==
               refrain::error_code::invalid_argument);
    };

    "memory/move construction transfers blocks"_test = [] {
        refrain::core::arena source{1024};
        const auto original{source.allocate(64, 16)};
        expect(original.has_value());
        (*original)[0] = std::byte{0x7f};

        refrain::core::arena moved{std::move(source)};
        expect(moved.block_count() == 1U);
        expect(moved.used_bytes() == 64U);
        expect(source.block_count() == 0U);
        expect(source.used_bytes() == 0U);
        expect(std::to_integer<unsigned int>((*original)[0]) == 0x7fU);

        // The moved-from arena is empty and immediately usable; the
        // implementation clears the moved-from block vector explicitly instead
        // of relying on the unspecified moved-from state of std::vector.
        expect(source.get_mark().block == 0U);
        expect(source.get_mark().used == 0U);
        expect(source.allocate(32).has_value());
        expect(source.block_count() == 1U);
        expect(source.used_bytes() == 32U);
        expect(moved.used_bytes() == 64U);
    };

    "memory/move assignment replaces the destination"_test = [] {
        refrain::core::arena source{1024};
        expect(source.allocate(64).has_value());
        const auto mark{source.get_mark()};

        refrain::core::arena destination{2048};
        expect(destination.allocate(128).has_value());
        expect(destination.used_bytes() == 128U);

        destination = std::move(source);
        expect(destination.block_size() == 1024U);
        expect(destination.block_count() == 1U);
        expect(destination.used_bytes() == mark.used);
        expect(source.block_count() == 0U);
        expect(source.used_bytes() == 0U);
    };

    "memory/create<T> constructs aligned trivially destructible objects"_test =
        [] {
            refrain::core::arena arena{};
            const auto created{arena.create<payload>(7)};
            expect(created.has_value());
            expect(reinterpret_cast<std::uintptr_t>(*created) %
                       alignof(payload) ==
                   0U);
            expect((*created)->value == 7);

            const auto number{arena.create<int>(42)};
            expect(number.has_value());
            expect(reinterpret_cast<std::uintptr_t>(*number) % alignof(int) ==
                   0U);
            expect(**number == 42);
        };

    "memory/create<T> enforces nothrow construction"_test = [] {
        // The static_assert inside create<T> turns a throwing constructor (or a
        // non-trivial destructor) into a compile error. Positive side: argument
        // construction through parentheses and over-aligned trivially
        // destructible types both satisfy the nothrow requirement.
        struct point {
            int x;
            int y;
        };
        refrain::core::arena arena{};
        const auto created{arena.create<point>(3, 4)};
        expect(created.has_value());
        if (created) {
            expect((*created)->x == 3);
            expect((*created)->y == 4);
        }

        struct alignas(64) cache_line {
            int value;
        };
        const auto aligned{arena.create<cache_line>(9)};
        expect(aligned.has_value());
        if (aligned) {
            expect(reinterpret_cast<std::uintptr_t>(*aligned) % 64U == 0U);
            expect((*aligned)->value == 9);
        }
    };

    "memory/default resource is a process-lifetime singleton"_test = [] {
        auto *first = refrain::core::memory::default_resource();
        expect(first != nullptr);
        // Stable identity across calls; usable through explicit (de)allocation.
        // The file-scope probe above exercises the same resource from a static
        // destructor, which is only sound because the adapter is never
        // destroyed.
        expect(first == refrain::core::memory::default_resource());
        void *pointer = first->allocate(32, 16);
        expect(pointer != nullptr);
        first->deallocate(pointer, 32, 16);
    };

    "memory/compiled backend is exposed through std::pmr resources"_test = [] {
        using refrain::core::memory::backend;
        const auto compiled{refrain::core::memory::compiled_backend()};
#if REFRAIN_WITH_MIMALLOC
        expect(compiled == backend::mimalloc);
        expect(refrain::core::memory::resource_for(backend::mimalloc) !=
               nullptr);
#else
        expect(compiled == backend::system);
        expect(refrain::core::memory::resource_for(backend::mimalloc) ==
               nullptr);
#endif
        expect(refrain::core::memory::to_string(compiled) ==
               (compiled == backend::mimalloc ? "mimalloc" : "system"));

        // The system backend always maps to the standard new/delete resource.
        expect(refrain::core::memory::resource_for(backend::system) ==
               std::pmr::new_delete_resource());
        expect(refrain::core::memory::default_resource() ==
               refrain::core::memory::resource_for(compiled));
    };

    "memory/default resource honours alignment requests"_test = [] {
        auto *resource = refrain::core::memory::default_resource();
        for (const std::size_t alignment :
             {std::size_t{1}, std::size_t{8}, std::size_t{16}, std::size_t{64},
              std::size_t{256}, std::size_t{4096}}) {
            void *pointer = resource->allocate(128, alignment);
            expect(pointer != nullptr);
            expect(
                is_aligned(static_cast<const std::byte *>(pointer), alignment));
            resource->deallocate(pointer, 128, alignment);
        }
    };

    "memory/arena forwards block storage to the injected upstream"_test = [] {
        counting_resource upstream{};
        {
            refrain::core::arena arena{1024, &upstream};
            expect(arena.upstream() == &upstream);
            expect(arena.allocate(64).has_value());
            expect(upstream.allocations() == 1U);

            // Oversized request: a dedicated block is requested upstream.
            expect(arena.allocate(4096).has_value());
            expect(upstream.allocations() == 2U);
            expect(arena.block_count() == 2U);
        }
        // Both blocks are returned to the upstream when the arena dies.
        expect(upstream.deallocations() == 2U);
    };

    "memory/arena is usable as a std::pmr resource"_test = [] {
        refrain::core::arena arena{4096};
        std::pmr::vector<int> values{&arena};
        for (int i = 0; i < 1024; ++i) {
            values.push_back(i);
        }
        expect(values.size() == 1024U);
        expect(values[1023] == 1023);
        expect(arena.used_bytes() > 0U);

        // std::pmr destroy only calls deallocate; the arena reclaims on reset.
        values.clear();
        expect(arena.used_bytes() > 0U);
        arena.reset();
        expect(arena.used_bytes() == 0U);
        expect(arena.block_count() == 1U);
    };

    "memory/arena forwards over-aligned std::pmr requests upstream"_test = [] {
        counting_resource upstream{};
        refrain::core::arena arena{1024, &upstream};
        std::pmr::memory_resource &resource = arena;

        // The two APIs intentionally differ. The std::pmr interface forwards
        // over-aligned requests upstream ...
        void *pointer = resource.allocate(32, 256);
        expect(pointer != nullptr);
        expect(is_aligned(static_cast<const std::byte *>(pointer), 256));
        expect(upstream.allocations() == 1U);
        expect(arena.block_count() == 0U);

        // ... while the public exception-free allocator rejects them instead of
        // silently forwarding (a forwarded block could not be reclaimed by
        // reset/release_to and the caller has no matching free call).
        const auto rejected{arena.allocate(32, 256)};
        expect(!rejected.has_value());
        expect(rejected.error().code == refrain::error_code::invalid_argument);
        expect(upstream.allocations() == 1U);
        expect(arena.block_count() == 0U);

        resource.deallocate(pointer, 32, 256);
        expect(upstream.deallocations() == 1U);
    };

    "memory/arena blocks come from the mimalloc backend"_test = [] {
#if REFRAIN_WITH_MIMALLOC
        refrain::core::arena arena{};
        const auto storage{arena.allocate(64)};
        expect(storage.has_value());
        expect(mi_is_in_heap_region(storage->data()));
#endif
    };

    "memory/global operator new is backed by mimalloc when enabled"_test = [] {
#if REFRAIN_WITH_MIMALLOC && REFRAIN_MIMALLOC_OVERRIDE_NEW
        const auto owned{std::make_unique<int>(7)};
        expect(*owned == 7);
        expect(mi_is_in_heap_region(owned.get()));

        int stack_value{0};
        expect(!mi_is_in_heap_region(&stack_value));
#endif
    };
};

} // namespace
