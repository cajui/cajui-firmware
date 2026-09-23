#include "cajui_protocol.h"
#include <cstring>
#ifdef ARDUINO
#include <mbedtls/gcm.h>
#else
#include <openssl/evp.h>
#endif

namespace cajui {
namespace {
bool crypt(bool sealing, const Key& key, const uint8_t nonce[12],
           const uint8_t* aad, size_t aadSize, const uint8_t* input,
           size_t size, uint8_t* output, uint8_t tag[16]) {
    if (aadSize > MaxFrame || size > MaxPayload) return false;
    bool ok = false;
#ifdef ARDUINO
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data(), 128) == 0) {
        const int result = sealing ?
            mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, size, nonce, 12,
                                     aad, aadSize, input, output, 16, tag) :
            mbedtls_gcm_auth_decrypt(&ctx, size, nonce, 12, aad, aadSize, tag, 16,
                                    input, output);
        ok = result == 0;
    }
    mbedtls_gcm_free(&ctx);
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int written = 0, finalSize = 0;
    if (sealing) {
        ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), nonce) == 1 &&
             EVP_EncryptUpdate(ctx, nullptr, &written, aad, int(aadSize)) == 1 &&
             EVP_EncryptUpdate(ctx, output, &written, input, int(size)) == 1 &&
             EVP_EncryptFinal_ex(ctx, output + written, &finalSize) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    } else {
        ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), nonce) == 1 &&
             EVP_DecryptUpdate(ctx, nullptr, &written, aad, int(aadSize)) == 1 &&
             EVP_DecryptUpdate(ctx, output, &written, input, int(size)) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1 &&
             EVP_DecryptFinal_ex(ctx, output + written, &finalSize) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
#endif
    // Do not expose unauthenticated plaintext through the output buffer on failure.
    if (!ok) std::memset(output, 0, size);
    return ok;
}
}
bool encrypt(const Key& key, const uint8_t nonce[12], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[16]) {
    return crypt(true, key, nonce, aad, aadSize, input, size, output, tag);
}
bool decrypt(const Key& key, const uint8_t nonce[12], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[16], uint8_t* output) {
    Tag copy{};
    std::memcpy(copy.data(), tag, copy.size());
    return crypt(false, key, nonce, aad, aadSize, input, size, output, copy.data());
}
} // namespace cajui
