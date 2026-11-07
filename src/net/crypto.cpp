// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// OpenSSL EVP implementation of the online-mode crypto primitives.
//
// The public header is OpenSSL-free. This translation unit is compiled in two
// modes:
//  - `REFRAIN_WITH_OPENSSL` set (the Conan/CMake wiring links OpenSSL and
//    defines the macro): every entry point below uses EVP and reports real
//    failures;
//  - the macro absent: the same entry points exist and fail with
//    `error_code::unsupported`, so consumers and tests stay compilable before
//    the dependency is wired. Only `secure_zero()` and `to_signed_hex()` keep
//    working because neither needs a cryptographic library.
//
// No algorithm is implemented here: key generation, decryption, the stream
// cipher and both digests are EVP calls. The few non-EVP calls are encoding
// helpers (`EVP_PKEY_get1_encoded_public_key`, `OPENSSL_free`,
// `OPENSSL_cleanse`, `ERR_clear_error`).

#include <refrain/net/crypto.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef REFRAIN_WITH_OPENSSL
#define REFRAIN_WITH_OPENSSL 0
#endif

#if REFRAIN_WITH_OPENSSL
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

namespace refrain::net {

namespace {

[[maybe_unused, nodiscard]] auto unsupported_error() -> error {
    return make_error(error_code::unsupported,
                      "OpenSSL support is not compiled in");
}

[[nodiscard]] auto not_initialized_error() -> error {
    return make_error(error_code::not_initialized,
                      "crypto object has no state (moved-from)");
}

#if REFRAIN_WITH_OPENSSL

[[nodiscard]] auto unknown_error(std::string_view message) -> error {
    return make_error(error_code::unknown, message);
}

[[nodiscard]] auto as_unsigned(const std::byte *bytes) noexcept -> const
    unsigned char * {
    return reinterpret_cast<const unsigned char *>(bytes);
}

[[nodiscard]] auto as_unsigned(std::byte *bytes) noexcept -> unsigned char * {
    return reinterpret_cast<unsigned char *>(bytes);
}

// Clear OpenSSL's thread-local error queue after a failed operation. The
// queue would otherwise accumulate stale entries that the next unrelated
// OpenSSL caller could observe.
void clear_openssl_errors() noexcept { ERR_clear_error(); }

[[nodiscard]] auto digest_bytes(const EVP_MD *algorithm,
                                std::span<const std::byte> data,
                                std::span<std::byte> output) -> status {
    if (algorithm == nullptr) {
        return std::unexpected{
            make_error(error_code::unsupported,
                       "digest algorithm is not available in this OpenSSL "
                       "provider")};
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
        EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!context) {
        return std::unexpected{
            unknown_error("could not allocate a digest context")};
    }
    if (EVP_DigestInit_ex(context.get(), algorithm, nullptr) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::unsupported,
                       "digest algorithm is not available in this OpenSSL "
                       "provider")};
    }
    if (!data.empty() &&
        EVP_DigestUpdate(context.get(), data.data(), data.size()) <= 0) {
        clear_openssl_errors();
        return std::unexpected{unknown_error("digest update failed")};
    }
    unsigned int length{0};
    if (EVP_DigestFinal_ex(context.get(), as_unsigned(output.data()),
                           &length) <= 0 ||
        length != output.size()) {
        clear_openssl_errors();
        return std::unexpected{unknown_error("digest finalization failed")};
    }
    return status_ok();
}

// CFB8 has no padding and the ciphertext length equals the plaintext length.
// The output buffer is over-allocated by one AES block because EVP's contract
// allows a block mode to write up to `input + block_size - 1` bytes; CFB8
// itself writes exactly `input` bytes, which is checked below.
[[nodiscard]] auto stream_bytes(EVP_CIPHER_CTX *context,
                                std::span<const std::byte> input,
                                bool encrypting)
    -> result<std::vector<std::byte>> {
    if (context == nullptr) {
        return std::unexpected{not_initialized_error()};
    }
    if (input.empty()) {
        return std::vector<std::byte>{};
    }
    if (input.size() >
        std::numeric_limits<std::size_t>::max() - aes_block_bytes) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "stream input length overflows the output buffer")};
    }
    std::vector<std::byte> output(input.size() + aes_block_bytes);
    std::size_t offset{0};
    while (offset < input.size()) {
        const auto remaining{input.size() - offset};
        const auto chunk{static_cast<int>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())))};
        int written{0};
        const int result =
            encrypting
                ? EVP_EncryptUpdate(
                      context, as_unsigned(output.data()) + offset, &written,
                      as_unsigned(input.data()) + offset, chunk)
                : EVP_DecryptUpdate(
                      context, as_unsigned(output.data()) + offset, &written,
                      as_unsigned(input.data()) + offset, chunk);
        if (result <= 0 || written != chunk) {
            clear_openssl_errors();
            return std::unexpected{
                unknown_error("AES-CFB8 stream operation failed")};
        }
        offset += static_cast<std::size_t>(chunk);
    }
    output.resize(input.size());
    return output;
}

#endif // REFRAIN_WITH_OPENSSL

} // namespace

auto crypto_available() noexcept -> bool {
#if REFRAIN_WITH_OPENSSL
    return true;
#else
    return false;
#endif
}

void secure_zero(std::span<std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
#if REFRAIN_WITH_OPENSSL
    OPENSSL_cleanse(bytes.data(), bytes.size());
#else
    // A volatile store loop is the portable fallback; unlike `std::fill` the
    // optimizer may not prove the dead stores unnecessary.
    volatile std::byte *cursor = bytes.data();
    for (std::size_t index{0}; index < bytes.size(); ++index) {
        cursor[index] = std::byte{0};
    }
#endif
}

auto to_signed_hex(std::span<const std::byte> bytes) -> std::string {
    if (bytes.empty()) {
        return "0";
    }
    const bool negative{
        (std::to_integer<unsigned int>(bytes.front()) & 0x80U) != 0U};

    std::vector<unsigned char> magnitude(bytes.size());
    if (!negative) {
        for (std::size_t index{0}; index < bytes.size(); ++index) {
            magnitude[index] = std::to_integer<unsigned char>(bytes[index]);
        }
    } else {
        // Magnitude of a negative two's complement value: invert and add one.
        unsigned int carry{1};
        for (std::size_t index{bytes.size()}; index-- > 0;) {
            const auto inverted{
                static_cast<unsigned int>(
                    std::to_integer<unsigned char>(bytes[index])) ^
                0xFFU};
            const auto sum{inverted + carry};
            magnitude[index] = static_cast<unsigned char>(sum & 0xFFU);
            carry = sum >> 8U;
        }
    }

    std::size_t first{0};
    while (first < magnitude.size() && magnitude[first] == 0U) {
        ++first;
    }
    if (first == magnitude.size()) {
        return "0";
    }

    constexpr char digits[]{"0123456789abcdef"};
    std::string text;
    text.reserve((magnitude.size() - first) * 2U + (negative ? 1U : 0U));
    if (negative) {
        text.push_back('-');
    }
    const auto leading{magnitude[first]};
    if ((leading >> 4U) != 0U) {
        text.push_back(digits[leading >> 4U]);
    }
    text.push_back(digits[leading & 0x0FU]);
    for (std::size_t index{first + 1U}; index < magnitude.size(); ++index) {
        text.push_back(digits[magnitude[index] >> 4U]);
        text.push_back(digits[magnitude[index] & 0x0FU]);
    }
    return text;
}

auto sha1(std::span<const std::byte> data) -> result<sha1_digest> {
#if REFRAIN_WITH_OPENSSL
    sha1_digest digest{};
    if (auto hashed{digest_bytes(EVP_sha1(), data, digest)}; !hashed) {
        return std::unexpected{hashed.error()};
    }
    return digest;
#else
    (void)data;
    return std::unexpected{unsupported_error()};
#endif
}

auto sha1(std::string_view text) -> result<sha1_digest> {
    return sha1(std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

auto md5(std::span<const std::byte> data) -> result<md5_digest> {
#if REFRAIN_WITH_OPENSSL
    md5_digest digest{};
    if (auto hashed{digest_bytes(EVP_md5(), data, digest)}; !hashed) {
        return std::unexpected{hashed.error()};
    }
    return digest;
#else
    (void)data;
    return std::unexpected{unsupported_error()};
#endif
}

auto md5(std::string_view text) -> result<md5_digest> {
    return md5(std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

auto minecraft_server_hash(std::string_view server_id,
                           std::span<const std::byte> shared_secret,
                           std::span<const std::byte> public_key_der)
    -> result<std::string> {
    std::vector<std::byte> material;
    material.reserve(server_id.size() + shared_secret.size() +
                     public_key_der.size());
    const auto append = [&material](std::span<const std::byte> bytes) {
        material.insert(material.end(), bytes.begin(), bytes.end());
    };
    append(std::as_bytes(
        std::span<const char>{server_id.data(), server_id.size()}));
    append(shared_secret);
    append(public_key_der);

    auto digest{sha1(std::span<const std::byte>{material})};
    secure_zero(material);
    if (!digest) {
        return std::unexpected{digest.error()};
    }
    return to_signed_hex(*digest);
}

struct rsa_keypair::impl {
#if REFRAIN_WITH_OPENSSL
    EVP_PKEY *key{nullptr};

    impl() = default;
    impl(const impl &) = delete;
    auto operator=(const impl &) -> impl & = delete;
    ~impl() {
        if (key != nullptr) {
            EVP_PKEY_free(key);
        }
    }
#endif
};

rsa_keypair::rsa_keypair(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

rsa_keypair::rsa_keypair(rsa_keypair &&other) noexcept
    : impl_{std::move(other.impl_)} {}

auto rsa_keypair::operator=(rsa_keypair &&other) noexcept -> rsa_keypair & {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

rsa_keypair::~rsa_keypair() = default;

auto rsa_keypair::generate() -> result<rsa_keypair> {
#if REFRAIN_WITH_OPENSSL
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr),
        &EVP_PKEY_CTX_free};
    if (!context) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::unsupported,
                       "RSA key generation is not available in this OpenSSL "
                       "provider")};
    }
    if (EVP_PKEY_keygen_init(context.get()) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::unsupported,
                       "RSA key generation is not available in this OpenSSL "
                       "provider")};
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(),
                                         static_cast<int>(rsa_key_bits)) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::unsupported,
                       "this OpenSSL provider refuses RSA-1024 keys")};
    }

    auto state{std::make_unique<impl>()};
    if (EVP_PKEY_keygen(context.get(), &state->key) <= 0) {
        clear_openssl_errors();
        return std::unexpected{unknown_error("RSA-1024 key generation failed")};
    }
    return rsa_keypair{std::move(state)};
#else
    return std::unexpected{unsupported_error()};
#endif
}

auto rsa_keypair::public_key_der() const -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{not_initialized_error()};
    }
#if REFRAIN_WITH_OPENSSL
    // Java's `PublicKey.getEncoded()` is X.509 SubjectPublicKeyInfo;
    // `i2d_PUBKEY` produces exactly that encoding. The provider-specific
    // `EVP_PKEY_get1_encoded_public_key` is not guaranteed to do so (and some
    // providers return no data for RSA keys).
    unsigned char *encoded{nullptr};
    const auto length{i2d_PUBKEY(impl_->key, &encoded)};
    if (length <= 0 || encoded == nullptr) {
        if (encoded != nullptr) {
            OPENSSL_free(encoded);
        }
        clear_openssl_errors();
        return std::unexpected{
            unknown_error("could not encode the RSA public key")};
    }
    std::vector<std::byte> der(static_cast<std::size_t>(length));
    std::memcpy(der.data(), encoded, static_cast<std::size_t>(length));
    OPENSSL_free(encoded);
    return der;
#else
    return std::unexpected{unsupported_error()};
#endif
}

auto rsa_keypair::decrypt_pkcs1_v1_5(std::span<const std::byte> ciphertext)
    const -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{not_initialized_error()};
    }
    if (ciphertext.size() != rsa_ciphertext_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "RSA ciphertext must be exactly 128 bytes for a 1024-bit key")};
    }
#if REFRAIN_WITH_OPENSSL
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context{
        EVP_PKEY_CTX_new_from_pkey(nullptr, impl_->key, nullptr),
        &EVP_PKEY_CTX_free};
    if (!context) {
        clear_openssl_errors();
        return std::unexpected{
            unknown_error("could not create an RSA decryption context")};
    }
    if (EVP_PKEY_decrypt_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_PADDING) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            unknown_error("could not initialize RSA decryption")};
    }

    // Ask for the maximum plaintext size first, then decrypt into a buffer of
    // that size. On failure the message stays generic: distinguishing padding,
    // length and modulus errors would expose a padding oracle in providers
    // that do not implement implicit rejection (OpenSSL 3.2+ providers do, and
    // then a malformed block decrypts to a pseudo-random message instead of
    // failing; the caller must validate the verify token either way).
    std::size_t output_length{0};
    if (EVP_PKEY_decrypt(context.get(), nullptr, &output_length,
                         as_unsigned(ciphertext.data()),
                         ciphertext.size()) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::parse_error, "RSA decryption failed")};
    }
    std::vector<std::byte> plaintext(output_length);
    if (EVP_PKEY_decrypt(context.get(), as_unsigned(plaintext.data()),
                         &output_length, as_unsigned(ciphertext.data()),
                         ciphertext.size()) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::parse_error, "RSA decryption failed")};
    }
    plaintext.resize(output_length);
    return plaintext;
#else
    return std::unexpected{unsupported_error()};
#endif
}

struct aes128_cfb8::impl {
#if REFRAIN_WITH_OPENSSL
    EVP_CIPHER_CTX *encrypt{nullptr};
    EVP_CIPHER_CTX *decrypt{nullptr};

    impl() = default;
    impl(const impl &) = delete;
    auto operator=(const impl &) -> impl & = delete;
    ~impl() {
        if (encrypt != nullptr) {
            EVP_CIPHER_CTX_free(encrypt);
        }
        if (decrypt != nullptr) {
            EVP_CIPHER_CTX_free(decrypt);
        }
    }
#endif
};

aes128_cfb8::aes128_cfb8(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

aes128_cfb8::aes128_cfb8(aes128_cfb8 &&other) noexcept
    : impl_{std::move(other.impl_)} {}

auto aes128_cfb8::operator=(aes128_cfb8 &&other) noexcept -> aes128_cfb8 & {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

aes128_cfb8::~aes128_cfb8() = default;

auto aes128_cfb8::create(std::span<const std::byte> key,
                         std::span<const std::byte> iv) -> result<aes128_cfb8> {
    if (key.size() != aes128_key_bytes || iv.size() != aes128_key_bytes) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "AES-128 requires a 16-byte key and a 16-byte IV")};
    }
#if REFRAIN_WITH_OPENSSL
    const EVP_CIPHER *cipher{EVP_aes_128_cfb8()};
    if (cipher == nullptr) {
        clear_openssl_errors();
        return std::unexpected{
            make_error(error_code::unsupported,
                       "AES-128-CFB8 is not available in this OpenSSL "
                       "provider")};
    }
    auto state{std::make_unique<impl>()};
    state->encrypt = EVP_CIPHER_CTX_new();
    state->decrypt = EVP_CIPHER_CTX_new();
    if (state->encrypt == nullptr || state->decrypt == nullptr) {
        clear_openssl_errors();
        return std::unexpected{
            unknown_error("could not allocate an AES cipher context")};
    }
    if (EVP_EncryptInit_ex(state->encrypt, cipher, nullptr,
                           as_unsigned(key.data()),
                           as_unsigned(iv.data())) <= 0 ||
        EVP_DecryptInit_ex(state->decrypt, cipher, nullptr,
                           as_unsigned(key.data()),
                           as_unsigned(iv.data())) <= 0 ||
        EVP_CIPHER_CTX_set_padding(state->encrypt, 0) <= 0 ||
        EVP_CIPHER_CTX_set_padding(state->decrypt, 0) <= 0) {
        clear_openssl_errors();
        return std::unexpected{
            unknown_error("could not initialize AES-128-CFB8")};
    }
    return aes128_cfb8{std::move(state)};
#else
    (void)key;
    (void)iv;
    return std::unexpected{unsupported_error()};
#endif
}

auto aes128_cfb8::encrypt(std::span<const std::byte> plaintext)
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{not_initialized_error()};
    }
#if REFRAIN_WITH_OPENSSL
    return stream_bytes(impl_->encrypt, plaintext, true);
#else
    (void)plaintext;
    return std::unexpected{unsupported_error()};
#endif
}

auto aes128_cfb8::decrypt(std::span<const std::byte> ciphertext)
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected{not_initialized_error()};
    }
#if REFRAIN_WITH_OPENSSL
    return stream_bytes(impl_->decrypt, ciphertext, false);
#else
    (void)ciphertext;
    return std::unexpected{unsupported_error()};
#endif
}

} // namespace refrain::net
