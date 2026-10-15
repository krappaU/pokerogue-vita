// SPDX-License-Identifier: AGPL-3.0-only
// The game encrypts its local save copies with crypto-js: AES.encrypt(json, passphrase), i.e.
// OpenSSL's "Salted__" format (EVP_BytesToKey with MD5, AES-256-CBC, PKCS#7, base64). In QuickJS
// that took 7-16 s per save on the Vita. These natives do the same with OpenSSL; js/savecrypt.js
// swaps them in after checking them against crypto-js itself.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "host.h"
#include "quickjs.h"

// Strict UTF-8, as decodeURIComponent accepts it (no overlongs, no surrogates, at most U+10FFFF).
// Strings with lone surrogates come out of JS_ToCStringLen as encoded surrogates and fail too.
static bool utf8_valid(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int len;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else return false;
        if (i + len > n || p[i + 1] < lo || p[i + 1] > hi) return false;
        for (int k = 2; k < len; k++)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *p, size_t n, size_t *out_len) {
    size_t o = 0, olen = (n + 2) / 3 * 4;
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = b64chars[(v >> 6) & 63];
        out[o++] = b64chars[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0);
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64chars[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

// Plain base64 with optional '=' padding; anything else (whitespace, URL alphabet) -> NULL, and
// the caller leaves the string to crypto-js.
static uint8_t *b64_decode(const char *s, size_t n, size_t *out_len) {
    while (n && s[n - 1] == '=') n--;
    if (n % 4 == 1) return NULL;
    static int8_t rev[256];
    if (!rev['B']) {
        memset(rev, -1, sizeof rev);
        for (int i = 0; i < 64; i++) rev[(uint8_t)b64chars[i]] = (int8_t)i;
    }
    uint8_t *out = malloc(n / 4 * 3 + 3);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = rev[(uint8_t)s[i]];
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = o;
    return out;
}

// EVP_BytesToKey(MD5, 1 round): crypto-js's OpenSSL KDF for AES-256 (32-byte key, 16-byte IV).
static bool derive(const char *pass, size_t pass_len, const uint8_t salt[8], uint8_t key[32], uint8_t iv[16]) {
    return EVP_BytesToKey(EVP_aes_256_cbc(), EVP_md5(), salt, (const unsigned char *)pass, (int)pass_len, 1, key, iv) == 32;
}

static uint8_t *aes_cbc(bool enc, const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t n, size_t *out_len) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return NULL;
    uint8_t *out = malloc(n + 32);
    int l1 = 0, l2 = 0;
    bool ok = out && EVP_CipherInit_ex(c, EVP_aes_256_cbc(), NULL, key, iv, enc ? 1 : 0) == 1 &&
              EVP_CipherUpdate(c, out, &l1, in, (int)n) == 1 && EVP_CipherFinal_ex(c, out + l1, &l2) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) {
        free(out);
        return NULL;
    }
    *out_len = (size_t)l1 + (size_t)l2;
    return out;
}

static JSValue ab(JSContext *ctx, const uint8_t *p, size_t n) { return JS_NewArrayBufferCopy(ctx, p, n); }

// aesEncrypt(message, passphrase) -> [base64, salt, key, iv, ciphertext] (ArrayBuffers), or null
// when crypto-js would do something else (message with lone surrogates).
static JSValue n_aes_encrypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_NULL;
    size_t mlen, plen;
    const char *msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    const char *pass = JS_ToCStringLen(ctx, &plen, argv[1]);
    JSValue ret = JS_NULL;
    uint8_t salt[8], key[32], iv[16];
    uint8_t *ct = NULL, *all = NULL;
    char *b64 = NULL;
    size_t ct_len = 0, b64_len = 0;
    if (!msg || !pass) {
        ret = JS_EXCEPTION;
        goto done;
    }
    if (!utf8_valid((const uint8_t *)msg, mlen) || !utf8_valid((const uint8_t *)pass, plen)) goto done;
    if (!host_random_bytes(salt, 8) || !derive(pass, plen, salt, key, iv)) goto done;
    if (!(ct = aes_cbc(true, key, iv, (const uint8_t *)msg, mlen, &ct_len))) goto done;
    if (!(all = malloc(16 + ct_len))) goto done;
    memcpy(all, "Salted__", 8);
    memcpy(all + 8, salt, 8);
    memcpy(all + 16, ct, ct_len);
    if (!(b64 = b64_encode(all, 16 + ct_len, &b64_len))) goto done;
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0, JS_NewStringLen(ctx, b64, b64_len));
    JS_SetPropertyUint32(ctx, ret, 1, ab(ctx, salt, 8));
    JS_SetPropertyUint32(ctx, ret, 2, ab(ctx, key, 32));
    JS_SetPropertyUint32(ctx, ret, 3, ab(ctx, iv, 16));
    JS_SetPropertyUint32(ctx, ret, 4, ab(ctx, ct, ct_len));
done:
    free(b64);
    free(all);
    free(ct);
    JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, pass);
    return ret;
}

// aesDecrypt(openSSLBase64, passphrase) -> [utf8 string | null, plaintext, salt, key, iv], or null
// when it isn't a well-formed salted ciphertext with valid padding (crypto-js then handles it,
// with its own results for such input).
static JSValue n_aes_decrypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_NULL;
    size_t slen, plen, raw_len = 0, pt_len = 0;
    const char *s = JS_ToCStringLen(ctx, &slen, argv[0]);
    const char *pass = JS_ToCStringLen(ctx, &plen, argv[1]);
    JSValue ret = JS_NULL;
    uint8_t key[32], iv[16];
    uint8_t *raw = NULL, *pt = NULL;
    if (!s || !pass) {
        ret = JS_EXCEPTION;
        goto done;
    }
    if (!utf8_valid((const uint8_t *)pass, plen)) goto done;
    if (!(raw = b64_decode(s, slen, &raw_len))) goto done;
    if (raw_len < 32 || (raw_len - 16) % 16 || memcmp(raw, "Salted__", 8)) goto done;
    if (!derive(pass, plen, raw + 8, key, iv)) goto done;
    if (!(pt = aes_cbc(false, key, iv, raw + 16, raw_len - 16, &pt_len))) goto done;
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0, utf8_valid(pt, pt_len) ? JS_NewStringLen(ctx, (const char *)pt, pt_len) : JS_NULL);
    JS_SetPropertyUint32(ctx, ret, 1, ab(ctx, pt, pt_len));
    JS_SetPropertyUint32(ctx, ret, 2, ab(ctx, raw + 8, 8));
    JS_SetPropertyUint32(ctx, ret, 3, ab(ctx, key, 32));
    JS_SetPropertyUint32(ctx, ret, 4, ab(ctx, iv, 16));
done:
    free(raw);
    free(pt);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, pass);
    return ret;
}

// bytesToWords(ArrayBuffer) -> crypto-js words (big-endian int32s, the last one zero-filled)
static JSValue n_bytes_to_words(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0, w = 0; i < n; i += 4, w++) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) v |= (uint32_t)(i + k < n ? p[i + k] : 0) << (24 - 8 * k);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)w, JS_NewInt32(ctx, (int32_t)v));
    }
    return arr;
}

void savecrypt_register(JSContext *ctx, JSValue native) {
    JS_SetPropertyStr(ctx, native, "aesEncrypt", JS_NewCFunction(ctx, n_aes_encrypt, "aesEncrypt", 2));
    JS_SetPropertyStr(ctx, native, "aesDecrypt", JS_NewCFunction(ctx, n_aes_decrypt, "aesDecrypt", 2));
    JS_SetPropertyStr(ctx, native, "bytesToWords", JS_NewCFunction(ctx, n_bytes_to_words, "bytesToWords", 1));
}
