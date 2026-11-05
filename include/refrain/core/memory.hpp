// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace refrain::core::memory {

/// General-purpose allocation backend compiled into the kernel.
///
/// The backend is selected at configure time (`REFRAIN_WITH_MIMALLOC`) and
/// owns the actual allocation logic; the kernel does not hand-roll a general
/// allocator. Every backend implements the standard
/// `std::pmr::memory_resource` interface, so engine code, `std::pmr`
/// containers and third-party libraries can share it.
enum class backend : std::uint8_t {
    /// Global operator new/delete (`std::pmr::new_delete_resource`). When the
    /// process-wide new/delete override is enabled this path uses mimalloc too.
    system,
    /// Microsoft mimalloc (https://github.com/microsoft/mimalloc).
    mimalloc,
};

/// Human-readable backend name for diagnostics ("system", "mimalloc").
[[nodiscard]] auto to_string(backend which) noexcept -> std::string_view;

/// The backend that `default_resource()` is wired to in this build.
[[nodiscard]] auto compiled_backend() noexcept -> backend;

/// Process-wide upstream resource for engine allocations.
///
/// Stable for the lifetime of the process and never null. A mimalloc build
/// returns the mimalloc-backed resource; a system build returns the standard
/// new/delete resource. The resource object is intentionally *immortal*: it is
/// never destroyed during static destruction, so global/static destructors may
/// still allocate and deallocate through it (the standard new/delete resource
/// has the same guarantee, keeping the two backends symmetric). Prefer this
/// over calling `operator new` directly.
[[nodiscard]] auto default_resource() noexcept -> std::pmr::memory_resource *;

/// Resource for a specific backend, or nullptr when that backend is not
/// compiled in (only `backend::mimalloc` can be absent).
[[nodiscard]] auto resource_for(backend which) noexcept
    -> std::pmr::memory_resource *;

} // namespace refrain::core::memory

namespace refrain::core {

/// Monotonic bump allocator backed by 1 MiB (configurable) blocks.
///
/// Used for per-tick scratch memory, worldgen temporaries and codec buffers.
/// Objects created through `create<T>` must be trivially destructible: the
/// arena never runs destructors (reset()/release_to() only rewind the cursor).
///
/// Blocks come from an upstream `std::pmr::memory_resource`
/// (default: `memory::default_resource()`, i.e. mimalloc in release builds), so
/// the arena is a thin policy layer on top of a mature allocator backend. The
/// arena is itself a `std::pmr::memory_resource`: `std::pmr::vector` and
/// friends can use per-tick scratch memory directly.
class arena : public std::pmr::memory_resource {
  public:
    static constexpr std::size_t default_block_size = 1U << 20U;
    static constexpr std::size_t block_alignment = 64;

    struct mark {
        std::size_t block{0};
        std::size_t used{0};
    };

    explicit arena(std::size_t block_size = default_block_size,
                   std::pmr::memory_resource *upstream =
                       memory::default_resource()) noexcept;

    arena(const arena &) = delete;
    arena &operator=(const arena &) = delete;

    arena(arena &&other) noexcept;
    arena &operator=(arena &&other) noexcept;

    ~arena() override;

    /// Allocate `bytes` with the given alignment (0 = natural maximum alignment
    /// of std::max_align_t; power-of-two values up to block_alignment
    /// supported).
    ///
    /// This public, exception-free API serves every request from arena blocks
    /// and therefore rejects `alignment > block_alignment` with
    /// `invalid_argument`. Over-aligned requests are only supported through
    /// the `std::pmr::memory_resource` interface
    /// (`std::pmr::vector<over_aligned>` etc.): `do_allocate()` forwards them
    /// to the upstream resource and `do_deallocate()` returns them there, so
    /// they are *not* reclaimed by `reset()`/`release_to()`.
    [[nodiscard]] auto allocate(std::size_t bytes, std::size_t alignment = 0)
        -> result<std::span<std::byte>>;

    /// Construct a trivially destructible object inside the arena.
    ///
    /// The constructor must be `nothrow`: the kernel is exception-free and an
    /// exception escaping this `result<>` function would bypass the error
    /// contract. The requirement is enforced at compile time.
    template <typename T, typename... Args>
    [[nodiscard]] auto create(Args &&...args) -> result<T *> {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena::create<T> requires a trivially destructible T");
        static_assert(
            std::is_nothrow_constructible_v<T, Args...>,
            "arena::create<T> requires a nothrow constructor (kernel code is "
            "exception-free; an escaping exception would break the result<> "
            "error contract)");
        auto storage = allocate(sizeof(T), alignof(T));
        if (!storage) {
            return std::unexpected(std::move(storage.error()));
        }
        auto *value = ::new (static_cast<void *>(storage->data()))
            T(std::forward<Args>(args)...);
        return value;
    }

    /// Rewind to a previously taken mark (mark/rollback for scratch arenas).
    [[nodiscard]] auto get_mark() const noexcept -> mark;

    void release_to(mark m) noexcept;

    /// Drop everything except the first (retained) block.
    void reset() noexcept;

    /// Free all blocks.
    void release_all() noexcept;

    [[nodiscard]] auto used_bytes() const noexcept -> std::size_t;

    [[nodiscard]] auto block_count() const noexcept -> std::size_t;

    [[nodiscard]] auto block_size() const noexcept -> std::size_t;

    /// Upstream resource that backs every block of this arena.
    [[nodiscard]] auto upstream() const noexcept -> std::pmr::memory_resource *;

  protected:
    // std::pmr::memory_resource interface. Requests up to block_alignment are
    // served from arena blocks (and are reclaimed by reset/release_to);
    // over-aligned requests are forwarded to the upstream resource so that
    // `std::pmr` containers of over-aligned types still work. This is
    // deliberately different from the public `allocate()`, which enforces the
    // arena-only ceiling; the two-API split is documented there.
    [[nodiscard]] auto do_allocate(std::size_t bytes, std::size_t alignment)
        -> void * override;

    void do_deallocate(void *pointer, std::size_t bytes,
                       std::size_t alignment) override;

    [[nodiscard]] auto
    do_is_equal(const std::pmr::memory_resource &other) const noexcept
        -> bool override;

  private:
    struct block {
        std::byte *data{};
        std::size_t capacity{};
        std::size_t used{};
        std::pmr::memory_resource *upstream{};

        block() = default;
        block(std::byte *ptr, std::size_t cap,
              std::pmr::memory_resource *owner) noexcept
            : data{ptr}, capacity{cap}, upstream{owner} {}

        block(const block &) = delete;
        block &operator=(const block &) = delete;

        block(block &&other) noexcept
            : data{other.data}, capacity{other.capacity}, used{other.used},
              upstream{other.upstream} {
            other.data = nullptr;
            other.capacity = 0;
            other.used = 0;
            other.upstream = nullptr;
        }

        auto operator=(block &&other) noexcept -> block & {
            if (this != &other) {
                destroy();
                data = other.data;
                capacity = other.capacity;
                used = other.used;
                upstream = other.upstream;
                other.data = nullptr;
                other.capacity = 0;
                other.used = 0;
                other.upstream = nullptr;
            }
            return *this;
        }

        ~block() { destroy(); }

        void destroy() noexcept {
            if (data != nullptr) {
                if (upstream != nullptr) {
                    try {
                        upstream->deallocate(data, capacity, block_alignment);
                    } catch (...) {
                        // Deallocation must never escape a noexcept path.
                    }
                }
                data = nullptr;
                capacity = 0;
                used = 0;
                upstream = nullptr;
            }
        }
    };

    [[nodiscard]] auto allocate_block() -> result<block *>;

    std::vector<block> blocks_{};
    std::size_t block_size_{default_block_size};
    std::size_t used_bytes_{0};
    std::pmr::memory_resource *upstream_{memory::default_resource()};
};

/// RAII helper: release_to(get_mark()) on scope exit.
class scoped_arena_mark {
  public:
    explicit scoped_arena_mark(arena &target) noexcept
        : arena_{&target}, mark_{target.get_mark()} {}

    scoped_arena_mark(const scoped_arena_mark &) = delete;
    scoped_arena_mark &operator=(const scoped_arena_mark &) = delete;

    ~scoped_arena_mark() { arena_->release_to(mark_); }

    [[nodiscard]] auto get_mark() const noexcept -> arena::mark {
        return mark_;
    }

  private:
    arena *arena_;
    arena::mark mark_;
};

} // namespace refrain::core
