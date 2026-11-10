// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/log.hpp>
#include <refrain/net/transport.hpp>

#include <asio/bind_executor.hpp>
#include <asio/connect.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/strand.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {
namespace {

using tcp = asio::ip::tcp;

[[nodiscard]] auto transport_failure(std::string_view message) -> error {
    return make_error(error_code::io_error, message);
}

class transport_impl final : public transport {
  public:
    transport_impl(transport_handler handler, std::size_t io_threads);
    ~transport_impl() override;

    [[nodiscard]] auto listen(const endpoint &bind) -> status override;
    [[nodiscard]] auto connect(const endpoint &target)
        -> result<std::uint64_t> override;
    void send(std::uint64_t connection,
              std::span<const std::byte> bytes) override;
    void close(std::uint64_t connection) override;
    void stop() noexcept override;
    [[nodiscard]] auto connection_count() const -> std::size_t override;
    [[nodiscard]] auto local_port() const -> std::uint16_t override;
    [[nodiscard]] auto descriptor() const -> transport_descriptor override;

  private:
    struct session final : std::enable_shared_from_this<session> {
        session(transport_impl &owner_value, std::uint64_t id_value,
                tcp::socket socket_value)
            : owner{&owner_value}, id{id_value},
              socket{std::move(socket_value)},
              strand{asio::make_strand(owner->context_)} {}

        void start() {
            connected = true;
            reader();
            writer();
        }

        void reader();
        void writer();
        void send_copy(std::vector<std::byte> bytes);
        void finish(std::string reason);

        transport_impl *owner;
        std::uint64_t id;
        tcp::socket socket;
        asio::strand<asio::io_context::executor_type> strand;
        std::array<std::byte, 64 * 1024> read_buffer{};
        std::deque<std::vector<std::byte>> writes{};
        bool connected{false};
        bool writing{false};
        bool closed{false};
    };

    void accept_next();
    void remove(std::uint64_t connection);
    [[nodiscard]] auto session_for(std::uint64_t connection)
        -> std::shared_ptr<session>;

    transport_handler handler_{};
    asio::io_context context_{};
    using work_guard =
        asio::executor_work_guard<asio::io_context::executor_type>;
    std::optional<work_guard> work_{};
    std::unique_ptr<tcp::acceptor> acceptor_{};
    mutable std::mutex mutex_{};
    std::unordered_map<std::uint64_t, std::shared_ptr<session>> sessions_{};
    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<std::uint16_t> local_port_{0};
    std::vector<std::thread> threads_{};
    std::atomic<bool> stopping_{false};
};

transport_impl::transport_impl(transport_handler handler,
                               std::size_t io_threads)
    : handler_{std::move(handler)}, work_{asio::make_work_guard(context_)} {
    threads_.reserve(io_threads);
    try {
        for (std::size_t index{0}; index < io_threads; ++index) {
            threads_.emplace_back([this, index] {
                log::set_thread_name("net-io-" + std::to_string(index));
                context_.run();
            });
        }
    } catch (...) {
        work_.reset();
        context_.stop();
        for (auto &thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }
}

transport_impl::~transport_impl() { stop(); }

auto transport_impl::listen(const endpoint &bind) -> status {
    std::error_code error{};
    auto address{asio::ip::make_address(bind.host, error)};
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener address '{}' is invalid: {}", bind.host,
                        error.message()))};
    }
    auto acceptor{std::make_unique<tcp::acceptor>(context_)};
    const tcp::endpoint target{address, bind.port};
    acceptor->open(target.protocol(), error);
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener open failed: {}", error.message()))};
    }
    acceptor->set_option(tcp::acceptor::reuse_address{true}, error);
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener reuse_address failed: {}", error.message()))};
    }
    acceptor->bind(target, error);
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener bind failed: {}", error.message()))};
    }
    acceptor->listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener listen failed: {}", error.message()))};
    }
    local_port_.store(acceptor->local_endpoint(error).port(),
                      std::memory_order_relaxed);
    if (error) {
        return std::unexpected{transport_failure(
            fmt::format("listener endpoint failed: {}", error.message()))};
    }
    acceptor_ = std::move(acceptor);
    accept_next();
    return status_ok();
}

void transport_impl::accept_next() {
    if (!acceptor_ || stopping_) {
        return;
    }
    acceptor_->async_accept(
        [this](const std::error_code &error, tcp::socket socket) {
            if (error) {
                if (!stopping_ && handler_.on_error) {
                    handler_.on_error(error.message());
                }
                if (!stopping_) {
                    accept_next();
                }
                return;
            }
            const auto id{next_id_.fetch_add(1, std::memory_order_relaxed)};
            auto connection{
                std::make_shared<session>(*this, id, std::move(socket))};
            {
                const std::lock_guard lock{mutex_};
                sessions_.emplace(id, connection);
            }
            if (handler_.on_open) {
                handler_.on_open(id);
            }
            connection->start();
            accept_next();
        });
}

auto transport_impl::connect(const endpoint &target) -> result<std::uint64_t> {
    const auto id{next_id_.fetch_add(1, std::memory_order_relaxed)};
    auto socket{tcp::socket{context_}};
    auto connection{std::make_shared<session>(*this, id, std::move(socket))};
    {
        const std::lock_guard lock{mutex_};
        sessions_.emplace(id, connection);
    }
    auto resolver{std::make_shared<tcp::resolver>(context_)};
    resolver->async_resolve(
        target.host, std::to_string(target.port),
        [this, connection,
         resolver](const std::error_code &error,
                   const tcp::resolver::results_type &results) {
            if (error) {
                connection->finish(
                    fmt::format("resolve failed: {}", error.message()));
                return;
            }
            asio::async_connect(
                connection->socket, results,
                asio::bind_executor(
                    connection->strand,
                    [this, connection](const std::error_code &connect_error,
                                       const tcp::endpoint & /*endpoint*/) {
                        if (connect_error) {
                            connection->finish(fmt::format(
                                "connect failed: {}", connect_error.message()));
                            return;
                        }
                        if (handler_.on_open) {
                            handler_.on_open(connection->id);
                        }
                        connection->start();
                    }));
        });
    return id;
}

void transport_impl::send(std::uint64_t connection,
                          std::span<const std::byte> bytes) {
    auto target{session_for(connection)};
    if (!target) {
        return;
    }
    target->send_copy(std::vector<std::byte>{bytes.begin(), bytes.end()});
}

void transport_impl::close(std::uint64_t connection) {
    auto target{session_for(connection)};
    if (!target) {
        return;
    }
    asio::post(target->strand, [target] { target->finish("closed locally"); });
}

void transport_impl::stop() noexcept {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    asio::post(context_, [this] {
        std::error_code error{};
        if (acceptor_) {
            acceptor_->close(error);
            acceptor_.reset();
        }
        std::vector<std::shared_ptr<session>> connections{};
        {
            const std::lock_guard lock{mutex_};
            for (auto &[id, connection] : sessions_) {
                connections.push_back(connection);
                static_cast<void>(id);
            }
        }
        for (auto &connection : connections) {
            connection->finish("transport stopped");
        }
    });
    // Let the posted cleanup run: stopping the context here would discard it,
    // so open sessions would never report their close. Releasing the work
    // guard lets the io threads drain and return once the handlers finish.
    work_.reset();
    for (auto &thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    const std::lock_guard lock{mutex_};
    sessions_.clear();
}

auto transport_impl::connection_count() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return sessions_.size();
}

auto transport_impl::local_port() const -> std::uint16_t {
    return local_port_.load(std::memory_order_relaxed);
}

void transport_impl::remove(std::uint64_t connection) {
    const std::lock_guard lock{mutex_};
    sessions_.erase(connection);
}

auto transport_impl::session_for(std::uint64_t connection)
    -> std::shared_ptr<session> {
    const std::lock_guard lock{mutex_};
    const auto found{sessions_.find(connection)};
    return found == sessions_.end() ? nullptr : found->second;
}

void transport_impl::session::reader() {
    if (closed) {
        return;
    }
    socket.async_read_some(
        asio::buffer(read_buffer),
        asio::bind_executor(
            strand, [self = shared_from_this()](const std::error_code &error,
                                                std::size_t transferred) {
                if (self->closed) {
                    return;
                }
                if (error) {
                    self->finish(error.message());
                    return;
                }
                if (self->owner->handler_.on_data) {
                    self->owner->handler_.on_data(
                        self->id, std::span<const std::byte>{
                                      self->read_buffer.data(), transferred});
                }
                self->reader();
            }));
}

void transport_impl::session::writer() {
    if (closed || !connected || writes.empty()) {
        return;
    }
    writing = true;
    asio::async_write(
        socket, asio::buffer(writes.front()),
        asio::bind_executor(
            strand, [self = shared_from_this()](const std::error_code &error,
                                                std::size_t /*transferred*/) {
                self->writing = false;
                if (self->closed) {
                    return;
                }
                if (error) {
                    self->finish(error.message());
                    return;
                }
                self->writes.pop_front();
                self->writer();
            }));
}

void transport_impl::session::send_copy(std::vector<std::byte> bytes) {
    asio::post(strand,
               [self = shared_from_this(), data = std::move(bytes)]() mutable {
                   if (self->closed) {
                       return;
                   }
                   self->writes.push_back(std::move(data));
                   if (!self->writing) {
                       self->writer();
                   }
               });
}

void transport_impl::session::finish(std::string reason) {
    if (closed) {
        return;
    }
    closed = true;
    connected = false;
    std::error_code error{};
    socket.close(error);
    owner->remove(id);
    if (owner->handler_.on_close) {
        owner->handler_.on_close(id, reason);
    }
}

} // namespace

auto transport_impl::descriptor() const -> transport_descriptor {
    return transport_descriptor{
        .provider = "asio.tcp",
        .protocol = transport_protocol::tcp,
        .reliable = true,
        .ordered = true,
        .message_oriented = false,
        .mtu = 0,
        .backpressure = false,
    };
}

auto transport::open(transport_handler handler, std::size_t io_threads)
    -> result<std::unique_ptr<transport>> {
    if (io_threads == 0) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "network transport needs at least one I/O thread")};
    }
    try {
        return std::unique_ptr<transport>{
            std::make_unique<transport_impl>(std::move(handler), io_threads)};
    } catch (const std::system_error &failure) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("network I/O thread failed: {}", failure.what()))};
    }
}

} // namespace refrain::net
