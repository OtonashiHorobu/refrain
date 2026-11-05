// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/signature.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::data::ed25519_keypair;
using refrain::data::ed25519_public_key;
using refrain::data::ed25519_secret_key;
using refrain::data::ed25519_seed;
using refrain::data::initialize;
using refrain::data::keypair_from_seed;
using refrain::data::parse_public_key_hex;
using refrain::data::parse_secret_key_hex;
using refrain::data::parse_signature_hex;
using refrain::data::sign;
using refrain::data::signature;
using refrain::data::to_hex;
using refrain::data::verify;

// RFC 8032 §7.1 test vectors (TEST 1, TEST 2 and TEST 3). The RFC only lists
// the 32-byte seed as "SECRET KEY"; libsodium's 64-byte secret-key layout is
// `seed || public_key`, which is what the `secret_key_hex` column holds.
struct rfc8032_vector {
    std::string_view secret_key_hex;
    std::string_view public_key_hex;
    std::string_view message_hex;
    std::string_view signature_hex;
};

constexpr std::array<rfc8032_vector, 3> rfc8032_vectors{{
    {.secret_key_hex =
         "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     .public_key_hex =
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     .message_hex = "",
     .signature_hex =
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
         "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {.secret_key_hex =
         "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     .public_key_hex =
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     .message_hex = "72",
     .signature_hex =
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {.secret_key_hex =
         "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7"
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     .public_key_hex =
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     .message_hex = "af82",
     .signature_hex =
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
}};

[[nodiscard]] auto nibble(char character) -> int {
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

/// Decode a hex string whose length is known even at test-data review time.
[[nodiscard]] auto hex_bytes(std::string_view hex) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(hex.size() / 2U);
    for (std::size_t index{0}; index < bytes.size(); ++index) {
        const auto high{nibble(hex[2U * index])};
        const auto low{nibble(hex[2U * index + 1U])};
        bytes[index] = static_cast<std::byte>((high << 4) | low);
    }
    return bytes;
}

/// ASCII/UTF-8 text as raw bytes, for message-building convenience.
[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto seed_of(const ed25519_secret_key &secret) -> ed25519_seed {
    ed25519_seed seed{};
    std::copy_n(secret.bytes.begin(), seed.size(), seed.begin());
    return seed;
}

[[nodiscard]] auto uppercase(std::string_view text) -> std::string {
    std::string upper{text};
    for (auto &character : upper) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return upper;
}

template <typename T>
[[nodiscard]] auto error_code_of(const refrain::result<T> &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

[[nodiscard]] auto parse_error() -> int {
    return static_cast<int>(refrain::error_code::parse_error);
}

[[nodiscard]] auto invalid_argument() -> int {
    return static_cast<int>(refrain::error_code::invalid_argument);
}

const suite signature_tests = [] {
    "signature/initialize is idempotent and succeeds"_test = [] {
        expect(initialize().has_value());
        // A second call must be a cached no-op (`sodium_init()` is idempotent).
        expect(initialize().has_value());
    };

    "signature/RFC 8032 TEST 1-3 derive, sign and verify"_test = [] {
        for (const auto &vector : rfc8032_vectors) {
            const auto secret{parse_secret_key_hex(vector.secret_key_hex)};
            expect(static_cast<bool>(secret)) << "RFC secret key parses";
            if (!secret) {
                continue;
            }
            const auto pair{keypair_from_seed(seed_of(*secret))};
            expect(static_cast<bool>(pair)) << "seed derives a keypair";
            if (!pair) {
                continue;
            }
            expect(to_hex(pair->public_key) == vector.public_key_hex);
            expect(to_hex(pair->secret_key) == vector.secret_key_hex);

            const auto message{hex_bytes(vector.message_hex)};
            const auto sig{sign(message, *secret)};
            expect(to_hex(sig) == vector.signature_hex);
            expect(verify(message, sig, pair->public_key).has_value());
        }
    };

    "signature/RFC 8032 signatures verify through the parsed-hex path"_test =
        [] {
            for (const auto &vector : rfc8032_vectors) {
                const auto public_key{
                    parse_public_key_hex(vector.public_key_hex)};
                const auto sig{parse_signature_hex(vector.signature_hex)};
                expect(public_key.has_value() && sig.has_value());
                if (!public_key || !sig) {
                    continue;
                }
                expect(to_hex(*public_key) == vector.public_key_hex);
                expect(to_hex(*sig) == vector.signature_hex);
                const auto message{hex_bytes(vector.message_hex)};
                expect(verify(message, *sig, *public_key).has_value());
            }
        };

    "signature/sign is deterministic"_test = [] {
        const auto secret{
            parse_secret_key_hex(rfc8032_vectors[1].secret_key_hex)};
        expect(static_cast<bool>(secret));
        if (!secret) {
            return;
        }
        const auto message{hex_bytes(rfc8032_vectors[1].message_hex)};
        const signature first{sign(message, *secret)};
        const signature second{sign(message, *secret)};
        expect(first == second);
        expect(to_hex(first) == rfc8032_vectors[1].signature_hex);

        // Any different message must change the signature (append a byte so
        // the test never indexes an empty buffer).
        auto changed{message};
        changed.push_back(std::byte{0x00});
        expect(sign(changed, *secret) != first);
    };

    "signature/verify rejects a tampered or wrong message"_test = [] {
        for (const auto &vector : rfc8032_vectors) {
            const auto public_key{parse_public_key_hex(vector.public_key_hex)};
            const auto sig{parse_signature_hex(vector.signature_hex)};
            expect(public_key.has_value() && sig.has_value());
            if (!public_key || !sig) {
                continue;
            }
            auto message{hex_bytes(vector.message_hex)};
            expect(verify(message, *sig, *public_key).has_value());

            // Appending a byte is always a different message, including TEST 1
            // whose message is empty.
            message.push_back(std::byte{0x00});
            expect(error_code_of(verify(message, *sig, *public_key)) ==
                   parse_error());
            message.pop_back();

            // Flipping an existing byte (when present) must also fail.
            if (!message.empty()) {
                message[0] ^= std::byte{0x01};
                expect(error_code_of(verify(message, *sig, *public_key)) ==
                       parse_error());
                message[0] ^= std::byte{0x01};
                expect(verify(message, *sig, *public_key).has_value());
            }
        }
    };

    "signature/verify rejects a tampered signature"_test = [] {
        for (const auto &vector : rfc8032_vectors) {
            const auto public_key{parse_public_key_hex(vector.public_key_hex)};
            const auto parsed{parse_signature_hex(vector.signature_hex)};
            expect(public_key.has_value() && parsed.has_value());
            if (!public_key || !parsed) {
                continue;
            }
            const auto message{hex_bytes(vector.message_hex)};
            for (const std::size_t index : {0U, 32U, 63U}) {
                auto tampered{*parsed};
                tampered[index] ^= std::byte{0x01};
                expect(error_code_of(verify(message, tampered, *public_key)) ==
                       parse_error())
                    << "flipped signature byte " << index;
            }
            // The unmodified signature still verifies afterwards.
            expect(verify(message, *parsed, *public_key).has_value());
        }
    };

    "signature/verify rejects the wrong public key"_test = [] {
        const auto message{hex_bytes(rfc8032_vectors[1].message_hex)};
        const auto sig{parse_signature_hex(rfc8032_vectors[1].signature_hex)};
        const auto other_key{
            parse_public_key_hex(rfc8032_vectors[2].public_key_hex)};
        expect(sig.has_value() && other_key.has_value());
        if (!sig || !other_key) {
            return;
        }
        expect(error_code_of(verify(message, *sig, *other_key)) ==
               parse_error());

        const ed25519_public_key zero_key{};
        expect(error_code_of(verify(message, *sig, zero_key)) == parse_error());
    };

    "signature/verify rejects an all-zero signature"_test = [] {
        const auto public_key{
            parse_public_key_hex(rfc8032_vectors[0].public_key_hex)};
        expect(static_cast<bool>(public_key));
        if (!public_key) {
            return;
        }
        const signature zero_signature{};
        const auto message{hex_bytes(rfc8032_vectors[0].message_hex)};
        expect(error_code_of(verify(message, zero_signature, *public_key)) ==
               parse_error());
    };

    "signature/parse helpers reject malformed hex and wrong lengths"_test = [] {
        expect(error_code_of(parse_public_key_hex("")) == invalid_argument());
        expect(error_code_of(parse_public_key_hex(std::string(63, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_public_key_hex(std::string(65, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_public_key_hex(std::string(64, 'g'))) ==
               invalid_argument());

        expect(error_code_of(parse_secret_key_hex("")) == invalid_argument());
        expect(error_code_of(parse_secret_key_hex(std::string(127, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_secret_key_hex(std::string(129, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_secret_key_hex(std::string(128, 'z'))) ==
               invalid_argument());

        expect(error_code_of(parse_signature_hex("")) == invalid_argument());
        expect(error_code_of(parse_signature_hex(std::string(126, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_signature_hex(std::string(130, 'a'))) ==
               invalid_argument());
        expect(error_code_of(parse_signature_hex(std::string(128, ' '))) ==
               invalid_argument());
    };

    "signature/parse_secret_key_hex rejects a mismatched public half"_test =
        [] {
            // Sanity: the untouched RFC key is accepted.
            expect(parse_secret_key_hex(rfc8032_vectors[0].secret_key_hex)
                       .has_value());

            // Mutating any hex digit of the public half makes seed/pk disagree;
            // the result must be rejected instead of producing unverifiable
            // signatures.
            std::string mismatched{rfc8032_vectors[0].secret_key_hex};
            expect(!mismatched.empty());
            if (!mismatched.empty()) {
                const auto last{mismatched.size() - 1U};
                mismatched[last] = mismatched[last] == 'a' ? 'b' : 'a';
            }
            expect(error_code_of(parse_secret_key_hex(mismatched)) ==
                   invalid_argument());

            std::string middle{rfc8032_vectors[0].secret_key_hex};
            middle[64U + 5U] = middle[64U + 5U] == '0' ? '1' : '0';
            expect(error_code_of(parse_secret_key_hex(middle)) ==
                   invalid_argument());
        };

    "signature/to_hex round-trips keys and signatures (uppercase accepted)"_test =
        [] {
            for (const auto &vector : rfc8032_vectors) {
                const auto public_key{
                    parse_public_key_hex(vector.public_key_hex)};
                const auto secret{parse_secret_key_hex(vector.secret_key_hex)};
                const auto sig{parse_signature_hex(vector.signature_hex)};
                expect(public_key.has_value() && secret.has_value() &&
                       sig.has_value());
                if (!public_key || !secret || !sig) {
                    continue;
                }
                expect(to_hex(*public_key) == vector.public_key_hex);
                expect(to_hex(*secret) == vector.secret_key_hex);
                expect(to_hex(*sig) == vector.signature_hex);

                const auto upper_key{
                    parse_public_key_hex(uppercase(vector.public_key_hex))};
                const auto upper_secret{
                    parse_secret_key_hex(uppercase(vector.secret_key_hex))};
                const auto upper_sig{
                    parse_signature_hex(uppercase(vector.signature_hex))};
                expect(upper_key.has_value() && upper_secret.has_value() &&
                       upper_sig.has_value());
                if (!upper_key || !upper_secret || !upper_sig) {
                    continue;
                }
                expect(to_hex(*upper_key) == vector.public_key_hex);
                expect(to_hex(*upper_secret) == vector.secret_key_hex);
                expect(to_hex(*upper_sig) == vector.signature_hex);
            }
        };

    "signature/1 MiB message signs deterministically and verifies"_test = [] {
        const auto secret{
            parse_secret_key_hex(rfc8032_vectors[0].secret_key_hex)};
        const auto public_key{
            parse_public_key_hex(rfc8032_vectors[0].public_key_hex)};
        expect(static_cast<bool>(secret) && static_cast<bool>(public_key));
        if (!secret || !public_key) {
            return;
        }
        std::vector<std::byte> message(1U << 20U);
        for (std::size_t index{0}; index < message.size(); ++index) {
            message[index] = static_cast<std::byte>(index % 251U);
        }

        const signature first{sign(message, *secret)};
        const signature second{sign(message, *secret)};
        expect(first == second);
        expect(verify(message, first, *public_key).has_value());

        const auto middle{message.size() / 2U};
        message[middle] ^= std::byte{0x01};
        expect(error_code_of(verify(message, first, *public_key)) ==
               parse_error());
        message[middle] ^= std::byte{0x01};
        expect(verify(message, first, *public_key).has_value());
    };

    "signature/secret key copies and moves stay independent"_test = [] {
        const auto secret{
            parse_secret_key_hex(rfc8032_vectors[1].secret_key_hex)};
        const auto expected{
            parse_signature_hex(rfc8032_vectors[1].signature_hex)};
        expect(secret.has_value() && expected.has_value());
        if (!secret || !expected) {
            return;
        }
        const auto message{hex_bytes(rfc8032_vectors[1].message_hex)};

        {
            // A copy owns its own bytes and signs identically; destroying it
            // (which zeroizes its storage) must not affect the original.
            auto copy{*secret};
            expect(to_hex(copy) == to_hex(*secret));
            expect(sign(message, copy) == *expected);
        }
        expect(sign(message, *secret) == *expected);

        const auto pair{keypair_from_seed(seed_of(*secret))};
        expect(static_cast<bool>(pair));
        if (!pair) {
            return;
        }
        expect(to_hex(pair->public_key) == rfc8032_vectors[1].public_key_hex);
        {
            auto copied_keypair{*pair};
            expect(sign(message, copied_keypair.secret_key) == *expected);
            auto moved_keypair{std::move(copied_keypair)};
            expect(sign(message, moved_keypair.secret_key) == *expected);
            expect(to_hex(moved_keypair.public_key) ==
                   rfc8032_vectors[1].public_key_hex);
        }
        // The original keypair is still usable after copies were destroyed.
        expect(sign(message, pair->secret_key) == *expected);
        expect(verify(message, *expected, pair->public_key).has_value());
    };

    "signature/keypair_from_seed is deterministic for arbitrary seeds"_test =
        [] {
            const ed25519_seed zero_seed{};
            const auto first{keypair_from_seed(zero_seed)};
            const auto second{keypair_from_seed(zero_seed)};
            expect(first.has_value() && second.has_value());
            if (first && second) {
                expect(to_hex(first->public_key) == to_hex(second->public_key));
                expect(to_hex(first->secret_key) == to_hex(second->secret_key));
            }

            ed25519_seed pattern{};
            for (std::size_t index{0}; index < pattern.size(); ++index) {
                pattern[index] = static_cast<std::byte>(index);
            }
            const auto patterned{keypair_from_seed(pattern)};
            expect(static_cast<bool>(patterned));
            if (first && patterned) {
                // A different seed must produce a different public key.
                expect(to_hex(first->public_key) !=
                       to_hex(patterned->public_key));
            }
        };

    "signature/generated keypair signs and verifies arbitrary messages"_test =
        [] {
            ed25519_seed seed{};
            for (std::size_t index{0}; index < seed.size(); ++index) {
                seed[index] = static_cast<std::byte>(index + 1U);
            }
            const auto pair{keypair_from_seed(seed)};
            expect(static_cast<bool>(pair));
            if (!pair) {
                return;
            }
            const std::array<std::string_view, 3> messages{
                "refrain:rfmod-manifest", "", "x"};
            for (const auto text : messages) {
                const auto bytes{bytes_of(text)};
                const auto sig{sign(bytes, pair->secret_key)};
                expect(verify(bytes, sig, pair->public_key).has_value());
                expect(to_hex(sig) == to_hex(sign(bytes, pair->secret_key)));
            }

            // Another key cannot verify this keypair's signatures.
            const auto other_pair{
                keypair_from_seed(ed25519_seed{std::byte{0x01}})};
            expect(static_cast<bool>(other_pair));
            if (other_pair) {
                const auto bytes{hex_bytes("00ff10")};
                const auto sig{sign(bytes, pair->secret_key)};
                expect(
                    error_code_of(verify(bytes, sig, other_pair->public_key)) ==
                    parse_error());
            }
        };

    "signature/secret key hex round-trips and stays usable"_test = [] {
        ed25519_seed seed{};
        seed.front() = std::byte{0xA5};
        seed.back() = std::byte{0x5A};
        const auto pair{keypair_from_seed(seed)};
        expect(static_cast<bool>(pair));
        if (!pair) {
            return;
        }
        const auto hex{to_hex(pair->secret_key)};
        expect(hex.size() == 128U);
        const auto reparsed{parse_secret_key_hex(hex)};
        expect(static_cast<bool>(reparsed));
        if (!reparsed) {
            return;
        }
        expect(to_hex(*reparsed) == hex);
        const auto message{hex_bytes("0102030405")};
        expect(to_hex(sign(message, *reparsed)) ==
               to_hex(sign(message, pair->secret_key)));
        expect(verify(message, sign(message, *reparsed), pair->public_key)
                   .has_value());
    };

    "signature/all-zero keys and signatures parse but never verify"_test = [] {
        const auto zero_key{parse_public_key_hex(std::string(64, '0'))};
        const auto zero_sig{parse_signature_hex(std::string(128, '0'))};
        expect(zero_key.has_value() && zero_sig.has_value());
        if (!zero_key || !zero_sig) {
            return;
        }
        const auto message{hex_bytes("00")};
        expect(error_code_of(verify(message, *zero_sig, *zero_key)) ==
               parse_error());

        // An all-zero secret key is not a seed/public-half pair, so parsing the
        // 64-byte form must fail rather than accept unverifiable key material.
        expect(error_code_of(parse_secret_key_hex(std::string(128, '0'))) ==
               invalid_argument());
    };

    "signature/message length changes change the signature"_test = [] {
        const auto secret{
            parse_secret_key_hex(rfc8032_vectors[0].secret_key_hex)};
        expect(static_cast<bool>(secret));
        if (!secret) {
            return;
        }
        const auto empty{sign(std::span<const std::byte>{}, *secret)};
        const auto one{sign(hex_bytes("00"), *secret)};
        const auto two{sign(hex_bytes("0000"), *secret)};
        // Compare the hex forms: boost.ut's range `operator==` reports
        // std::byte elements through an ostream, which has no formatter.
        expect(to_hex(empty) != to_hex(one));
        expect(to_hex(one) != to_hex(two));
        expect(to_hex(empty) != to_hex(two));

        const auto public_key{
            parse_public_key_hex(rfc8032_vectors[0].public_key_hex)};
        expect(static_cast<bool>(public_key));
        if (public_key) {
            expect(verify(std::span<const std::byte>{}, empty, *public_key)
                       .has_value());
            expect(verify(hex_bytes("00"), one, *public_key).has_value());
            expect(verify(hex_bytes("0000"), two, *public_key).has_value());
            expect(error_code_of(verify(hex_bytes("00"), empty, *public_key)) ==
                   parse_error());
        }
    };
};

} // namespace
