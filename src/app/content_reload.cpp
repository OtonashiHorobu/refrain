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

} // namespace

content_reload::content_reload(content_view_snapshot initial,
                               content_reload_options options, builder build)
    : live_{std::move(initial)}, options_{std::move(options)},
      build_{std::move(build)} {}

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
        if (live_.view) {
            const auto change{
                data::describe_registry_change(*live_.view, **built)};
            // TODO: immutable registry generations are not implemented. A
            // running server freezes the block/item/entity registries when it
            // starts and every existing chunk keeps the palette generation it
            // was written with, so a registry change is refused here instead of
            // being half-applied. When generations land, this refusal becomes
            // the switch point: existing chunks stay on the old generation,
            // newly generated chunks bind to the new one and only a remap that
            // is explicitly supported may rewrite old palettes.
            if (!change.empty()) {
                result.failed = true;
                result.restart_required = true;
                result.error =
                    fmt::format("block, item or entity registry data changed: "
                                "{}",
                                change);
                result.lines = {
                    fmt::format("refused the content reload because {}",
                                result.error),
                    "a running server cannot change a frozen registry and "
                    "existing chunks keep the generation they were written "
                    "with, so this change needs a restart",
                    fmt::format("the running view stays at generation {}",
                                live_.generation),
                };
            }
        }
        if (!result.failed) {
            live_ = content_view_snapshot{
                .generation = live_.generation + 1U,
                .view = std::move(*built),
            };
            result.published = true;
            result.lines.push_back(
                fmt::format("published content view generation {} with {}",
                            live_.generation,
                            data::format_content_view_summary(*live_.view)));
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
