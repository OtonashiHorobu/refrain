// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/crypto.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

[[maybe_unused, nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t index{0}; index < text.size(); ++index) {
        bytes[index] = static_cast<std::byte>(
            static_cast<unsigned char>(text[index]));
    }
    return bytes;
}

[[nodiscard]] auto hex_of(std::span<const std::byte> bytes) -> std::string {
    constexpr char digits[]{"0123456789abcdef"};
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        const auto value{std::to_integer<unsigned int>(byte)};
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0FU]);
    }
    return text;
}

template <std::size_t size>
[[nodiscard]] auto hex_of(const std::array<std::byte, size> &bytes)
    -> std::string {
    return hex_of(std::span<const std::byte>{bytes.data(), bytes.size()});
}

[[nodiscard]] auto from_hex(std::string_view hex) -> std::vector<std::byte> {
    const auto nibble = [](char character) -> int {
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
    };
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2U);
    for (std::size_t index{0}; index + 1U < hex.size(); index += 2U) {
        const int high{nibble(hex[index])};
        const int low{nibble(hex[index + 1U])};
        if (high < 0 || low < 0) {
            return {};
        }
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned int>(high << 4) |
                                   static_cast<unsigned int>(low)));
    }
    return bytes;
}

[[nodiscard]] auto hex_bytes(std::string_view hex) -> std::vector<std::byte> {
    return from_hex(hex);
}

[[nodiscard]] auto join(const std::vector<std::byte> &first,
                        const std::vector<std::byte> &second)
    -> std::vector<std::byte> {
    std::vector<std::byte> joined{first};
    joined.insert(joined.end(), second.begin(), second.end());
    return joined;
}

const suite tests = [] {
    "crypto/secure zero clears the whole buffer"_test = [] {
        std::array<std::byte, 5> secret{
            std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
            std::byte{0x78}, std::byte{0x9A}};
        net::secure_zero(std::span<std::byte>{secret});
        for (const auto byte : secret) {
            expect(byte == std::byte{0});
        }
        net::secure_zero(std::span<std::byte>{}); // empty span is a no-op
    };

    "crypto/signed hex matches Java BigInteger rendering"_test = [] {
        expect(net::to_signed_hex(std::span<const std::byte>{}) == "0");
        expect(net::to_signed_hex(hex_bytes("00")) == "0");
        expect(net::to_signed_hex(hex_bytes("000a")) == "a");
        expect(net::to_signed_hex(hex_bytes("ff")) == "-1");
        expect(net::to_signed_hex(hex_bytes("8000")) == "-8000");
        expect(net::to_signed_hex(hex_bytes("0100")) == "100");
    };

    "crypto/minecraft login hash matches the published digest examples"_test =
        [] {
            if (!net::crypto_available()) {
                return;
            }
            // SHA-1("Notch") / SHA-1("jeb_") / SHA-1("simon"), including the
            // negative and leading-zero cases of the Java rendering.
            for (const auto &[text, expected] :
                 {std::pair<std::string_view, std::string_view>{
                      "Notch", "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48"},
                  {"jeb_", "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1"},
                  {"simon", "88e16a1019277b15d58faf0541e11910eb756f6"}}) {
                const auto hash{net::minecraft_server_hash(text, {}, {})};
                expect(static_cast<bool>(hash)) << "hash failed for " << text;
                if (hash) {
                    expect(*hash == expected) << "wrong hash for " << text;
                }
            }
        };

    "crypto/sha1 matches the FIPS test vectors"_test = [] {
        if (!net::crypto_available()) {
            return;
        }
        const auto empty{net::sha1(std::string_view{})};
        expect(empty.has_value());
        if (empty) {
            expect(hex_of(*empty) ==
                   "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        }
        const auto abc{net::sha1("abc")};
        expect(abc.has_value());
        if (abc) {
            expect(hex_of(*abc) ==
                   "a9993e364706816aba3e25717850c26c9cd0d89d");
        }
        const auto text{net::sha1("The quick brown fox jumps over the lazy "
                                  "dog")};
        expect(text.has_value());
        if (text) {
            expect(hex_of(*text) ==
                   "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
        }
    };

    "crypto/md5 matches the RFC 1321 test vector"_test = [] {
        if (!net::crypto_available()) {
            return;
        }
        const auto digest{net::md5("abc")};
        expect(digest.has_value());
        if (digest) {
            expect(hex_of(*digest) == "900150983cd24fb0d6963f7d28e17f72");
        }
    };

    "crypto/digests fail with unsupported when no backend is built"_test = [] {
        if (net::crypto_available()) {
            return;
        }
        const auto sha{net::sha1("abc")};
        expect(!sha.has_value());
        if (!sha) {
            expect(sha.error().code == refrain::error_code::unsupported);
        }
        const auto digest{net::md5("abc")};
        expect(!digest.has_value());
        if (!digest) {
            expect(digest.error().code == refrain::error_code::unsupported);
        }
    };

    "crypto/aes128 cfb8 matches the NIST multi-block vectors"_test = [] {
        const auto key{hex_bytes("280cf81af5cc7e7363579c1da03390e6")};
        const auto iv{hex_bytes("5d6cf4722d0e21f1d9ced53a0e36c342")};
        const auto plaintext{hex_bytes("b2a22ced")};
        const auto ciphertext{hex_bytes("73f3aebf")};
        auto cipher{net::aes128_cfb8::create(key, iv)};
        if (!net::crypto_available()) {
            expect(!cipher.has_value());
            if (!cipher) {
                expect(cipher.error().code ==
                       refrain::error_code::unsupported);
            }
            return;
        }
        expect(cipher.has_value());
        if (!cipher) {
            return;
        }
        const auto encrypted{cipher->encrypt(plaintext)};
        expect(encrypted.has_value());
        if (encrypted) {
            expect(*encrypted == ciphertext);
        }
        const auto decrypted{cipher->decrypt(ciphertext)};
        expect(decrypted.has_value());
        if (decrypted) {
            expect(*decrypted == plaintext);
        }
    };

    "crypto/aes128 cfb8 keeps stream state across chunk boundaries"_test = [] {
        const auto key{hex_bytes("d01da95d2c2a61da06ea78cfba59cc30")};
        const auto iv{hex_bytes("f9a393ad90814faf262e3a5b1d97592e")};
        const auto plaintext{hex_bytes("57c1a30e48166d9640")};
        const auto ciphertext{hex_bytes("e9a8c3b776edd39e3d")};
        if (!net::crypto_available()) {
            return;
        }
        auto cipher{net::aes128_cfb8::create(key, iv)};
        expect(cipher.has_value());
        if (!cipher) {
            return;
        }
        const auto first{cipher->encrypt(
            std::span<const std::byte>{plaintext.data(), 1U})};
        const auto second{cipher->encrypt(std::span<const std::byte>{
            plaintext.data() + 1, plaintext.size() - 1U})};
        expect(first.has_value());
        expect(second.has_value());
        if (first && second) {
            expect(join(*first, *second) == ciphertext);
        }

        // The decrypt stream is independent of the encrypt stream, so it
        // still starts at the IV even after the encrypt calls above.
        const auto first_plain{cipher->decrypt(
            std::span<const std::byte>{ciphertext.data(), 4U})};
        const auto second_plain{cipher->decrypt(std::span<const std::byte>{
            ciphertext.data() + 4, ciphertext.size() - 4U})};
        expect(first_plain.has_value());
        expect(second_plain.has_value());
        if (first_plain && second_plain) {
            expect(join(*first_plain, *second_plain) == plaintext);
        }
    };

    "crypto/aes128 rejects wrong key or IV sizes"_test = [] {
        for (const auto &[key_size, iv_size] :
             {std::pair<std::size_t, std::size_t>{15U, 16U},
              {16U, 15U}, {0U, 0U}, {17U, 16U}}) {
            const std::vector<std::byte> key(key_size);
            const std::vector<std::byte> iv(iv_size);
            const auto cipher{net::aes128_cfb8::create(key, iv)};
            expect(!cipher.has_value());
            if (!cipher) {
                expect(cipher.error().code ==
                       refrain::error_code::invalid_argument);
            }
        }
    };

    "crypto/rsa generates a DER public key and rejects bad block sizes"_test =
        [] {
            if (!net::crypto_available()) {
                const auto key{net::rsa_keypair::generate()};
                expect(!key.has_value());
                if (!key) {
                    expect(key.error().code ==
                           refrain::error_code::unsupported);
                }
                return;
            }
            auto key{net::rsa_keypair::generate()};
            expect(key.has_value());
            if (!key) {
                return;
            }
            const auto der{key->public_key_der()};
            expect(der.has_value());
            if (der) {
                expect(!der->empty());
                // X.509 SubjectPublicKeyInfo is a DER SEQUENCE.
                expect((*der)[0] == std::byte{0x30});
            }

            std::vector<std::byte> short_block(net::rsa_ciphertext_bytes - 1U);
            const auto too_short{key->decrypt_pkcs1_v1_5(short_block)};
            expect(!too_short.has_value());
            if (!too_short) {
                expect(too_short.error().code ==
                       refrain::error_code::invalid_argument);
            }

            // OpenSSL 3.2+ providers implement implicit rejection, so a
            // malformed block can either fail generically or decrypt to an
            // unauthenticated pseudo-random message. Neither outcome may
            // claim a padded 128-byte block.
            std::vector<std::byte> garbage(net::rsa_ciphertext_bytes);
            const auto decrypted{key->decrypt_pkcs1_v1_5(garbage)};
            if (!decrypted) {
                expect(decrypted.error().code ==
                       refrain::error_code::parse_error);
                expect(decrypted.error().message == "RSA decryption failed");
            } else {
                expect(decrypted->size() <= 117U);
            }
        };

    "crypto/rsa and aes report unsupported without a backend"_test = [] {
        if (net::crypto_available()) {
            return;
        }
        const auto key{net::rsa_keypair::generate()};
        expect(!key.has_value());
        if (!key) {
            expect(key.error().code == refrain::error_code::unsupported);
        }
        std::array<std::byte, net::aes128_key_bytes> key_bytes{};
        const auto cipher{net::aes128_cfb8::create(key_bytes, key_bytes)};
        expect(!cipher.has_value());
        if (!cipher) {
            expect(cipher.error().code == refrain::error_code::unsupported);
        }
    };
};

} // namespace
