// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace refrain::data {

// Ed25519 detached signatures for signed `.rfmod` manifests.
//
// The detached signature covers the canonical serialization of a mod's
// `manifest.toml` (`[signature] scheme = "ed25519"`); per-file integrity
// stays BLAKE3 (checksum.hpp). This header is the narrow crypto boundary:
// libsodium is an implementation detail of `src/data/signature.cpp`, so no
// `<sodium.h>` type, macro or include leaks into the public headers or the
// installed SDK.
//
// Key material uses libsodium's 64-byte secret-key layout, `seed[32] ||
// public_key[32]`: `keypair_from_seed()` writes it, `parse_secret_key_hex()`
// accepts it, and `parse_secret_key_hex()` re-derives the keypair to reject a
// seed/public-half mismatch (such a key would produce signatures that can
// never verify). The RFC 8032 "secret key" is the 32-byte seed half only; the
// full 128-hex form is what a Refrain signing tool stores/passes around.
//
// Determinism: Ed25519 signatures are deterministic (RFC 8032 §5.1.6), so
// signing the same message with the same key always yields the same 64 bytes.
// This keeps packaging reproducible and content-addressable.
//
// Secret-key zeroization: `ed25519_secret_key` wipes its bytes with
// `sodium_memzero()` in its destructor. Copies and moved-from objects each own
// their own storage and are likewise wiped on destruction, so no object leaves
// live secret material behind in freed memory. Zeroization is not observable
// through the API; `test/data/test_signature.cpp` pins the documented
// semantics (destination-independent copies + the value remaining usable until
// destruction) instead.
//
// No exceptions, no RTTI, errors are `refrain::result` values.

/// A 32-byte Ed25519 seed (the RFC 8032 "secret key").
using ed25519_seed = std::array<std::byte, 32>;

/// An Ed25519 public key (compressed Edwards-y point, 32 bytes).
struct ed25519_public_key {
    std::array<std::byte, 32> bytes{};
};

/// An Ed25519 secret key in libsodium's 64-byte layout
/// (`seed[32] || public_key[32]`). The destructor zeroizes `bytes`.
struct ed25519_secret_key {
    std::array<std::byte, 64> bytes{};

    ed25519_secret_key() = default;
    ed25519_secret_key(const ed25519_secret_key &) = default;
    ed25519_secret_key &operator=(const ed25519_secret_key &) = default;
    ed25519_secret_key(ed25519_secret_key &&) = default;
    ed25519_secret_key &operator=(ed25519_secret_key &&) = default;
    ~ed25519_secret_key();
};

/// A 64-byte detached Ed25519 signature (`R || S`, RFC 8032 §5.1.6).
using signature = std::array<std::byte, 64>;

/// A derived Ed25519 keypair. `secret_key` is wiped on destruction.
struct ed25519_keypair {
    ed25519_public_key public_key;
    ed25519_secret_key secret_key;
};

/// One-time process-wide libsodium initialization (`sodium_init()`).
///
/// The primitives below initialize libsodium lazily on first use, so calling
/// this eagerly is optional: it exists so a host (e.g. `refrain-tools mod
/// verify`) can fail fast and report the error through its own diagnostics.
/// Returns `not_initialized` when libsodium reports an initialization failure
/// (rare: unsupported CPU/OS); the deterministic Ed25519 primitives themselves
/// do not use the random generator or the secure allocator.
[[nodiscard]] auto initialize() -> status;

/// Parse a 64-character hex public key. Fails with `invalid_argument` for a
/// wrong length or non-hex characters (uppercase is accepted).
[[nodiscard]] auto parse_public_key_hex(std::string_view hex)
    -> result<ed25519_public_key>;

/// Parse a 128-character hex secret key (`seed || public_key`). Fails with
/// `invalid_argument` for a wrong length, non-hex characters, or a public half
/// that does not match the seed (garbage that would only ever produce
/// unverifiable signatures); `not_initialized` propagates from libsodium.
[[nodiscard]] auto parse_secret_key_hex(std::string_view hex)
    -> result<ed25519_secret_key>;

/// Parse a 128-character hex detached signature. Fails with
/// `invalid_argument` for a wrong length or non-hex characters.
[[nodiscard]] auto parse_signature_hex(std::string_view hex)
    -> result<signature>;

/// Parse a 64-character hex Ed25519 seed (the 32-byte RFC 8032 secret key).
///
/// This is the signing-tool input format: callers derive the keypair with
/// `keypair_from_seed()` instead of handling libsodium's 64-byte layout. Fails
/// with `invalid_argument` for a wrong length or non-hex characters; no
/// random key generation is provided (key material is always supplied by the
/// caller).
[[nodiscard]] auto parse_seed_hex(std::string_view hex) -> result<ed25519_seed>;

/// Lowercase hex (64 characters) of a public key.
[[nodiscard]] auto to_hex(const ed25519_public_key &key) -> std::string;

/// Lowercase hex (128 characters) of a secret key.
///
/// The result is live secret material; only use it for key storage/backup and
/// never log it.
[[nodiscard]] auto to_hex(const ed25519_secret_key &key) -> std::string;

/// Lowercase hex (128 characters) of a detached signature.
[[nodiscard]] auto to_hex(const signature &sig) -> std::string;

/// Derive a keypair from a 32-byte seed. Fails with `not_initialized` when
/// libsodium initialization failed; deriving itself cannot fail.
[[nodiscard]] auto keypair_from_seed(const ed25519_seed &seed)
    -> result<ed25519_keypair>;

/// Sign `message` with `secret_key` (deterministic, no allocation).
///
/// Returns an all-zero signature if libsodium initialization failed (fail
/// closed: `verify()` rejects it); check `initialize()`/`keypair_from_seed()`
/// first when that distinction matters. A valid secret key always produces a
/// verifiable signature.
[[nodiscard]] auto sign(std::span<const std::byte> message,
                        const ed25519_secret_key &secret_key) -> signature;

/// Verify a detached signature over `message` with `public_key`.
///
/// Returns `parse_error` for an invalid signature (wrong key, wrong message,
/// tampered signature bytes or a non-canonical point), and `not_initialized`
/// if libsodium could not initialize.
[[nodiscard]] auto verify(std::span<const std::byte> message,
                          const signature &sig,
                          const ed25519_public_key &public_key) -> status;

} // namespace refrain::data
