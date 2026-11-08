// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/argument.hpp>
#include <refrain/command/context.hpp>
#include <refrain/command/source.hpp>
#include <refrain/command/suggestions.hpp>
#include <refrain/command/tree.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace refrain::command {

/// Upper bound for one command line, in bytes. Longer input is rejected before
/// parsing rather than scanned.
inline constexpr std::size_t max_command_bytes = 32'767;

/// Upper bound for parsed arguments in one command.
inline constexpr std::size_t max_arguments = 128;

/// Upper bound for redirect hops while resolving one node.
inline constexpr std::size_t max_redirect_depth = 16;

/// Result of a successfully parsed and executed command.
///
/// `success` reflects the executor: it is false when the executor called
/// `context::fail()`. `value` is the executor's return value (vanilla uses it
/// as the command result) and is preserved even for a failed execution. The
/// consumed range covers the whole input, including surrounding whitespace that
/// the parser skipped.
struct execution {
    bool success{true};
    int value{0};
    std::size_t consumed_start{0};
    std::size_t consumed_end{0};
    /// Non-empty when the executor marked the command as failed.
    std::string failure{};
    /// Feedback lines emitted through `context::reply()` (bounded).
    std::vector<std::string> feedback{};

    [[nodiscard]] auto consumed_size() const noexcept -> std::size_t {
        return consumed_end - consumed_start;
    }
};

class dispatcher;
class node_ref;

/// Lightweight handle to one node of a `dispatcher`.
///
/// Handles hold an index, not a pointer into the node arena, so registering
/// more commands never invalidates them. A handle only copies the index and is
/// safe to store; using it after its dispatcher is destroyed is a caller bug.
///
/// Registration example:
/// ```cpp
/// command::dispatcher d;
/// d.literal("gamemode")
///     .require(command::level::gamemode)
///     .then(d.argument("mode", command::gamemode_arg())
///               .executes([](command::context &c) -> result<int> {
///                   c.reply(c.string("mode"));
///                   return 1;
///               }));
/// ```
/// `dispatcher::literal()` and `dispatcher::argument()` attach new nodes to the
/// root; `then()` moves the child under this node.
class node_ref {
  public:
    node_ref() noexcept = default;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto index() const noexcept -> std::size_t;
    [[nodiscard]] auto name() const noexcept -> std::string_view;

    /// Require a permission level. The predicate is evaluated for tree
    /// snapshots, suggestions and dispatch. (`requires` is a C++ keyword, so
    /// the registration verb is `require`.)
    auto require(permission_level required) -> node_ref;
    auto require(std::function<bool(const source &)> predicate) -> node_ref;

    /// Tooltip attached to this literal's suggestion entry.
    auto tooltip(std::string_view text) -> node_ref;

    /// Attach `child` below this node, detaching it from its previous parent
    /// (including the implicit root attachment). Returns this handle.
    auto then(node_ref child) -> node_ref;

    /// Continue parsing at `target` without consuming the target's name. The
    /// target's children and executor are used; this node's executor is not.
    auto redirect(node_ref target) -> node_ref;

    /// Attach an executor. The callable may return `void`, `int`, `status` or
    /// `result<int>`; `void` and successful `status` map to result 0. Executors
    /// must be copyable and must not throw.
    template <typename Fn>
        requires std::invocable<Fn &, context &>
    auto executes(Fn &&fn) -> node_ref;

  private:
    friend class dispatcher;
    node_ref(dispatcher *owner, std::size_t index) noexcept;

    dispatcher *owner_{nullptr};
    std::size_t index_{0};
};

/// Command graph plus its immutable snapshots.
///
/// The dispatcher owns a flat node arena. Registration mutates the arena and
/// bumps the revision counter; `execute`, `suggest` and `tree` are const and
/// may run concurrently with each other. Registration itself is single-writer:
/// build the tree first, then share the dispatcher.
class dispatcher {
  public:
    dispatcher();
    ~dispatcher();
    dispatcher(const dispatcher &) = delete;
    dispatcher &operator=(const dispatcher &) = delete;
    dispatcher(dispatcher &&) = delete;
    dispatcher &operator=(dispatcher &&) = delete;

    [[nodiscard]] auto root() noexcept -> node_ref;
    [[nodiscard]] auto literal(std::string_view name) -> node_ref;

    /// Register an argument node. Concrete parsers are stored by value in the
    /// arena; a `unique_ptr<const argument_parser>` overload accepts shared or
    /// factory-built parsers.
    template <typename Parser>
        requires std::derived_from<std::remove_cvref_t<Parser>, argument_parser>
    [[nodiscard]] auto argument(std::string_view name, Parser &&parser)
        -> node_ref {
        using parser_type = std::remove_cvref_t<Parser>;
        return add_argument(
            name, std::make_unique<parser_type>(std::forward<Parser>(parser)));
    }
    [[nodiscard]] auto argument(std::string_view name,
                                std::unique_ptr<const argument_parser> parser)
        -> node_ref;

    /// Monotonic registration revision. It starts at 1 and changes on every
    /// mutation (node creation, link, executor, requirement, tooltip,
    /// redirect).
    [[nodiscard]] auto revision() const noexcept -> std::uint64_t;
    [[nodiscard]] auto node_count() const noexcept -> std::size_t;

    /// Parse and execute `input`. The input is a command line without the
    /// leading slash, exactly as it arrives in the serverbound command packet.
    ///
    /// Syntax failures return `error_code::parse_error` with an
    /// `"<reason> at position <byte offset>"` message. Executor failures are
    /// propagated unchanged. A parsed command that the executor marks as failed
    /// returns an `execution` with `success == false`.
    [[nodiscard]] auto execute(std::string_view input, source &src) const
        -> result<execution>;

    /// Complete the token at `cursor`. The returned range is
    /// `[token_start, cursor)` in bytes. Cursors outside the input are clamped;
    /// oversized input yields empty suggestions. Entries are filtered by the
    /// source requirements and bounded by `max_suggestions`.
    [[nodiscard]] auto suggest(std::string_view input, std::size_t cursor,
                               const source &src) const -> suggestions;

    /// Build an immutable flat tree filtered by `src`. The snapshot owns all of
    /// its storage and stays valid while the dispatcher is mutated.
    [[nodiscard]] auto tree(const source &src) const
        -> std::shared_ptr<const refrain::command::tree>;

  private:
    friend class node_ref;

    static constexpr std::size_t no_node =
        std::numeric_limits<std::size_t>::max();

    struct node_entry {
        node_kind kind{node_kind::root};
        std::string name{};
        std::unique_ptr<const argument_parser> parser{};
        std::function<bool(const source &)> requirement{};
        std::function<result<int>(context &)> executor{};
        std::vector<std::size_t> children{};
        std::size_t parent{no_node};
        std::size_t redirect{no_node};
        std::string tooltip{};
    };

    struct parsed_argument {
        std::size_t node{0};
        argument_value value{};
        std::size_t start{0};
        std::size_t end{0};
    };

    struct parsed_command {
        std::size_t node{0};
        std::vector<parsed_argument> arguments{};
    };

    struct parse_failure {
        std::size_t cursor{0};
        std::string message{};
    };

    [[nodiscard]] auto add_node(node_kind kind, std::string_view name)
        -> std::size_t;
    [[nodiscard]] auto
    add_argument(std::string_view name,
                 std::unique_ptr<const argument_parser> parser) -> node_ref;
    void attach(std::size_t parent, std::size_t child);
    void detach(std::size_t child);
    void set_requirement(std::size_t index,
                         std::function<bool(const source &)> predicate);
    void set_executor(std::size_t index,
                      std::function<result<int>(context &)> fn);
    void set_tooltip(std::size_t index, std::string_view text);
    void set_redirect(std::size_t index, std::size_t target);
    void bump_revision() noexcept;
    [[nodiscard]] auto is_allowed(std::size_t index, const source &src) const
        -> bool;
    [[nodiscard]] auto parse_command(std::string_view input, const source &src,
                                     bool allow_incomplete) const
        -> std::expected<parsed_command, parse_failure>;

    std::vector<node_entry> nodes_{};
    std::atomic<std::uint64_t> revision_{1};
};

namespace detail {

template <typename> inline constexpr bool always_false = false;

} // namespace detail

template <typename Fn>
    requires std::invocable<Fn &, context &>
auto node_ref::executes(Fn &&fn) -> node_ref {
    static_assert(std::copy_constructible<std::remove_cvref_t<Fn>>,
                  "command executors must be copyable");
    auto callable =
        [function = std::forward<Fn>(fn)](context &ctx) mutable -> result<int> {
        using return_type = std::invoke_result_t<Fn &, context &>;
        if constexpr (std::is_void_v<return_type>) {
            function(ctx);
            return 0;
        } else if constexpr (std::same_as<std::remove_cvref_t<return_type>,
                                          status>) {
            auto outcome = function(ctx);
            if (!outcome) {
                return std::unexpected{std::move(outcome.error())};
            }
            return 0;
        } else if constexpr (std::same_as<std::remove_cvref_t<return_type>,
                                          result<int>>) {
            return function(ctx);
        } else if constexpr (std::is_convertible_v<return_type, int>) {
            return static_cast<int>(function(ctx));
        } else {
            static_assert(detail::always_false<Fn>,
                          "a command executor must return void, int, status or "
                          "result<int>");
        }
    };
    if (valid()) {
        owner_->set_executor(
            index_, std::function<result<int>(context &)>{std::move(callable)});
    }
    return *this;
}

} // namespace refrain::command
