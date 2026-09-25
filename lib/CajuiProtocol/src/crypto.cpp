// SPDX-License-Identifier: Apache-2.0
#include "cajui_protocol.h"
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#else
#include <openssl/evp.h>
#include <openssl/kdf.h>
#endif

namespace cajui {
namespace {
#ifdef ESP_PLATFORM
bool gcmSeal(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[TagSize]) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    const bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data(), KeySize * 8) == 0 &&
                    mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, size, nonce, NonceSize,
                                              aad, aadSize, input, output, TagSize, tag) == 0;
    mbedtls_gcm_free(&ctx);
    return ok;
}
bool gcmOpen(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[TagSize], uint8_t* output) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    const bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data(), KeySize * 8) == 0 &&
                    mbedtls_gcm_auth_decrypt(&ctx, size, nonce, NonceSize, aad, aadSize, tag,
                                             TagSize, input, output) == 0;
    mbedtls_gcm_free(&ctx);
    return ok;
}
#else
bool gcmSeal(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[TagSize]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int written = 0;
    int finalSize = 0;
    const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), nonce) == 1 &&
                    EVP_EncryptUpdate(ctx, nullptr, &written, aad, int(aadSize)) == 1 &&
                    EVP_EncryptUpdate(ctx, output, &written, input, int(size)) == 1 &&
                    EVP_EncryptFinal_ex(ctx, output + written, &finalSize) == 1 &&
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TagSize, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
bool gcmOpen(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[TagSize], uint8_t* output) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    Tag expected{}; // EVP_CIPHER_CTX_ctrl takes a mutable pointer even to set the tag.
    std::memcpy(expected.data(), tag, expected.size());
    int written = 0;
    int finalSize = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), nonce) == 1 &&
                    EVP_DecryptUpdate(ctx, nullptr, &written, aad, int(aadSize)) == 1 &&
                    EVP_DecryptUpdate(ctx, output, &written, input, int(size)) == 1 &&
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TagSize, expected.data()) == 1 &&
                    EVP_DecryptFinal_ex(ctx, output + written, &finalSize) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
#endif
#ifdef ESP_PLATFORM
// Blinding randomness for mbedtls_ecp_mul; never used as key material.
int blinding(void*, unsigned char* output, size_t size) {
    esp_fill_random(output, size);
    return 0;
}
// RFC 7748 decoding: clamp the scalar; mask the top bit of the peer u-coordinate.
bool montgomery(const X25519Key& scalar, const uint8_t* point, X25519Key& out) {
    X25519Key k = scalar;
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    X25519Key u{};
    if (point) {
        std::memcpy(u.data(), point, u.size());
        u[31] &= 127;
    }
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point base, result;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&base);
    mbedtls_ecp_point_init(&result);
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, k.data(), k.size()) == 0;
    if (ok && point)
        ok = mbedtls_mpi_read_binary_le(&base.X, u.data(), u.size()) == 0 &&
             mbedtls_mpi_lset(&base.Z, 1) == 0;
    ok = ok &&
         mbedtls_ecp_mul(&group, &result, &d, point ? &base : &group.G, blinding, nullptr) == 0 &&
         mbedtls_mpi_write_binary_le(&result.X, out.data(), out.size()) == 0;
    mbedtls_ecp_point_free(&result);
    mbedtls_ecp_point_free(&base);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
    volatile uint8_t* wipe = k.data();
    for (size_t i = 0; i < k.size(); ++i) wipe[i] = 0;
    return ok;
}
#else
bool montgomery(const X25519Key& scalar, const uint8_t* point, X25519Key& out) {
    EVP_PKEY* own =
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, scalar.data(), scalar.size());
    if (!own) return false;
    size_t size = out.size();
    bool ok = false;
    if (!point) {
        ok = EVP_PKEY_get_raw_public_key(own, out.data(), &size) == 1 && size == out.size();
    } else {
        EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, point, X25519Size);
        EVP_PKEY_CTX* ctx = peer ? EVP_PKEY_CTX_new(own, nullptr) : nullptr;
        ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
             EVP_PKEY_derive(ctx, out.data(), &size) == 1 && size == out.size();
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peer);
    }
    EVP_PKEY_free(own);
    return ok;
}
#endif
// Bounds keep the int casts required by OpenSSL safe on both backends.
bool withinBounds(size_t aadSize, size_t size) {
    return aadSize <= MaxFrame && size <= MaxPayload;
}
}
bool encrypt(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[TagSize]) {
    if (!withinBounds(aadSize, size)) return false;
    const bool ok = gcmSeal(key, nonce, aad, aadSize, input, size, output, tag);
    if (!ok) std::memset(output, 0, size);
    return ok;
}
bool decrypt(const Key& key, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[TagSize], uint8_t* output) {
    if (!withinBounds(aadSize, size)) return false;
    const bool ok = gcmOpen(key, nonce, aad, aadSize, input, size, tag, output);
    // Do not expose unauthenticated plaintext through the output buffer on failure.
    if (!ok) std::memset(output, 0, size);
    return ok;
}
} // namespace cajui

namespace cajui {
bool x25519Public(const X25519Key& privateKey, X25519Key& publicKey) {
    publicKey = X25519Key{};
    return montgomery(privateKey, nullptr, publicKey);
}
bool x25519Shared(const X25519Key& privateKey, const X25519Key& peerPublic, X25519Key& shared) {
    shared = X25519Key{};
    if (!montgomery(privateKey, peerPublic.data(), shared)) return false;
    uint8_t any = 0;
    for (auto byte : shared) any |= byte;
    if (any) return true;
    shared = X25519Key{};
    return false;
}
bool hkdfSha256(const uint8_t* ikm, size_t ikmSize, const uint8_t* salt, size_t saltSize,
                const uint8_t* info, size_t infoSize, uint8_t* output, size_t outputSize) {
    constexpr size_t Limit = size_t(255) * 32; // RFC 5869 maximum output for SHA-256.
    if (!ikm || !output || !outputSize || outputSize > Limit) return false;
#ifdef ESP_PLATFORM
    // The prebuilt Arduino-ESP32 mbedTLS is built without MBEDTLS_HKDF_C, so neither
    // mbedtls_hkdf nor PSA HKDF exists (both verified on hardware). RFC 5869 extract/expand
    // is composed from the library's HMAC-SHA256; the RFC test vector is checked on a device.
    constexpr size_t Hash = 32;
    const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t prk[Hash]{};
    uint8_t block[Hash]{};
    mbedtls_md_context_t hmac;
    mbedtls_md_init(&hmac);
    bool ok = sha256 && mbedtls_md_hmac(sha256, salt, saltSize, ikm, ikmSize, prk) == 0 &&
              mbedtls_md_setup(&hmac, sha256, 1) == 0;
    size_t done = 0;
    size_t previous = 0;
    uint8_t counter = 1;
    while (ok && done < outputSize) {
        ok = mbedtls_md_hmac_starts(&hmac, prk, sizeof(prk)) == 0 &&
             mbedtls_md_hmac_update(&hmac, block, previous) == 0 &&
             mbedtls_md_hmac_update(&hmac, info, infoSize) == 0 &&
             mbedtls_md_hmac_update(&hmac, &counter, 1) == 0 &&
             mbedtls_md_hmac_finish(&hmac, block) == 0;
        const size_t count = outputSize - done < Hash ? outputSize - done : Hash;
        if (ok) std::memcpy(output + done, block, count);
        done += count;
        previous = Hash;
        ++counter;
    }
    mbedtls_md_free(&hmac);
    volatile uint8_t* clear = prk;
    for (size_t i = 0; i < Hash; ++i) clear[i] = 0;
    clear = block;
    for (size_t i = 0; i < Hash; ++i) clear[i] = 0;
#else
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    size_t size = outputSize;
    const bool ok = ctx && EVP_PKEY_derive_init(ctx) == 1 &&
                    EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
                    EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, int(saltSize)) == 1 &&
                    EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, int(ikmSize)) == 1 &&
                    EVP_PKEY_CTX_add1_hkdf_info(ctx, info, int(infoSize)) == 1 &&
                    EVP_PKEY_derive(ctx, output, &size) == 1 && size == outputSize;
    EVP_PKEY_CTX_free(ctx);
#endif
    if (!ok) std::memset(output, 0, outputSize);
    return ok;
}
} // namespace cajui
