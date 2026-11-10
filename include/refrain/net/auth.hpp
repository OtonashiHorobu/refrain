// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

// Online-mode player authentication.
//
// The server never sees a Mojang access token: after the login handshake it
// computes the login hash (see `minecraft_server_hash()` in crypto.hpp) from
// the decrypted shared secret and the server's public key, then asks the
// session server whether the client completed the matching `join` call:
//
//   GET <session_server>/session/minecraft/hasJoined
//       ?username=<name>&serverId=<hash>[&ip=<remote_ip>]
//
// A 204 response means the client has not joined (a normal negative result);
// a 200 response carries the profile to send in Login Success; a 5xx response
// means the session server is unavailable. Those three cases are distinct in
// `auth_error` so the login handler can choose between "reject this login",
// "retry/queue the login" and "this is a client-side failure".
//
// HTTP access goes through `http_transport`, a narrow injectable interface.
// `auth_service` depends only on that interface, so unit tests supply a fake
// transport and never touch the network. The libcurl implementation is created
// with `make_curl_transport()` and is compiled only when the build defines
// `REFRAIN_WITH_CURL`; without it the factory returns `unsupported`.
//
// Privacy: a request URL contains the player name and, when supplied, the
// client IP address. Neither is secret, but both are player-identifying data:
// do not log request URLs, query strings or response bodies at info level.
// No access token, session-server response or account data is stored in this
// layer beyond the returned profile.
//
// Error handling: no exceptions cross this API. Failures are
// `refrain::result` values. Because `refrain::result` fixes its error payload
// to `refrain::error`, the typed category is carried losslessly in
// `error.code` by `make_auth_error()` and recovered with `auth_error_of()`; no
// caller needs to parse error text.

/// Session-server response ceiling. A real profile is a few KiB; the bound
/// keeps a hostile or broken endpoint from growing the response buffer without
/// limit in the libcurl transport and in the JSON parser path.
inline constexpr std::size_t max_session_response_bytes = 1U << 20U;

/// Maximum number of `properties` entries accepted from a profile (the
/// protocol's GameProfile property list is prefixed, and vanilla profiles carry
/// one or two entries).
inline constexpr std::size_t max_profile_properties = 16;

/// Typed failure categories of the authentication service.
///
/// `refrain::result` stores a generic `refrain::error`; `make_auth_error()` is
/// the only supported constructor and `auth_error_of()` performs the inverse
/// narrowing. The mapping is one-to-one and stable:
///
///   auth_error           error_code
///   invalid_argument     invalid_argument
///   unsupported          unsupported
///   transport_failure    io_error
///   tls_failure          permission_denied
///   timeout              timeout
///   not_joined           not_found
///   remote_error         unknown
///   malformed_response   parse_error
enum class auth_error : std::uint8_t {
    /// The caller supplied an unusable name, hash, IP or session-server URL.
    invalid_argument,
    /// The requested transport/crypto is not compiled into this build.
    unsupported,
    /// DNS, connect, send or receive failed before an HTTP status arrived.
    transport_failure,
    /// TLS setup or certificate verification failed (the request was not
    /// sent, or its response was not trusted).
    tls_failure,
    /// The request exceeded its deadline.
    timeout,
    /// The session server answered 204: the client has not joined this server.
    not_joined,
    /// The session server answered 5xx or another unexpected HTTP status; the
    /// status is named in the error message.
    remote_error,
    /// The session server answered 200 with a body that is not a valid
    /// profile for the protocol.
    malformed_response,
};

/// Stable diagnostic name (lowercase snake case).
[[nodiscard]] auto to_string(auth_error reason) noexcept -> std::string_view;

/// Map a typed auth failure onto the shared error taxonomy.
[[nodiscard]] auto to_error_code(auth_error reason) noexcept -> error_code;

/// Build the `refrain::error` returned by this service. `detail` should be a
/// short, player-independent diagnostic; do not pass a request URL, query
/// string or response body.
[[nodiscard]] auto
make_auth_error(auth_error reason, std::string_view detail = {},
                std::source_location location = std::source_location::current())
    -> error;

/// Recover the typed auth category from an error produced by this service.
///
/// Returns `std::nullopt` for an `error_code` this service never produces
/// (for example a value from an unrelated subsystem). Applying it to an
/// arbitrary `refrain::error` is allowed but only auth-produced errors are
/// guaranteed to map back to their original category.
[[nodiscard]] auto auth_error_of(const error &failure) noexcept
    -> std::optional<auth_error>;

/// A resolved player profile.
struct profile {
    /// Canonical 128-bit UUID.
    uuid id{};

    /// Player name as returned by the session server (canonical casing).
    std::string name;

    /// Compact UTF-8 JSON array of the session server's `properties` member,
    /// for example `[{"name":"textures","value":"..."}]`. No properties is the
    /// two-byte array `[]`; a missing member is treated the same way. The
    /// bytes are kept opaque here because the Login Success encoder owns the
    /// wire representation (the protocol encodes the entries as a prefixed
    /// array of name/value/optional-signature strings). The JSON text is
    /// canonical for the parsed value (object keys sorted), so equal profiles
    /// produce equal bytes.
    std::vector<std::byte> properties;
};

/// Derive the deterministic offline-mode profile for `name`.
///
/// Implements Java's `UUID.nameUUIDFromBytes(("OfflinePlayer:" + name)
/// .getBytes(UTF_8))` followed by the version-3/variant-IETF bit fixups, then
/// returns the empty property list (`[]`). This is the identity an offline-mode
/// server uses; it is not proof of authentication.
///
/// Fails with `invalid_argument` for an empty name, a name longer than 16 UTF-8
/// bytes, or invalid UTF-8, and with `unsupported` when MD5 is unavailable
/// (an OpenSSL-free build). No network access is involved.
[[nodiscard]] auto offline_profile(std::string_view name) -> result<profile>;

/// One HTTP request issued by `auth_service`.
///
/// `headers` holds complete header lines (`"Name: value"`); the transport must
/// reject values containing CR/LF. Only `GET` and `POST` are produced by this
/// layer. `timeout` is the total request deadline in milliseconds.
struct http_request {
    std::string method{"GET"};
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{5000};
};

/// One HTTP response returned by a transport.
///
/// `status` is the numeric HTTP status (200, 204, 500, ...), or 0 when no
/// status was received; a 0 status is treated as a transport failure by
/// `auth_service`. `body` is the decoded response body (content encoding is
/// the transport's responsibility) and is bounded by the transport; the auth
/// layer additionally rejects bodies above `max_session_response_bytes`.
struct http_response {
    int status{0};
    std::string body;
};

/// Minimal injectable HTTP transport.
///
/// Implementations must:
///  - perform the request with the supplied total timeout;
///  - use TLS with certificate and hostname verification for `https://` URLs
///    and refuse plaintext HTTP;
///  - return transport/TLS/timeout failures through `make_auth_error()`
///    (`transport_failure`, `tls_failure`, `timeout`), so `auth_error_of()`
///    keeps working when `auth_service` propagates them;
///  - avoid logging URLs, headers or bodies (they contain player data);
///  - return the status and decoded body for any received HTTP response,
///    including 4xx/5xx, without turning them into transport errors.
///
/// A transport may be called from several threads when the owner does so; the
/// interface makes no thread-safety promise itself.
class http_transport {
  public:
    http_transport() = default;
    http_transport(const http_transport &) = delete;
    auto operator=(const http_transport &) -> http_transport & = delete;
    http_transport(http_transport &&) = delete;
    auto operator=(http_transport &&) -> http_transport & = delete;
    virtual ~http_transport() = default;

    [[nodiscard]] virtual auto send(const http_request &request)
        -> result<http_response> = 0;
};

/// Session-server endpoint and request deadline.
struct auth_options {
    /// Base URL of the session server. Must use `https://`; trailing slashes
    /// are ignored. The default is Mojang's public endpoint; an operator may
    /// point it at a compatible proxy.
    std::string session_server{"https://sessionserver.mojang.com"};

    /// Total deadline applied to each `hasJoined` request.
    std::chrono::milliseconds timeout{5000};
};

/// Online-mode authentication service.
///
/// The service is stateless apart from its transport reference and options;
/// it may be used concurrently when the transport is safe for that use.
/// The transport must outlive the service.
class auth_service {
  public:
    explicit auth_service(http_transport &transport, auth_options options = {});

    /// Query `hasJoined` for a client that claims `name` and completed the
    /// join for `server_hash`.
    ///
    /// Validation (all fail with `invalid_argument` before any request):
    ///  - `name` is 1..16 UTF-8 bytes;
    ///  - `server_hash` is 1..41 characters of lowercase/uppercase hex with an
    ///    optional leading `'-'` (the signed-hex login hash);
    ///  - `remote_ip` is empty or at most 64 UTF-8 bytes;
    ///  - the configured session-server URL starts with `https://` and its
    ///    deadline is between 1 ms and 60 s.
    ///
    /// `name`, `server_hash` and `remote_ip` are percent-encoded into the
    /// query string; `remote_ip` is omitted when empty. On a 200 response the
    /// returned profile is validated (UUID, name, properties shape and size)
    /// and uses the response's canonical name, not the requested spelling.
    ///
    /// Errors:
    ///  - 204 (not joined): `error_code::not_found` /
    ///    `auth_error::not_joined`;
    ///  - 5xx or any other status: `error_code::unknown` /
    ///    `auth_error::remote_error`, with the status in the message;
    ///  - 200 with an invalid body: `error_code::parse_error` /
    ///    `auth_error::malformed_response`;
    ///  - transport failures: propagated unchanged (the transport contract
    ///    requires `make_auth_error()` values).
    [[nodiscard]] auto has_joined(std::string_view name,
                                  std::string_view server_hash,
                                  std::string_view remote_ip = {})
        -> result<profile>;

  private:
    [[nodiscard]] auto build_url(std::string_view name,
                                 std::string_view server_hash,
                                 std::string_view remote_ip) const
        -> result<std::string>;

    http_transport *transport_;
    auth_options options_;
};

/// Create the libcurl-backed HTTP transport.
///
/// The transport enforces TLS verification (peer and hostname), uses the
/// system trust store, refuses non-HTTPS URLs, disables redirects, applies the
/// request deadline, caps the response body at `max_session_response_bytes`
/// and never logs the URL or body. No credentials are stored.
///
/// Returns `unsupported` when this build was compiled without
/// `REFRAIN_WITH_CURL`. The returned transport is safe to share between
/// threads (each request uses a fresh easy handle).
[[nodiscard]] auto make_curl_transport()
    -> result<std::unique_ptr<http_transport>>;

} // namespace refrain::net
