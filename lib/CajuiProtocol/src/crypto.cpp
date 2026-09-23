#include "cajui_protocol.h"
#include <cstring>
#ifdef ESP_PLATFORM
#include <mbedtls/gcm.h>
#else
#include <openssl/evp.h>
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
