// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/download.hpp>

#include <refrain/core/version.hpp>

#include <fmt/format.h>

#if REFRAIN_WITH_CURL && REFRAIN_WITH_OPENSSL
#include <curl/curl.h>
#include <openssl/evp.h>
#endif

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::net {

namespace {

[[nodiscard]] auto transfer_failure(std::string message) -> error {
    return make_error(error_code::io_error, std::move(message));
}

[[nodiscard]] auto invalid_request(std::string message) -> error {
    return make_error(error_code::invalid_argument, std::move(message));
}

#if REFRAIN_WITH_CURL && REFRAIN_WITH_OPENSSL

/// Incremental SHA-1 of a response body. The download result must name the
/// exact bytes the sink accepted, and the archive can be hundreds of MiB, so
/// the digest is folded in as chunks arrive instead of after the transfer.
class sha1_stream {
  public:
    sha1_stream() noexcept : context_{EVP_MD_CTX_new()} {
        if (context_ != nullptr &&
            EVP_DigestInit_ex(context_, EVP_sha1(), nullptr) != 1) {
            EVP_MD_CTX_free(context_);
            context_ = nullptr;
        }
    }

    sha1_stream(const sha1_stream &) = delete;
    auto operator=(const sha1_stream &) -> sha1_stream & = delete;
    sha1_stream(sha1_stream &&) = delete;
    auto operator=(sha1_stream &&) -> sha1_stream & = delete;

    ~sha1_stream() {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(context_);
        }
    }

    [[nodiscard]] auto ready() const noexcept -> bool {
        return context_ != nullptr;
    }

    [[nodiscard]] auto update(std::span<const std::byte> chunk) -> bool {
        return chunk.empty() ||
               EVP_DigestUpdate(context_, chunk.data(), chunk.size()) == 1;
    }

    [[nodiscard]] auto finish(sha1_digest &digest) noexcept -> bool {
        unsigned int length{0};
        return EVP_DigestFinal_ex(
                   context_, reinterpret_cast<unsigned char *>(digest.data()),
                   &length) == 1 &&
               length == digest.size();
    }

  private:
    EVP_MD_CTX *context_{nullptr};
};

/// One in-flight transfer: the sink, the running digest and the failure the
/// libcurl callback reported (libcurl only sees a short write, not the error).
struct transfer_state {
    download_sink *sink{nullptr};
    std::uint64_t max_bytes{0};
    std::uint64_t received{0};
    sha1_stream hasher{};
    bool failed{false};
    error failure{};
};

void report_failure(transfer_state &state, error cause) {
    if (!state.failed) {
        state.failed = true;
        state.failure = std::move(cause);
    }
}

[[nodiscard]] auto write_body(char *data, std::size_t size, std::size_t count,
                              void *userdata) -> std::size_t {
    auto &state{*static_cast<transfer_state *>(userdata)};
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        report_failure(state,
                       transfer_failure("the response chunk length overflows"));
        return 0;
    }
    const auto length{size * count};
    if (length == 0) {
        return 0;
    }
    if (static_cast<std::uint64_t>(length) > state.max_bytes - state.received) {
        report_failure(state,
                       transfer_failure(fmt::format(
                           "the response exceeds the {} byte download limit",
                           state.max_bytes)));
        return 0;
    }
    const auto chunk{std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(data), length}};
    if (!state.hasher.update(chunk)) {
        report_failure(state,
                       transfer_failure("failed to hash the response body"));
        return 0;
    }
    if (!state.sink->write(chunk)) {
        report_failure(state, transfer_failure("the download sink failed"));
        return 0;
    }
    state.received += length;
    return length;
}

/// Buffers a small metadata response in memory.
class text_sink final : public download_sink {
  public:
    explicit text_sink(std::size_t max_bytes) : max_bytes_{max_bytes} {}

    [[nodiscard]] auto write(std::span<const std::byte> chunk)
        -> status override {
        if (chunk.size() > max_bytes_ - body_.size()) {
            return std::unexpected{transfer_failure(
                fmt::format("the response exceeds the {} byte document limit",
                            max_bytes_))};
        }
        body_.append(reinterpret_cast<const char *>(chunk.data()),
                     chunk.size());
        return {};
    }

    [[nodiscard]] auto body() && -> std::string { return std::move(body_); }

  private:
    std::size_t max_bytes_{0};
    std::string body_{};
};

[[nodiscard]] auto curl_ready() noexcept -> bool {
    static std::once_flag flag;
    static bool ready{false};
    std::call_once(flag, [] {
        ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    });
    return ready;
}

[[nodiscard]] auto ssl_failure(CURLcode code) noexcept -> bool {
    switch (code) {
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_USE_SSL_FAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_CLIENTCERT:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] auto curl_error(CURLcode code, std::string_view url) -> error {
    const char *text{curl_easy_strerror(code)};
    const std::string_view detail{text != nullptr ? text : "libcurl error"};
    const auto message{fmt::format("download of {} failed: {}", url, detail)};
    if (code == CURLE_OPERATION_TIMEDOUT) {
        return make_error(error_code::timeout, message);
    }
    if (ssl_failure(code)) {
        return make_error(error_code::permission_denied, message);
    }
    return make_error(error_code::io_error, message);
}

struct curl_handle_deleter {
    void operator()(CURL *handle) const noexcept {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

struct curl_list_deleter {
    void operator()(curl_slist *list) const noexcept {
        if (list != nullptr) {
            curl_slist_free_all(list);
        }
    }
};

using curl_handle = std::unique_ptr<CURL, curl_handle_deleter>;
using curl_list = std::unique_ptr<curl_slist, curl_list_deleter>;

[[nodiscard]] auto validate_url(std::string_view url) -> status {
    if (!url.starts_with("https://")) {
        return std::unexpected{invalid_request(
            fmt::format("download url '{}' must use https", url))};
    }
    return {};
}

[[nodiscard]] auto validate_timeout(std::chrono::milliseconds timeout)
    -> status {
    if (timeout.count() < 1 || timeout.count() > 3'600'000) {
        return std::unexpected{
            invalid_request("download timeout must be 1..3600000 ms")};
    }
    return {};
}

/// Shared curl setup for both transfer shapes: HTTPS only, TLS verification,
/// bounded redirects, no progress UI, total deadline.
///
/// `curl`, `url` and `headers` stay alive until after `curl_easy_perform()`;
/// the caller owns them and all outlive the call. libcurl keeps pointers into
/// the header list for the duration of the transfer.
[[nodiscard]] auto configure_handle(CURL *curl, const std::string &url,
                                    std::chrono::milliseconds timeout,
                                    transfer_state &state, curl_list &headers)
    -> status {
    const auto set = [curl](CURLoption option, auto value) {
        return curl_easy_setopt(curl, option, value) == CURLE_OK;
    };
    const std::vector<std::string> header_lines{
        "Accept: application/json",
        fmt::format("User-Agent: Refrain/{}",
                    refrain::version::release_version)};
    curl_slist *header_list{nullptr};
    for (const auto &header : header_lines) {
        curl_slist *updated{curl_slist_append(header_list, header.c_str())};
        if (updated == nullptr) {
            curl_slist_free_all(header_list);
            return std::unexpected{
                transfer_failure("could not allocate the request headers")};
        }
        header_list = updated;
    }
    headers.reset(header_list);

    const auto timeout_ms{static_cast<long>(timeout.count())};
    const bool configured{
        set(CURLOPT_URL, url.c_str()) && set(CURLOPT_NOSIGNAL, 1L) &&
        set(CURLOPT_FOLLOWLOCATION, 1L) && set(CURLOPT_MAXREDIRS, 5L) &&
        set(CURLOPT_SSL_VERIFYPEER, 1L) && set(CURLOPT_SSL_VERIFYHOST, 2L) &&
        set(CURLOPT_PROTOCOLS_STR, "https") &&
        set(CURLOPT_REDIR_PROTOCOLS_STR, "https") &&
        set(CURLOPT_TIMEOUT_MS, timeout_ms) &&
        set(CURLOPT_CONNECTTIMEOUT_MS, timeout_ms) &&
        set(CURLOPT_WRITEFUNCTION, &write_body) &&
        set(CURLOPT_WRITEDATA, &state) &&
        set(CURLOPT_HTTPHEADER, headers.get()) && set(CURLOPT_HTTPGET, 1L)};
    if (!configured) {
        return std::unexpected{
            transfer_failure("could not configure the download request")};
    }
    return {};
}

/// Perform one request and map its status/transport result to our error model.
[[nodiscard]] auto perform(CURL *curl, std::string_view url,
                           transfer_state &state) -> result<long> {
    const CURLcode performed{curl_easy_perform(curl)};
    if (state.failed) {
        return std::unexpected{std::move(state.failure)};
    }
    if (performed != CURLE_OK) {
        return std::unexpected{curl_error(performed, url)};
    }
    long status{0};
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK ||
        status <= 0 || status > 999) {
        return std::unexpected{transfer_failure(
            fmt::format("download of {} returned no HTTP status", url))};
    }
    return status;
}

class curl_download_transport final : public download_transport {
  public:
    [[nodiscard]] auto fetch_text(std::string_view url, std::size_t max_bytes,
                                  std::chrono::milliseconds timeout)
        -> result<std::string> override {
        if (!curl_ready()) {
            return std::unexpected{
                transfer_failure("libcurl could not initialize")};
        }
        if (auto valid{validate_url(url)}; !valid) {
            return std::unexpected{valid.error()};
        }
        if (auto valid{validate_timeout(timeout)}; !valid) {
            return std::unexpected{valid.error()};
        }
        if (max_bytes == 0) {
            return std::unexpected{
                invalid_request("download document limit must be positive")};
        }
        const std::string url_text{url};
        curl_handle handle{curl_easy_init()};
        if (!handle) {
            return std::unexpected{
                transfer_failure("could not allocate a libcurl handle")};
        }
        text_sink sink{max_bytes};
        transfer_state state{
            .sink = &sink, .max_bytes = static_cast<std::uint64_t>(max_bytes)};
        if (!state.hasher.ready()) {
            return std::unexpected{
                transfer_failure("could not allocate the SHA-1 state")};
        }
        curl_list headers{};
        if (auto configured{configure_handle(handle.get(), url_text, timeout,
                                             state, headers)};
            !configured) {
            return std::unexpected{configured.error()};
        }
        auto status{perform(handle.get(), url_text, state)};
        if (!status) {
            return std::unexpected{status.error()};
        }
        if (*status < 200 || *status > 299) {
            return std::unexpected{transfer_failure(fmt::format(
                "download of {} returned HTTP {}", url_text, *status))};
        }
        return std::move(sink).body();
    }

    [[nodiscard]] auto download(const download_request &request,
                                download_sink &sink)
        -> result<download_result> override {
        if (!curl_ready()) {
            return std::unexpected{
                transfer_failure("libcurl could not initialize")};
        }
        if (auto valid{validate_url(request.url)}; !valid) {
            return std::unexpected{valid.error()};
        }
        if (auto valid{validate_timeout(request.timeout)}; !valid) {
            return std::unexpected{valid.error()};
        }
        if (request.max_bytes == 0) {
            return std::unexpected{
                invalid_request("download body limit must be positive")};
        }
        curl_handle handle{curl_easy_init()};
        if (!handle) {
            return std::unexpected{
                transfer_failure("could not allocate a libcurl handle")};
        }
        transfer_state state{.sink = &sink, .max_bytes = request.max_bytes};
        if (!state.hasher.ready()) {
            return std::unexpected{
                transfer_failure("could not allocate the SHA-1 state")};
        }
        curl_list headers{};
        if (auto configured{configure_handle(handle.get(), request.url,
                                             request.timeout, state, headers)};
            !configured) {
            return std::unexpected{configured.error()};
        }
        auto status{perform(handle.get(), request.url, state)};
        if (!status) {
            return std::unexpected{status.error()};
        }
        if (*status < 200 || *status > 299) {
            return std::unexpected{transfer_failure(fmt::format(
                "download of {} returned HTTP {}", request.url, *status))};
        }
        download_result result{.bytes = state.received};
        if (!state.hasher.finish(result.sha1)) {
            return std::unexpected{
                transfer_failure("failed to finalize the response digest")};
        }
        return result;
    }
};

#endif // REFRAIN_WITH_CURL && REFRAIN_WITH_OPENSSL

} // namespace

auto make_curl_download_transport()
    -> result<std::unique_ptr<download_transport>> {
#if REFRAIN_WITH_CURL && REFRAIN_WITH_OPENSSL
    try {
        return std::unique_ptr<download_transport>{
            std::make_unique<curl_download_transport>()};
    } catch (const std::bad_alloc &) {
        return std::unexpected{
            make_error(error_code::unsupported,
                       "could not allocate the libcurl download transport")};
    }
#else
    return std::unexpected{make_error(
        error_code::unsupported, "downloads need a libcurl and OpenSSL build")};
#endif
}

} // namespace refrain::net
