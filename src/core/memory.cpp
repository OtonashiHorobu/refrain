// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/memory.hpp>

#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#if REFRAIN_WITH_MIMALLOC
#include <mimalloc.h>
#endif

namespace refrain::core::memory {

namespace {

#if REFRAIN_WITH_MIMALLOC

/// std::pmr adapter over the mimalloc C API (the vendor library owns the
/// allocator; this class only translates the standard interface).
class mimalloc_resource final : public std::pmr::memory_resource {
  public:
    [[nodiscard]] auto name() const noexcept -> std::string_view {
        return "mimalloc";
    }

  private:
    [[nodiscard]] auto do_allocate(std::size_t bytes, std::size_t alignment)
        -> void * override {
        void *pointer{alignment <= alignof(std::max_align_t)
                          ? mi_malloc(bytes)
                          : mi_malloc_aligned(bytes, alignment)};
        if (pointer == nullptr) {
            throw std::bad_alloc{};
        }
        return pointer;
    }

    void do_deallocate(void *pointer, std::size_t, std::size_t) override {
        mi_free(pointer);
    }

    [[nodiscard]] auto
    do_is_equal(const std::pmr::memory_resource &other) const noexcept
        -> bool override {
        return this == &other;
    }
};

/// Process-lifetime adapter instance.
///
/// The object is intentionally leaked: `default_resource()` must stay valid
/// while other translation units' static destructors run, so it cannot be a
/// plain function-local static (which would be destroyed during static
/// destruction and dangle for later users). This matches the standard
/// `std::pmr::new_delete_resource()`, which is immortal by contract. The
/// adapter itself is stateless; only the process-global mimalloc runtime is
/// involved.
///
/// `noexcept` boundary: if the one-time `new` fails, fall back to the standard
/// new/delete resource instead of terminating; `default_resource()` promises a
/// non-null result.
[[nodiscard]] auto mimalloc_runtime() noexcept -> std::pmr::memory_resource * {
    static auto *resource{[]() noexcept -> std::pmr::memory_resource * {
        try {
            return new mimalloc_resource{};
        } catch (...) {
            return std::pmr::new_delete_resource();
        }
    }()};
    return resource;
}

#endif // REFRAIN_WITH_MIMALLOC

} // namespace

auto to_string(backend which) noexcept -> std::string_view {
    switch (which) {
    case backend::system:
        return "system";
    case backend::mimalloc:
        return "mimalloc";
    }
    return "unknown";
}

auto compiled_backend() noexcept -> backend {
#if REFRAIN_WITH_MIMALLOC
    return backend::mimalloc;
#else
    return backend::system;
#endif
}

auto default_resource() noexcept -> std::pmr::memory_resource * {
#if REFRAIN_WITH_MIMALLOC
    return mimalloc_runtime();
#else
    return std::pmr::new_delete_resource();
#endif
}

auto resource_for(backend which) noexcept -> std::pmr::memory_resource * {
    switch (which) {
    case backend::system:
        return std::pmr::new_delete_resource();
    case backend::mimalloc:
#if REFRAIN_WITH_MIMALLOC
        return mimalloc_runtime();
#else
        return nullptr;
#endif
    }
    return nullptr;
}

} // namespace refrain::core::memory

namespace refrain::core {

namespace {

[[nodiscard]] constexpr auto is_power_of_two(std::size_t value) noexcept
    -> bool {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Alignment requested by callers that pass 0: the natural maximum alignment.
constexpr auto default_alignment{alignof(std::max_align_t)};

/// The std::pmr interface reports failures by throwing; arena's public API is
/// exception-free, so the upstream boundary translates between the two worlds.
[[nodiscard]] auto try_allocate(std::pmr::memory_resource &upstream,
                                std::size_t bytes,
                                std::size_t alignment) noexcept -> void * {
    try {
        return upstream.allocate(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void try_deallocate(std::pmr::memory_resource &upstream, void *pointer,
                    std::size_t bytes, std::size_t alignment) noexcept {
    try {
        upstream.deallocate(pointer, bytes, alignment);
    } catch (...) {
        // Deallocation must not escape a noexcept path.
    }
}

} // namespace

arena::arena(std::size_t block_size,
             std::pmr::memory_resource *upstream) noexcept
    : block_size_{block_size == 0 ? default_block_size : block_size},
      upstream_{upstream != nullptr ? upstream : memory::default_resource()} {}

arena::arena(arena &&other) noexcept
    : blocks_{std::move(other.blocks_)}, block_size_{other.block_size_},
      used_bytes_{other.used_bytes_}, upstream_{other.upstream_} {
    // Leave the moved-from arena empty and fully usable. The standard only
    // guarantees a moved-from vector is valid but unspecified (it is empty in
    // every implementation we support, but that is not guaranteed), so clear
    // explicitly instead of trusting the implementation's move behavior.
    other.blocks_.clear();
    other.used_bytes_ = 0;
}

auto arena::operator=(arena &&other) noexcept -> arena & {
    if (this != &other) {
        // Return this arena's own blocks to *its* upstream before adopting the
        // other arena's blocks and upstream.
        release_all();
        blocks_ = std::move(other.blocks_);
        block_size_ = other.block_size_;
        used_bytes_ = other.used_bytes_;
        upstream_ = other.upstream_;
        // Explicit: moved-from vectors are valid-but-unspecified, not
        // guaranteed empty.
        other.blocks_.clear();
        other.used_bytes_ = 0;
    }
    return *this;
}

arena::~arena() = default;

auto arena::allocate(std::size_t bytes, std::size_t alignment)
    -> result<std::span<std::byte>> {
    const auto requested_alignment{
        alignment == 0 ? static_cast<std::size_t>(default_alignment)
                       : alignment};
    if (!is_power_of_two(requested_alignment) ||
        requested_alignment > block_alignment) {
        return std::unexpected(make_error(
            error_code::invalid_argument,
            std::string_view{
                "arena::allocate: alignment must be a power of two up to 64"}));
    }
    if (bytes == 0) {
        return std::span<std::byte>{};
    }
    if (bytes > std::numeric_limits<std::size_t>::max() - requested_alignment) {
        return std::unexpected(
            make_error(error_code::out_of_memory,
                       std::string_view{"arena::allocate: size overflow"}));
    }

    const auto mask{requested_alignment - 1};
    if (!blocks_.empty()) {
        auto &current{blocks_.back()};
        const auto aligned{(current.used + mask) & ~mask};
        // `aligned >= current.used` rejects wrap-around; the subtraction below
        // is then safe because `aligned <= current.capacity` is checked first.
        if (aligned >= current.used && aligned <= current.capacity &&
            bytes <= current.capacity - aligned) {
            const auto previous_used{current.used};
            current.used = aligned + bytes;
            used_bytes_ += current.used - previous_used;
            return std::span<std::byte>{current.data + aligned, bytes};
        }
    }

    const auto required{bytes + requested_alignment};
    block *target{nullptr};
    if (required <= block_size_) {
        auto appended{allocate_block()};
        if (!appended) {
            return std::unexpected(std::move(appended.error()));
        }
        target = *appended;
    } else {
        // Oversized request: dedicate a block covering the request plus
        // alignment slack. The block pointer is consumed immediately after the
        // push, before any further vector growth could invalidate it.
        auto *raw{static_cast<std::byte *>(
            try_allocate(*upstream_, required, block_alignment))};
        if (raw == nullptr) {
            return std::unexpected(
                make_error(error_code::out_of_memory,
                           std::string_view{"arena: block allocation failed"}));
        }
        // The vector's bookkeeping allocation can throw std::bad_alloc; the
        // public API is exception-free, so translate it into out_of_memory.
        try {
            blocks_.emplace_back(raw, required, upstream_);
        } catch (...) {
            try_deallocate(*upstream_, raw, required, block_alignment);
            return std::unexpected(make_error(
                error_code::out_of_memory,
                std::string_view{"arena: block bookkeeping failed"}));
        }
        target = &blocks_.back();
    }

    // Fresh blocks are block_alignment-aligned (>= requested_alignment), so a
    // new block always starts at offset zero.
    target->used = bytes;
    used_bytes_ += bytes;
    return std::span<std::byte>{target->data, bytes};
}

auto arena::allocate_block() -> result<block *> {
    auto *raw{static_cast<std::byte *>(
        try_allocate(*upstream_, block_size_, block_alignment))};
    if (raw == nullptr) {
        return std::unexpected(
            make_error(error_code::out_of_memory,
                       std::string_view{"arena: block allocation failed"}));
    }
    try {
        blocks_.emplace_back(raw, block_size_, upstream_);
    } catch (...) {
        try_deallocate(*upstream_, raw, block_size_, block_alignment);
        return std::unexpected(
            make_error(error_code::out_of_memory,
                       std::string_view{"arena: block bookkeeping failed"}));
    }
    return &blocks_.back();
}

auto arena::get_mark() const noexcept -> mark {
    if (blocks_.empty()) {
        return mark{};
    }
    return mark{blocks_.size() - 1, blocks_.back().used};
}

void arena::release_to(mark m) noexcept {
    // A mark whose block no longer exists (the arena shrank below it) is
    // stale; treating the rewind as a no-op is the safe interpretation.
    if (m.block >= blocks_.size()) {
        return;
    }
    auto &target{blocks_[m.block]};
    target.used = m.used <= target.capacity ? m.used : target.capacity;
    if (blocks_.size() > m.block + 1) {
        blocks_.resize(m.block + 1);
    }
    std::size_t total{0};
    for (const auto &current : blocks_) {
        total += current.used;
    }
    used_bytes_ = total;
}

void arena::reset() noexcept {
    if (blocks_.empty()) {
        used_bytes_ = 0;
        return;
    }
    blocks_.resize(1);
    blocks_.front().used = 0;
    used_bytes_ = 0;
}

void arena::release_all() noexcept {
    blocks_.clear();
    used_bytes_ = 0;
}

// Bytes consumed across all blocks, including alignment padding.
auto arena::used_bytes() const noexcept -> std::size_t { return used_bytes_; }

auto arena::block_count() const noexcept -> std::size_t {
    return blocks_.size();
}

auto arena::block_size() const noexcept -> std::size_t { return block_size_; }

auto arena::upstream() const noexcept -> std::pmr::memory_resource * {
    return upstream_;
}

auto arena::do_allocate(std::size_t bytes, std::size_t alignment) -> void * {
    if (bytes == 0) {
        // std::pmr expects a distinct, freeable pointer for zero-byte requests.
        bytes = 1;
    }
    if (alignment > block_alignment) {
        // Over-aligned requests bypass the arena: forward to the upstream
        // resource (throwing std::bad_alloc on failure, per the std::pmr
        // contract) and hand the memory back in do_deallocate.
        return upstream_->allocate(bytes, alignment);
    }
    const auto storage{allocate(bytes, alignment)};
    if (!storage) {
        throw std::bad_alloc{};
    }
    return storage->data();
}

void arena::do_deallocate(void *pointer, std::size_t bytes,
                          std::size_t alignment) {
    if (pointer == nullptr) {
        return;
    }
    // Arena blocks are reclaimed by reset()/release_to(); only over-aligned
    // requests were served by the upstream resource.
    if (alignment > block_alignment) {
        try_deallocate(*upstream_, pointer, bytes, alignment);
    }
}

auto arena::do_is_equal(const std::pmr::memory_resource &other) const noexcept
    -> bool {
    return this == &other;
}

} // namespace refrain::core
