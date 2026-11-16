// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/crypto.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace refrain::net {

// HTTPS downloads for first-run game data acquisition.
//
// The server fetches the Mojang version manifest and the official server
// archive once, verifies the archive against the SHA-1 the manifest publishes
// and keeps only the unpacked datapack. This module owns the transfer; the
// server layer owns what the bytes mean (JSON parsing, archive extraction,
// cache markers).
//
// The transport is deliberately injectable: tests implement
// `download_transport` over local files so the acquisition logic runs without
// network access and without any Mojang asset. The libcurl implementation is
// created with `make_curl_download_transport()`; it only speaks HTTPS, verifies
// TLS certificates and host names, and follows at most five HTTPS redirects
// (Mojang serves both metadata and archive through redirect-capable CDNs).
//
// Errors are values. Transfer and TLS failures are `io_error` /
// `permission_denied`, a deadline is `timeout`, and a sink that fails keeps
// its own error. Response bodies are streamed: the archive is never buffered
// whole in memory.

/// Ceiling for one metadata document (version manifest or version JSON). A
/// real manifest is a few hundred KiB; the bound keeps a hostile or broken
/// endpoint from growing the response buffer without limit.
inline constexpr std::size_t max_metadata_bytes = 16U << 20U;

/// Ceiling for one downloaded archive. The official server archive is roughly
/// 60 MiB; the bound is far above it while still rejecting an endless body.
inline constexpr std::uint64_t max_archive_bytes = 512ULL * 1024ULL * 1024ULL;

/// Receives the response body in order, as the transport decrypts it.
///
/// `write` may be called several times per transfer with arbitrary chunk
/// sizes; returning an error aborts the transfer. An implementation must not
/// retain the viewed bytes after the call returns.
class download_sink {
  public:
    download_sink() = default;
    download_sink(const download_sink &) = delete;
    auto operator=(const download_sink &) -> download_sink & = delete;
    download_sink(download_sink &&) = delete;
    auto operator=(download_sink &&) -> download_sink & = delete;
    virtual ~download_sink() = default;

    [[nodiscard]] virtual auto write(std::span<const std::byte> chunk)
        -> status = 0;
};

/// One archive download.
struct download_request {
    std::string url{};
    /// Reject the transfer as soon as the received body passes this size.
    std::uint64_t max_bytes{max_archive_bytes};
    /// Total deadline for the request, including connect and body transfer.
    std::chrono::milliseconds timeout{std::chrono::minutes{10}};
};

/// What the transport observed while streaming the body.
struct download_result {
    /// Received (and therefore sink-visible) body bytes.
    std::uint64_t bytes{0};
    /// SHA-1 of exactly those bytes, in network byte order.
    sha1_digest sha1{};
};

/// Injectable HTTP client used by first-run game data acquisition.
///
/// Implementations must:
///  - use TLS with certificate and host-name verification, and refuse
///    plaintext HTTP for the initial request and every redirect;
///  - stream the body to the sink in order, never buffering the whole body in
///    memory, and stop at `request.max_bytes`;
///  - apply `request.timeout` as a total deadline;
///  - return the SHA-1 of the bytes written to the sink;
///  - treat a non-2xx status as a failure naming the status and URL.
///
/// The interface makes no thread-safety promise; the first-run path is
/// single-threaded.
class download_transport {
  public:
    download_transport() = default;
    download_transport(const download_transport &) = delete;
    auto operator=(const download_transport &) -> download_transport & = delete;
    download_transport(download_transport &&) = delete;
    auto operator=(download_transport &&) -> download_transport & = delete;
    virtual ~download_transport() = default;

    /// Fetch a small text document (JSON metadata) into memory.
    [[nodiscard]] virtual auto fetch_text(std::string_view url,
                                          std::size_t max_bytes,
                                          std::chrono::milliseconds timeout)
        -> result<std::string> = 0;

    /// Stream one body into `sink` and return its size and SHA-1.
    [[nodiscard]] virtual auto download(const download_request &request,
                                        download_sink &sink)
        -> result<download_result> = 0;
};

/// libcurl-backed transport. Returns `unsupported` when the build has no
/// libcurl or no OpenSSL (the SHA-1 of the transfer is part of the contract).
[[nodiscard]] auto make_curl_download_transport()
    -> result<std::unique_ptr<download_transport>>;

} // namespace refrain::net
