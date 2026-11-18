// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/content_reload.hpp>

#include <refrain/core/log.hpp>

#include <fmt/format.h>

#include <chrono>
#include <exception>
#include <utility>

namespace refrain::app {

namespace {

constexpr refrain::log::logger log_content{"content"};

[[nodiscard]] auto gate_source(const content_reload_options &options)
    -> std::string {
    return options.gate_source.empty()
               ? std::string{"the mod administration configuration"}
               : options.gate_source;
}

[[nodiscard]] auto failure_lines(std::string_view detail,
                                 std::uint64_t generation)
    -> std::vector<std::string> {
    return {
        fmt::format("failed to rebuild the content view: {}", detail),
        fmt::format("the running view stays at generation {}", generation),
    };
}

/// Fill in the registry generation a caller left out. A snapshot that names a
/// view but no mapping means "the view's own mapping"; a boot view is registry
/// generation 1 so a later document change makes it 2.
[[nodiscard]] auto normalize_snapshot(content_view_snapshot snapshot)
    -> content_view_snapshot {
    if (!snapshot.view) {
        snapshot.registries.reset();
        snapshot.registry_generation = 0;
        return snapshot;
    }
    if (!snapshot.registries) {
        snapshot.registries = std::make_shared<const data::registry_generation>(
            snapshot.view->registries());
    }
    if (snapshot.registry_generation == 0) {
        snapshot.registry_generation = 1;
    }
    return snapshot;
}

} // namespace

content_reload::content_reload(content_view_snapshot initial,
                               content_reload_options options, builder build)
    : live_{normalize_snapshot(std::move(initial))},
      options_{std::move(options)}, build_{std::move(build)} {}

content_reload::~content_reload() {
    std::future<result<std::shared_ptr<const data::content_view>>> pending{};
    {
        const std::lock_guard lock{mutex_};
        pending = std::move(build_future_);
    }
    // A build still running keeps the process alive until it finishes; the
    // candidate view it produces is dropped. Shutdown waits instead of
    // abandoning a thread that reads the content of a pack the process may be
    // about to remove.
    if (pending.valid()) {
        pending.wait();
    }
}

auto content_reload::request() -> result<std::vector<std::string>> {
    const std::lock_guard lock{mutex_};
    if (!options_.enabled) {
        return std::unexpected{make_error(
            error_code::permission_denied,
            fmt::format("content reload is disabled ([lifecycle] "
                        "hot_reload_data = false in '{}'); enable the gate and "
                        "restart the server to reload content at runtime",
                        gate_source(options_)))};
    }
    if (build_future_.valid()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "a content view rebuild is already in flight; the new view "
            "publishes at the next tick boundary")};
    }
    if (!build_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "no content view builder is configured for this server")};
    }
    build_future_ = std::async(
        std::launch::async,
        [build =
             build_]() -> result<std::shared_ptr<const data::content_view>> {
            try {
                auto built{build()};
                if (!built) {
                    return std::unexpected{std::move(built.error())};
                }
                if (!*built) {
                    return std::unexpected{make_error(
                        error_code::unknown,
                        "the content view builder returned no view")};
                }
                return built;
            } catch (const std::exception &failure) {
                return std::unexpected{
                    make_error(error_code::unknown,
                               fmt::format("the content view builder threw: {}",
                                           failure.what()))};
            } catch (...) {
                return std::unexpected{make_error(
                    error_code::unknown,
                    "the content view builder threw an unknown exception")};
            }
        });
    std::vector<std::string> lines{};
    if (live_.view) {
        lines.push_back(
            fmt::format("generation {} is live with {}", live_.generation,
                        data::format_content_view_summary(*live_.view)));
    }
    lines.push_back(fmt::format(
        "queued a content view rebuild; generation {} stays live until the "
        "next tick boundary",
        live_.generation));
    lines.push_back("the new view is loaded and validated off the tick before "
                    "it is published");
    return lines;
}

auto content_reload::commit() -> content_reload_commit_result {
    std::future<result<std::shared_ptr<const data::content_view>>> finished{};
    std::uint64_t live_generation{0};
    {
        const std::lock_guard lock{mutex_};
        if (!build_future_.valid()) {
            return {};
        }
        const auto wait{std::chrono::seconds{0}};
        if (build_future_.wait_for(wait) != std::future_status::ready) {
            content_reload_commit_result pending{};
            pending.pending = true;
            return pending;
        }
        live_generation = live_.generation;
        finished = std::move(build_future_);
    }
    auto built{finished.get()};
    content_reload_commit_result result{};
    if (!built) {
        result.failed = true;
        result.error = built.error().message;
        result.lines = failure_lines(result.error, live_generation);
        log_content.error("{}", result.lines.front());
        log_content.error("{}", result.lines.back());
        return result;
    }
    std::vector<std::string> warnings{};
    {
        const std::lock_guard lock{mutex_};
        data::registry_change change{};
        if (live_.view) {
            change = data::classify_registry_change(*live_.view, **built);
        }
        const bool rebind_missing{change.hot_appliable() &&
                                  !options_.registry_generations};
        if (change.restart_required() || rebind_missing) {
            // A registry container changed while the document class may have
            // changed too. The reload is refused as one unit: applying only the
            // documents would leave the frozen containers describing a
            // different content set than the published generation.
            result.failed = true;
            result.restart_required = true;
            if (rebind_missing) {
                result.error = fmt::format("registry document data changed: {}",
                                           change.description());
                result.lines = {
                    fmt::format("refused the content reload because {}",
                                result.error),
                    options_.registry_generation_unsupported_reason.empty()
                        ? "the running chunk-generation path cannot bind a new "
                          "registry generation, so this change needs a restart"
                        : options_.registry_generation_unsupported_reason,
                    fmt::format("the running view stays at generation {}",
                                live_.generation),
                };
            } else {
                result.error =
                    fmt::format("registry container data changed: {}",
                                change.description());
                result.lines = {
                    fmt::format("refused the content reload because {}",
                                result.error),
                    "the running server froze the numeric identities of the "
                    "block, item and entity tables and of the wire registry "
                    "catalog at startup, so replacing 'tables.rvtb' or "
                    "'protocol_registry.json' needs a restart",
                    fmt::format("the running view stays at generation {}",
                                live_.generation),
                };
            }
        } else {
            auto registries{
                change.hot_appliable()
                    ? std::make_shared<const data::registry_generation>(
                          (*built)->registries())
                    : live_.registries};
            if (registries == nullptr) {
                // The live snapshot started without a mapping (an embedding
                // that published a bare view); the published one always names
                // the mapping of the view it describes.
                registries = std::make_shared<const data::registry_generation>(
                    (*built)->registries());
            }
            auto registry_generation{live_.registry_generation};
            if (change.hot_appliable()) {
                ++registry_generation;
            } else if (registry_generation == 0) {
                registry_generation = 1;
            }
            live_ = content_view_snapshot{
                .generation = live_.generation + 1U,
                .view = std::move(*built),
                .registry_generation = registry_generation,
                .registries = std::move(registries),
            };
            result.published = true;
            result.registry_generation_published = change.hot_appliable();
            result.lines.push_back(
                fmt::format("published content view generation {} with {}",
                            live_.generation,
                            data::format_content_view_summary(*live_.view)));
            if (result.registry_generation_published) {
                const auto &mapping{*live_.registries};
                result.lines.push_back(fmt::format(
                    "published registry generation {} with {} block, {} item "
                    "and {} entity entries; chunks generated before it keep "
                    "registry generation {} and the block states they wrote "
                    "stay as they are",
                    live_.registry_generation,
                    mapping.size(data::registry_kind::block),
                    mapping.size(data::registry_kind::item),
                    mapping.size(data::registry_kind::entity_type),
                    live_.registry_generation - 1U));
                if (!change.entry_names_changed()) {
                    result.lines.push_back(
                        "the registry entry names are unchanged, so chunk "
                        "generation output is unchanged; the new generation "
                        "records only the changed entry document digests");
                }
            }
            warnings.assign(live_.view->warnings().begin(),
                            live_.view->warnings().end());
            if (!warnings.empty()) {
                result.lines.push_back(fmt::format(
                    "the new content view has {} validation warning(s)",
                    warnings.size()));
            }
            if (!live_.view->skipped_sources().empty()) {
                result.lines.push_back(fmt::format(
                    "{} content layer(s) are not published yet and were not "
                    "mounted",
                    live_.view->skipped_sources().size()));
            }
        }
    }
    if (result.published) {
        log_content.info("{}", result.lines.front());
        for (std::size_t index{1}; index < result.lines.size(); ++index) {
            log_content.warn("{}", result.lines[index]);
        }
        for (const auto &warning : warnings) {
            log_content.warn("{}", warning);
        }
    } else {
        for (const auto &line : result.lines) {
            log_content.error("{}", line);
        }
    }
    return result;
}

auto content_reload::snapshot() const -> content_view_snapshot {
    const std::lock_guard lock{mutex_};
    return live_;
}

auto content_reload::pending() const -> bool {
    const std::lock_guard lock{mutex_};
    return build_future_.valid();
}

auto content_reload::generation() const -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return live_.generation;
}

auto content_reload::registry_generation() const -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return live_.registry_generation;
}

auto make_content_reload_port(content_reload &service) -> content_reload_port {
    content_reload_port port{};
    port.request = [&service]() -> result<content_reload_outcome> {
        auto lines{service.request()};
        if (!lines) {
            return std::unexpected{std::move(lines.error())};
        }
        content_reload_outcome outcome{};
        outcome.lines = std::move(*lines);
        return outcome;
    };
    return port;
}

} // namespace refrain::app
