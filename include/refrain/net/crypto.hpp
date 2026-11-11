// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::net {

// Online-mode cryptography for the login sequence.
//
// Scope: the primitives the Java Edition login handshake needs - an RSA-1024
// keypair (the protocol fixes the modulus size), PKCS#1 v1.5 decryption of the
// client's shared secret and verify token, the AES-128-CFB8 stream cipher that
// encrypts every frame after the handshake, SHA-1/MD5 digests for the login
// hash and the offline player UUID, and secure zeroization of secret buffers.
//
// OpenSSL is an implementation detail of `src/net/crypto.cpp`: this header
// never includes an OpenSSL header, and the implementation is compiled only
// when the build defines `REFRAIN_WITH_OPENSSL`. Every entry point below stays
// available in an OpenSSL-free build; it reports `error_code::unsupported`
// instead. `crypto_available()` distinguishes the two builds at runtime (a
// compilation flag in this header would not reach consumers of the library).
//
// Error handling: no exceptions cross this API. Failures are `refrain::result`
// values with a stable `error_code`; allocation failure inside the standard
// library containers is the usual hard-OOM boundary and is not translated.
//
// Secret lifetime: `rsa_keypair` owns its private key and frees it on
// destruction; `aes128_cfb8` owns its two cipher states and frees them on
// destruction. OpenSSL cleanses key schedules when those objects are freed.
// Callers that copy secret bytes into ordinary buffers (shared secrets,
// decrypted tokens) remain responsible for wiping those buffers with
// `secure_zero()` once they are no longer needed.

/// 20-byte SHA-1 digest in network (big-endian) byte order.
using sha1_digest = std::array<std::byte, 20>;

/// 16-byte MD5 digest in network (big-endian) byte order.
using md5_digest = std::array<std::byte, 16>;

/// RSA modulus size mandated by the login protocol: the client sends a
/// 128-byte ciphertext block in the encryption response, so the key size is
/// part of the wire format. The key only protects one login exchange and is
/// never reused for other traffic.
inline constexpr std::size_t rsa_key_bits = 1024;

/// Size of one RSA-1024 ciphertext block (`rsa_key_bits / 8`).
inline constexpr std::size_t rsa_ciphertext_bytes = rsa_key_bits / 8U;

/// AES-128 key and IV length.
inline constexpr std::size_t aes128_key_bytes = 16;

/// Bytes per AES block; also the CFB8 feedback register size.
inline constexpr std::size_t aes_block_bytes = 16;

/// True when this build contains the OpenSSL EVP implementation.
///
/// False means every cryptographic operation in this header fails with
/// `error_code::unsupported`; `secure_zero()` still works because it does not
/// depend on a cryptographic library.
[[nodiscard]] auto crypto_available() noexcept -> bool;

/// Overwrite `bytes` with zero in a way the optimizer may not remove.
///
/// This is a best-effort wipe for caller-owned secret buffers. It does not
/// make a copy of the buffer, does not lock pages, and cannot guarantee that
/// bytes previously copied elsewhere (by the allocator, a container or the
/// operating system) are gone.
void secure_zero(std::span<std::byte> bytes) noexcept;

/// SHA-1 digest of the concatenation of the viewed bytes.
///
/// Returns `unsupported` when the build has no OpenSSL or the provider refuses
/// SHA-1 (for example a FIPS-only configuration); any other failure is
/// reported as `unknown`. SHA-1 is required by the login hash and is not used
/// as a collision-resistant primitive here.
[[nodiscard]] auto sha1(std::span<const std::byte> data) -> result<sha1_digest>;

/// Convenience overload for text input; the bytes of `text` are hashed as-is.
[[nodiscard]] auto sha1(std::string_view text) -> result<sha1_digest>;

/// MD5 digest, used only to derive the deterministic offline-mode player UUID
/// (Java `UUID.nameUUIDFromBytes("OfflinePlayer:" + name)`).
///
/// MD5 is not used as a security primitive anywhere else in the engine and
/// this helper must not be reused for authentication, signatures or integrity
/// checks. Returns `unsupported` when the build has no OpenSSL or the provider
/// refuses MD5.
[[nodiscard]] auto md5(std::span<const std::byte> data) -> result<md5_digest>;

/// Convenience overload for text input.
[[nodiscard]] auto md5(std::string_view text) -> result<md5_digest>;

/// Render bytes the way Java's `BigInteger(byte[]).toString(16)` does.
///
/// The input is read as a signed, big-endian two's complement integer: a
/// leading byte with bit 7 set yields a `'-'` prefix, the magnitude is printed
/// in lowercase hex without leading zeros, and an all-zero input becomes
/// `"0"`. Minecraft uses this non-standard rendering for the login hash. This
/// helper performs no cryptography and never fails.
[[nodiscard]] auto to_signed_hex(std::span<const std::byte> bytes)
    -> std::string;

/// Compute the Minecraft login hash:
/// `SHA-1(server_id || shared_secret || public_key_der)` rendered by
/// `to_signed_hex()`.
///
/// `server_id` is used as raw ISO-8859-1/ASCII bytes (the current protocol
/// sends an empty string); `public_key_der` is the X.509 SubjectPublicKeyInfo
/// encoding returned by `rsa_keypair::public_key_der()`. Returns `unsupported`
/// when SHA-1 is unavailable and `unknown` when SHA-1 fails. The result is the
/// `serverId` value sent to the session server; it is not secret, but it is
/// derived from the shared secret and must not be treated as an authenticator
/// on its own.
[[nodiscard]] auto minecraft_server_hash(
    std::string_view server_id, std::span<const std::byte> shared_secret,
    std::span<const std::byte> public_key_der) -> result<std::string>;

/// RSA-1024 keypair used by the login handshake.
///
/// The keypair is created by `generate()`; there is no default constructor and
/// the object is move-only, so a keypair cannot be accidentally duplicated.
/// `generate()` returns a fresh keypair on every call; the modulus size is
/// fixed by the protocol and is not configurable.
///
/// Lifetime: the object owns the private key. Destruction releases it and
/// lets OpenSSL cleanse the key material. A moved-from object is empty and
/// every operation on it fails with `not_initialized`; only `generate()`
/// produces a usable object.
///
/// Thread safety: after construction the object is immutable and
/// `public_key_der()`/`decrypt_pkcs1_v1_5()` may be called concurrently; each
/// decryption uses its own OpenSSL context.
class rsa_keypair {
  public:
    rsa_keypair(rsa_keypair &&) noexcept;
    auto operator=(rsa_keypair &&) noexcept -> rsa_keypair &;
    ~rsa_keypair();

    rsa_keypair(const rsa_keypair &) = delete;
    auto operator=(const rsa_keypair &) -> rsa_keypair & = delete;

    /// Generate a fresh RSA-1024 keypair.
    ///
    /// Fails with `unsupported` when the build has no OpenSSL or the provider
    /// refuses RSA key generation (for example a FIPS-only configuration that
    /// rejects 1024-bit keys), and with `unknown` on any other OpenSSL
    /// failure. Generation is comparatively expensive and belongs in server
    /// startup, not in a per-connection path.
    [[nodiscard]] static auto generate() -> result<rsa_keypair>;

    /// DER encoding of the public key (X.509 SubjectPublicKeyInfo).
    ///
    /// The returned vector is owned by the caller and contains no secret
    /// material. Fails with `not_initialized` for a moved-from object and
    /// `unknown` if OpenSSL cannot encode the key.
    [[nodiscard]] auto public_key_der() const -> result<std::vector<std::byte>>;

    /// Decrypt one RSA ciphertext block with PKCS#1 v1.5 padding.
    ///
    /// `ciphertext` must be exactly `rsa_ciphertext_bytes` (128) bytes; any
    /// other length fails with `invalid_argument` before OpenSSL is called.
    /// A well-formed block decrypts to at most 117 bytes (the protocol uses 16
    /// for the shared secret and 4 for the verify token). A moved-from object
    /// fails with `not_initialized`.
    ///
    /// Invalid padding is not a reliable failure signal: current OpenSSL
    /// providers implement implicit rejection and return a pseudo-random
    /// plaintext instead of an error for a malformed block. Callers must
    /// therefore validate the decrypted verify token and its expected length
    /// before using any other decrypted value; the returned bytes are never
    /// authenticated. When a provider reports failure instead of rejecting
    /// implicitly, the failure is a single generic `parse_error` whose message
    /// deliberately does not distinguish padding, length and modulus errors,
    /// so this API cannot be used as a padding oracle.
    [[nodiscard]] auto
    decrypt_pkcs1_v1_5(std::span<const std::byte> ciphertext) const
        -> result<std::vector<std::byte>>;

  private:
    struct impl;
    explicit rsa_keypair(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

/// Encrypt one block with an X.509 SubjectPublicKeyInfo RSA public key.
///
/// The protocol uses this on the client side: the shared secret and verify
/// token are RSA/PKCS#1 v1.5 encrypted with the server's public key. The
/// plaintext must be at most `rsa_ciphertext_bytes - 11` bytes.
[[nodiscard]] auto
rsa_encrypt_pkcs1_v1_5(std::span<const std::byte> public_key_der,
                       std::span<const std::byte> plaintext)
    -> result<std::vector<std::byte>>;

/// AES-128-CFB8 stream cipher for one encrypted connection.
///
/// The Java Edition protocol runs one AES/CFB8 cipher per direction and seeds
/// both with the shared secret as key and IV. This class mirrors that setup:
/// `create()` builds two independent stream states, one used by `encrypt()`
/// and one by `decrypt()`, both starting at the supplied IV. Calls on the same
/// direction continue that stream; arbitrary chunk boundaries are allowed and
/// produce the same bytes as one large call. Mixing directions does not
/// disturb either state.
///
/// CFB8 is a stream cipher without authentication: it hides packet contents
/// but does not detect modification. The protocol's framing/validation layer
/// remains responsible for rejecting malformed plaintext.
///
/// Lifetime: the object owns both cipher states and frees them on destruction.
/// It is move-only and not thread-safe (the OpenSSL contexts carry mutable
/// stream state); use one object per connection and do not interleave calls
/// from several threads. A moved-from object fails every operation with
/// `not_initialized`.
class aes128_cfb8 {
  public:
    aes128_cfb8(aes128_cfb8 &&) noexcept;
    auto operator=(aes128_cfb8 &&) noexcept -> aes128_cfb8 &;
    ~aes128_cfb8();

    aes128_cfb8(const aes128_cfb8 &) = delete;
    auto operator=(const aes128_cfb8 &) -> aes128_cfb8 & = delete;

    /// Create both stream states from a 16-byte key and 16-byte IV.
    ///
    /// Fails with `invalid_argument` when either span is not exactly
    /// `aes128_key_bytes` bytes, `unsupported` when the build has no OpenSSL,
    /// and `unknown` when OpenSSL cannot initialize the cipher.
    [[nodiscard]] static auto create(std::span<const std::byte> key,
                                     std::span<const std::byte> iv)
        -> result<aes128_cfb8>;

    /// Encrypt `plaintext` on the outbound stream; output length equals input
    /// length. An empty input is a successful no-op and does not advance the
    /// stream. Fails with `not_initialized` for a moved-from object and
    /// `unknown` on an OpenSSL failure.
    [[nodiscard]] auto encrypt(std::span<const std::byte> plaintext)
        -> result<std::vector<std::byte>>;

    /// Decrypt `ciphertext` on the inbound stream; output length equals input
    /// length. An empty input is a successful no-op and does not advance the
    /// stream. Fails with `not_initialized` for a moved-from object and
    /// `unknown` on an OpenSSL failure.
    [[nodiscard]] auto decrypt(std::span<const std::byte> ciphertext)
        -> result<std::vector<std::byte>>;

  private:
    struct impl;
    explicit aes128_cfb8(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::net
