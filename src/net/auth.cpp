// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Session-server authentication: URL construction, response validation and the
// optional libcurl transport.
//
// The service never handles an access token. The only network call is the
// server-side `hasJoined` query; tests replace `http_transport` with a fake and
// keep the process offline. The libcurl transport is compiled only when the
// build defines `REFRAIN_WITH_CURL`; without the macro the factory reports
// `unsupported` and `auth_service` still works with an injected transport.
//
// TLS policy for the libcurl transport: certificate and hostname verification
// are always on, the system trust store is used, non-HTTPS URLs are refused
// before curl sees them, and redirects are disabled. The transport never logs
// the URL, query string, headers or body (they identify the player).

#include <refrain/net/auth.hpp>

#include <refrain/data/json.hpp>
#include <refrain/net/crypto.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef REFRAIN_WITH_CURL
#define REFRAIN_WITH_CURL 0
#endif

#if REFRAIN_WITH_CURL
#include <curl/curl.h>
#endif

namespace refrain::net {

namespace {

[[nodiscard]] auto invalid_error(std::string_view detail) -> error {
    return make_auth_error(auth_error::invalid_argument, detail);
}

[[nodiscard]] auto malformed_error(std::string_view detail) -> error {
    return make_auth_error(auth_error::malformed_response, detail);
}

[[nodiscard]] auto is_ascii_hex(char character) noexcept -> bool {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

[[nodiscard]] auto is_ascii_control(char character) noexcept -> bool {
    const auto value{static_cast<unsigned char>(character)};
    return value < 0x20U || value == 0x7FU;
}

/// RFC 3986 percent-encoding for a query value. Unreserved characters pass
/// through; everything else becomes an uppercase `%XX` triplet. The input is
/// UTF-8, so bytes are encoded one at a time.
[[nodiscard]] auto url_encode(std::string_view text) -> std::string {
    constexpr char digits[]{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(text.size() * 3U);
    for (const char character : text) {
        const auto byte{static_cast<unsigned char>(character)};
        const bool unreserved{(byte >= static_cast<unsigned char>('A') &&
                               byte <= static_cast<unsigned char>('Z')) ||
                              (byte >= static_cast<unsigned char>('a') &&
                               byte <= static_cast<unsigned char>('z')) ||
                              (byte >= static_cast<unsigned char>('0') &&
                               byte <= static_cast<unsigned char>('9')) ||
                              byte == static_cast<unsigned char>('-') ||
                              byte == static_cast<unsigned char>('_') ||
                              byte == static_cast<unsigned char>('.') ||
                              byte == static_cast<unsigned char>('~')};
        if (unreserved) {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0FU]);
        }
    }
    return encoded;
}

[[nodiscard]] auto bytes_of_text(std::string_view text)
    -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

[[nodiscard]] auto json_string(const data::json::value &object,
                               std::string_view key)
    -> result<std::string_view> {
    const auto *member{object.find(key)};
    if (member == nullptr) {
        return std::unexpected{malformed_error(
            fmt::format("session-server profile is missing '{}'", key))};
    }
    const auto text{member->as_string()};
    if (!text) {
        return std::unexpected{malformed_error(fmt::format(
            "session-server profile field '{}' is not a string", key))};
    }
    return *text;
}

[[nodiscard]] auto parse_profile_uuid(std::string_view text) -> result<uuid> {
    std::string canonical;
    if (text.size() == 32) {
        canonical.reserve(36);
        for (std::size_t index{0}; index < text.size(); ++index) {
            if (index == 8 || index == 12 || index == 16 || index == 20) {
                canonical.push_back('-');
            }
            canonical.push_back(text[index]);
        }
    } else if (text.size() == 36) {
        canonical.assign(text);
    } else {
        return std::unexpected{
            malformed_error("session-server profile id has an invalid length")};
    }
    auto parsed{uuid::parse(canonical)};
    if (!parsed) {
        return std::unexpected{
            malformed_error("session-server profile id is not a UUID")};
    }
    return *parsed;
}

[[nodiscard]] auto validate_properties(const data::json::value &properties)
    -> result<std::vector<std::byte>> {
    const auto array{properties.as_array()};
    if (!array) {
        return std::unexpected{malformed_error(
            "session-server profile properties is not an array")};
    }
    if (array->size() > max_profile_properties) {
        return std::unexpected{
            malformed_error("session-server profile has too many properties")};
    }
    for (const auto &entry : *array) {
        if (!entry.is_object()) {
            return std::unexpected{malformed_error(
                "session-server profile property is not an object")};
        }
        auto name{json_string(entry, "name")};
        if (!name) {
            return std::unexpected{name.error()};
        }
        if (name->empty() || name->size() > 64U) {
            return std::unexpected{malformed_error(
                "session-server profile property name has an invalid length")};
        }
        auto value{json_string(entry, "value")};
        if (!value) {
            return std::unexpected{value.error()};
        }
        if (value->size() > 32'767U) {
            return std::unexpected{malformed_error(
                "session-server profile property value is too large")};
        }
        if (const auto *signature{entry.find("signature")};
            signature != nullptr) {
            const auto signature_text{signature->as_string()};
            if (!signature_text || signature_text->size() > 1024U) {
                return std::unexpected{malformed_error(
                    "session-server profile property signature is invalid")};
            }
        }
    }
    return bytes_of_text(properties.dump());
}

[[nodiscard]] auto parse_profile(std::string_view body) -> result<profile> {
    auto parsed{data::json::value::parse(body)};
    if (!parsed) {
        return std::unexpected{
            malformed_error("session-server response is not valid JSON")};
    }
    if (!parsed->is_object()) {
        return std::unexpected{
            malformed_error("session-server response is not a JSON object")};
    }

    auto id_text{json_string(*parsed, "id")};
    if (!id_text) {
        return std::unexpected{id_text.error()};
    }
    auto id{parse_profile_uuid(*id_text)};
    if (!id) {
        return std::unexpected{id.error()};
    }

    auto name_text{json_string(*parsed, "name")};
    if (!name_text) {
        return std::unexpected{name_text.error()};
    }
    if (name_text->empty() || name_text->size() > 16U ||
        !is_valid_utf8(*name_text)) {
        return std::unexpected{malformed_error(
            "session-server profile name has an invalid length")};
    }

    std::vector<std::byte> properties;
    if (const auto *properties_value{parsed->find("properties")};
        properties_value != nullptr) {
        auto validated{validate_properties(*properties_value)};
        if (!validated) {
            return std::unexpected{validated.error()};
        }
        properties = std::move(*validated);
    } else {
        properties = bytes_of_text("[]");
    }

    return profile{.id = *id,
                   .name = std::string{*name_text},
                   .properties = std::move(properties)};
}

} // namespace

auto to_string(auth_error reason) noexcept -> std::string_view {
    switch (reason) {
    case auth_error::invalid_argument:
        return "invalid_argument";
    case auth_error::unsupported:
        return "unsupported";
    case auth_error::transport_failure:
        return "transport_failure";
    case auth_error::tls_failure:
        return "tls_failure";
    case auth_error::timeout:
        return "timeout";
    case auth_error::not_joined:
        return "not_joined";
    case auth_error::remote_error:
        return "remote_error";
    case auth_error::malformed_response:
        return "malformed_response";
    }
    return "unknown";
}

auto to_error_code(auth_error reason) noexcept -> error_code {
    switch (reason) {
    case auth_error::invalid_argument:
        return error_code::invalid_argument;
    case auth_error::unsupported:
        return error_code::unsupported;
    case auth_error::transport_failure:
        return error_code::io_error;
    case auth_error::tls_failure:
        return error_code::permission_denied;
    case auth_error::timeout:
        return error_code::timeout;
    case auth_error::not_joined:
        return error_code::not_found;
    case auth_error::remote_error:
        return error_code::unknown;
    case auth_error::malformed_response:
        return error_code::parse_error;
    }
    return error_code::unknown;
}

auto make_auth_error(auth_error reason, std::string_view detail,
                     std::source_location location) -> error {
    std::string message{to_string(reason)};
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    }
    return make_error(to_error_code(reason), message, location);
}

auto auth_error_of(const error &failure) noexcept -> std::optional<auth_error> {
    switch (failure.code) {
    case error_code::invalid_argument:
        return auth_error::invalid_argument;
    case error_code::unsupported:
        return auth_error::unsupported;
    case error_code::io_error:
        return auth_error::transport_failure;
    case error_code::permission_denied:
        return auth_error::tls_failure;
    case error_code::timeout:
        return auth_error::timeout;
    case error_code::not_found:
        return auth_error::not_joined;
    case error_code::unknown:
        return auth_error::remote_error;
    case error_code::parse_error:
        return auth_error::malformed_response;
    default:
        return std::nullopt;
    }
}

auto offline_profile(std::string_view name) -> result<profile> {
    if (name.empty() || name.size() > 16U || !is_valid_utf8(name)) {
        return std::unexpected{
            invalid_error("offline player name must be 1..16 UTF-8 bytes")};
    }

    std::string material{"OfflinePlayer:"};
    material.append(name);
    auto digest{md5(std::as_bytes(
        std::span<const char>{material.data(), material.size()}))};
    secure_zero(std::span<std::byte>{
        reinterpret_cast<std::byte *>(material.data()), material.size()});
    if (!digest) {
        return std::unexpected{
            make_auth_error(auth_error::unsupported,
                            "offline UUID generation requires MD5 support")};
    }

    uuid id{};
    for (std::size_t index{0}; index < id.bytes.size(); ++index) {
        id.bytes[index] = std::to_integer<std::uint8_t>((*digest)[index]);
    }
    // UUID version 3 (name-based, MD5) and RFC 4122 variant.
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0FU) | 0x30U);
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3FU) | 0x80U);
    return profile{
        .id = id, .name = std::string{name}, .properties = bytes_of_text("[]")};
}

auth_service::auth_service(http_transport &transport, auth_options options)
    : transport_{&transport}, options_{std::move(options)} {}

auto auth_service::build_url(std::string_view name,
                             std::string_view server_hash,
                             std::string_view remote_ip) const
    -> result<std::string> {
    std::string base{options_.session_server};
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (!base.starts_with("https://")) {
        return std::unexpected{
            invalid_error("session-server URL must start with https://")};
    }
    if (base.size() == std::string_view{"https://"}.size()) {
        return std::unexpected{invalid_error("session-server URL has no host")};
    }
    if (base.find_first_of("?#") != std::string::npos ||
        std::any_of(base.begin(), base.end(), [](char character) {
            return std::isspace(static_cast<unsigned char>(character)) != 0 ||
                   is_ascii_control(character);
        })) {
        return std::unexpected{
            invalid_error("session-server URL contains an invalid character")};
    }

    std::string url{
        fmt::format("{}/session/minecraft/hasJoined?username={}&serverId={}",
                    base, url_encode(name), url_encode(server_hash))};
    if (!remote_ip.empty()) {
        url += fmt::format("&ip={}", url_encode(remote_ip));
    }
    return url;
}

auto auth_service::has_joined(std::string_view name,
                              std::string_view server_hash,
                              std::string_view remote_ip) -> result<profile> {
    if (name.empty() || name.size() > 16U || !is_valid_utf8(name)) {
        return std::unexpected{
            invalid_error("username must be 1..16 UTF-8 bytes")};
    }
    if (server_hash.empty() || server_hash.size() > 41U) {
        return std::unexpected{
            invalid_error("server hash must be 1..41 characters")};
    }
    std::size_t hash_start{0};
    if (server_hash.front() == '-') {
        hash_start = 1;
    }
    if (hash_start == server_hash.size() ||
        !std::all_of(server_hash.begin() +
                         static_cast<std::ptrdiff_t>(hash_start),
                     server_hash.end(),
                     [](char character) { return is_ascii_hex(character); })) {
        return std::unexpected{
            invalid_error("server hash must contain only hex digits")};
    }
    if (!remote_ip.empty()) {
        if (remote_ip.size() > 64U || !is_valid_utf8(remote_ip) ||
            std::any_of(remote_ip.begin(), remote_ip.end(), is_ascii_control)) {
            return std::unexpected{
                invalid_error("remote IP must be 0..64 printable UTF-8 bytes")};
        }
    }
    const auto timeout{options_.timeout.count()};
    if (timeout < 1 || timeout > 60'000) {
        return std::unexpected{
            invalid_error("authentication timeout must be 1..60000 ms")};
    }

    auto url{build_url(name, server_hash, remote_ip)};
    if (!url) {
        return std::unexpected{url.error()};
    }

    http_request request{};
    request.method = "GET";
    request.url = std::move(*url);
    request.headers = {"Accept: application/json", "User-Agent: Refrain"};
    request.timeout = options_.timeout;

    auto response{transport_->send(request)};
    if (!response) {
        return std::unexpected{response.error()};
    }

    if (response->status == 204) {
        return std::unexpected{make_auth_error(
            auth_error::not_joined, "the session server has no matching join")};
    }
    if (response->status == 0) {
        return std::unexpected{
            make_auth_error(auth_error::transport_failure,
                            "the transport returned no HTTP status")};
    }
    if (response->status != 200) {
        return std::unexpected{
            make_auth_error(auth_error::remote_error,
                            fmt::format("the session server returned HTTP {}",
                                        response->status))};
    }
    if (response->body.size() > max_session_response_bytes) {
        return std::unexpected{malformed_error(
            "the session-server response exceeds the size limit")};
    }
    return parse_profile(response->body);
}

#if REFRAIN_WITH_CURL

namespace {

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

struct response_sink {
    std::string body;
    bool overflow{false};
};

[[nodiscard]] auto write_response(char *data, std::size_t size,
                                  std::size_t count, void *userdata)
    -> std::size_t {
    auto *sink{static_cast<response_sink *>(userdata)};
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        sink->overflow = true;
        return 0;
    }
    const auto length{size * count};
    if (length > max_session_response_bytes - sink->body.size()) {
        sink->overflow = true;
        return 0;
    }
    sink->body.append(data, length);
    return length;
}

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

[[nodiscard]] auto curl_error(CURLcode code) -> error {
    const char *text{curl_easy_strerror(code)};
    const std::string_view detail{text != nullptr ? text : "libcurl error"};
    if (code == CURLE_OPERATION_TIMEDOUT) {
        return make_auth_error(auth_error::timeout, detail);
    }
    if (ssl_failure(code)) {
        return make_auth_error(auth_error::tls_failure, detail);
    }
    return make_auth_error(auth_error::transport_failure, detail);
}

class curl_transport final : public http_transport {
  public:
    auto send(const http_request &request) -> result<http_response> override {
        if (!curl_ready()) {
            return std::unexpected{make_auth_error(
                auth_error::unsupported, "libcurl could not initialize")};
        }
        if (request.method != "GET" && request.method != "POST") {
            return std::unexpected{
                invalid_error("libcurl transport only supports GET and POST")};
        }
        if (!request.url.starts_with("https://")) {
            return std::unexpected{
                invalid_error("libcurl transport only supports https URLs")};
        }
        const auto timeout{request.timeout.count()};
        if (timeout < 1 || timeout > 60'000) {
            return std::unexpected{
                invalid_error("request timeout must be 1..60000 ms")};
        }
        for (const auto &header : request.headers) {
            if (header.find(':') == std::string::npos ||
                header.find_first_of("\r\n") != std::string::npos) {
                return std::unexpected{
                    invalid_error("request header is malformed")};
            }
        }

        curl_list headers;
        curl_slist *header_list{nullptr};
        for (const auto &header : request.headers) {
            curl_slist *updated{curl_slist_append(header_list, header.c_str())};
            if (updated == nullptr) {
                curl_slist_free_all(header_list);
                return std::unexpected{make_auth_error(
                    auth_error::transport_failure,
                    "could not allocate the request header list")};
            }
            header_list = updated;
        }
        headers.reset(header_list);

        curl_handle handle{curl_easy_init()};
        if (!handle) {
            return std::unexpected{
                make_auth_error(auth_error::transport_failure,
                                "could not allocate a libcurl handle")};
        }
        CURL *curl{handle.get()};
        response_sink sink;

        const auto set = [curl](CURLoption option, auto value) {
            return curl_easy_setopt(curl, option, value) == CURLE_OK;
        };
        const bool configured{
            set(CURLOPT_URL, request.url.c_str()) &&
            set(CURLOPT_NOSIGNAL, 1L) && set(CURLOPT_FOLLOWLOCATION, 0L) &&
            set(CURLOPT_MAXREDIRS, 0L) && set(CURLOPT_SSL_VERIFYPEER, 1L) &&
            set(CURLOPT_SSL_VERIFYHOST, 2L) &&
            set(CURLOPT_PROTOCOLS_STR, "https") &&
            set(CURLOPT_REDIR_PROTOCOLS_STR, "https") &&
            set(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout)) &&
            set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout)) &&
            set(CURLOPT_WRITEFUNCTION, &write_response) &&
            set(CURLOPT_WRITEDATA, &sink)};
        if (!configured) {
            return std::unexpected{
                make_auth_error(auth_error::transport_failure,
                                "could not configure the libcurl request")};
        }
        if (headers) {
            if (!set(CURLOPT_HTTPHEADER, headers.get())) {
                return std::unexpected{
                    make_auth_error(auth_error::transport_failure,
                                    "could not attach the request headers")};
            }
        }
        if (request.method == "POST") {
            if (!set(CURLOPT_POST, 1L) ||
                !set(CURLOPT_POSTFIELDS, request.body.data()) ||
                !set(CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()))) {
                return std::unexpected{make_auth_error(
                    auth_error::transport_failure,
                    "could not configure the libcurl request body")};
            }
        } else if (!set(CURLOPT_HTTPGET, 1L)) {
            return std::unexpected{make_auth_error(
                auth_error::transport_failure,
                "could not configure the libcurl request method")};
        }

        const CURLcode performed{curl_easy_perform(curl)};
        if (sink.overflow) {
            return std::unexpected{make_auth_error(
                auth_error::transport_failure,
                "the session-server response exceeds the size limit")};
        }
        if (performed != CURLE_OK) {
            return std::unexpected{curl_error(performed)};
        }
        long status{0};
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) !=
                CURLE_OK ||
            status <= 0 || status > 999) {
            return std::unexpected{
                make_auth_error(auth_error::transport_failure,
                                "libcurl returned no HTTP status")};
        }
        return http_response{.status = static_cast<int>(status),
                             .body = std::move(sink.body)};
    }
};

} // namespace

#endif // REFRAIN_WITH_CURL

auto make_curl_transport() -> result<std::unique_ptr<http_transport>> {
#if REFRAIN_WITH_CURL
    try {
        return std::unique_ptr<http_transport>{
            std::make_unique<curl_transport>()};
    } catch (const std::bad_alloc &) {
        return std::unexpected{
            make_auth_error(auth_error::unsupported,
                            "could not allocate the libcurl transport")};
    }
#else
    return std::unexpected{make_auth_error(
        auth_error::unsupported, "libcurl support is not compiled in")};
#endif
}

} // namespace refrain::net
