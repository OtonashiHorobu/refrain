// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/signature.hpp>

#include <refrain/data/checksum.hpp>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::data {
namespace {

// The fixed sizes are part of the manifest signature format; fail the build
// if libsodium is ever built with different Ed25519 parameters.
static_assert(crypto_sign_SEEDBYTES == 32U);
static_assert(crypto_sign_PUBLICKEYBYTES == 32U);
static_assert(crypto_sign_SECRETKEYBYTES == 64U);
static_assert(crypto_sign_BYTES == 64U);

// `sodium_init()` is idempotent and process-wide; a function-local static is
// initialized exactly once even if several threads race on the first call
// (C++11 thread-safe static initialization). We cache the outcome so the
// result-returning entry points can report `not_initialized`.
[[nodiscard]] auto libsodium_ready() noexcept -> bool {
    static const bool ready{[] { return sodium_init() >= 0; }()};
    return ready;
}

[[nodiscard]] constexpr auto hex_nibble(char character) noexcept -> int {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

template <std::size_t size>
[[nodiscard]] auto decode_hex(std::string_view hex,
                              std::array<std::byte, size> &out,
                              std::string_view length_error,
                              std::string_view character_error) -> status {
    if (hex.size() != size * 2U) {
        return std::unexpected{
            make_error(error_code::invalid_argument, length_error)};
    }
    for (std::size_t index{0}; index < size; ++index) {
        const auto high{hex_nibble(hex[2U * index])};
        const auto low{hex_nibble(hex[2U * index + 1U])};
        if (high < 0 || low < 0) {
            return std::unexpected{
                make_error(error_code::invalid_argument, character_error)};
        }
        out[index] = static_cast<std::byte>((high << 4) | low);
    }
    return status_ok();
}

// libsodium's C API takes `unsigned char *`. `std::byte` is the byte type of
// the public API; accessing storage through `unsigned char` is always allowed.
[[nodiscard]] auto as_unsigned(std::byte *bytes) noexcept -> unsigned char * {
    return reinterpret_cast<unsigned char *>(bytes);
}

[[nodiscard]] auto as_unsigned(const std::byte *bytes) noexcept -> const
    unsigned char * {
    return reinterpret_cast<const unsigned char *>(bytes);
}

// `std::span<const std::byte>{}` may carry a null `data()`; libsodium's hash
// update must never receive a null pointer, so an empty message borrows the
// address of a zero byte instead.
[[nodiscard]] auto message_bytes(std::span<const std::byte> message) noexcept
    -> const unsigned char * {
    static constexpr unsigned char empty_message{0};
    return message.empty() ? &empty_message : as_unsigned(message.data());
}

} // namespace

ed25519_secret_key::~ed25519_secret_key() {
    // No live secret material may survive destruction of the key object
    // (`sodium_memzero` is a compiler-barrier-protected wipe and does not
    // depend on successful `sodium_init()`).
    sodium_memzero(bytes.data(), bytes.size());
}

auto initialize() -> status {
    if (!libsodium_ready()) {
        return std::unexpected{make_error(
            error_code::not_initialized,
            "libsodium initialization failed; Ed25519 primitives are "
            "unavailable")};
    }
    return status_ok();
}

auto parse_public_key_hex(std::string_view hex) -> result<ed25519_public_key> {
    ed25519_public_key key{};
    const auto decoded{decode_hex(
        hex, key.bytes,
        "Ed25519 public key must be 32 bytes (64 hexadecimal characters)",
        "Ed25519 public key must be hexadecimal characters only")};
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    return key;
}

auto parse_secret_key_hex(std::string_view hex) -> result<ed25519_secret_key> {
    ed25519_secret_key key{};
    const auto decoded{decode_hex(
        hex, key.bytes,
        "Ed25519 secret key must be 64 bytes (128 hexadecimal characters)",
        "Ed25519 secret key must be hexadecimal characters only")};
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }

    // libsodium stores a secret key as `seed || public_key`. Reject a pair
    // whose halves disagree: such a key can only ever produce signatures that
    // fail verification, so accepting it would defer the error to a confusing
    // later stage (and make "wrong key" tests look like crypto failures).
    ed25519_seed seed{};
    std::copy_n(key.bytes.begin(), seed.size(), seed.begin());
    const auto derived{keypair_from_seed(seed)};
    if (!derived) {
        return std::unexpected{derived.error()};
    }
    const auto public_half_offset{key.bytes.size() / 2U};
    if (!std::equal(derived->public_key.bytes.begin(),
                    derived->public_key.bytes.end(),
                    key.bytes.begin() +
                        static_cast<std::ptrdiff_t>(public_half_offset))) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "Ed25519 secret key public half does not match its seed")};
    }
    return key;
}

auto parse_signature_hex(std::string_view hex) -> result<signature> {
    signature sig{};
    const auto decoded{decode_hex(
        hex, sig,
        "Ed25519 signature must be 64 bytes (128 hexadecimal characters)",
        "Ed25519 signature must be hexadecimal characters only")};
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    return sig;
}

auto parse_seed_hex(std::string_view hex) -> result<ed25519_seed> {
    ed25519_seed seed{};
    const auto decoded{decode_hex(
        hex, seed, "Ed25519 seed must be 32 bytes (64 hexadecimal characters)",
        "Ed25519 seed must be hexadecimal characters only")};
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    return seed;
}

auto to_hex(const ed25519_public_key &key) -> std::string {
    return to_hex(std::span<const std::byte>{key.bytes});
}

auto to_hex(const ed25519_secret_key &key) -> std::string {
    return to_hex(std::span<const std::byte>{key.bytes});
}

auto to_hex(const signature &sig) -> std::string {
    return to_hex(std::span<const std::byte>{sig});
}

auto keypair_from_seed(const ed25519_seed &seed) -> result<ed25519_keypair> {
    if (!libsodium_ready()) {
        return std::unexpected{make_error(
            error_code::not_initialized,
            "libsodium initialization failed; cannot derive an Ed25519 "
            "keypair")};
    }
    ed25519_keypair pair{};
    if (crypto_sign_seed_keypair(as_unsigned(pair.public_key.bytes.data()),
                                 as_unsigned(pair.secret_key.bytes.data()),
                                 as_unsigned(seed.data())) != 0) {
        return std::unexpected{make_error(
            error_code::unknown,
            "libsodium failed to derive an Ed25519 keypair from the seed")};
    }
    return pair;
}

auto sign(std::span<const std::byte> message,
          const ed25519_secret_key &secret_key) -> signature {
    signature output{};
    if (!libsodium_ready()) {
        // Fail closed (documented in the header): an all-zero signature is not
        // valid for any message or key, so a host that skipped `initialize()`
        // cannot silently emit an accepted manifest signature.
        return output;
    }
    unsigned long long length{0};
    const auto result{crypto_sign_detached(
        as_unsigned(output.data()), &length, message_bytes(message),
        static_cast<unsigned long long>(message.size()),
        as_unsigned(secret_key.bytes.data()))};
    if (result != 0 || length != output.size()) {
        return signature{};
    }
    return output;
}

auto verify(std::span<const std::byte> message, const signature &sig,
            const ed25519_public_key &public_key) -> status {
    if (!libsodium_ready()) {
        return std::unexpected{make_error(
            error_code::not_initialized,
            "libsodium initialization failed; cannot verify an Ed25519 "
            "signature")};
    }
    const auto result{crypto_sign_verify_detached(
        as_unsigned(sig.data()), message_bytes(message),
        static_cast<unsigned long long>(message.size()),
        as_unsigned(public_key.bytes.data()))};
    if (result != 0) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "Ed25519 signature verification failed (wrong key, message or "
            "signature)")};
    }
    return status_ok();
}

} // namespace refrain::data
