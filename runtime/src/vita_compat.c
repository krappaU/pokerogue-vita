// SPDX-License-Identifier: AGPL-3.0-only
// VitaSDK's OpenSSL is built without its console UI, but libcurl references UI_OpenSSL() for
// passphrase-protected client keys (never used here). Hand it OpenSSL's null UI method instead.
#include <openssl/ui.h>

UI_METHOD *UI_OpenSSL(void) { return (UI_METHOD *)UI_null(); }

// ---------------------------------------------------------------- OpenSSL without thread locks
// OpenSSL 1.1 creates a pthread read-write lock for every certificate, TLS context and many other
// objects. On the Vita each one is backed by kernel objects, which a process only gets a limited
// number of: after ~90 certificates every further lock failed, which OpenSSL reports as "malloc
// failure" and which made TLS impossible. This app only uses OpenSSL from its main thread, so these
// replace OpenSSL's threads_pthread.c (all of its symbols, so the original is never linked) with
// single-threaded versions that need no kernel objects.
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdint.h>

CRYPTO_RWLOCK *CRYPTO_THREAD_lock_new(void) { return OPENSSL_zalloc(sizeof(int)); }
int CRYPTO_THREAD_read_lock(CRYPTO_RWLOCK *lock) { return 1; }
int CRYPTO_THREAD_write_lock(CRYPTO_RWLOCK *lock) { return 1; }
int CRYPTO_THREAD_unlock(CRYPTO_RWLOCK *lock) { return 1; }
void CRYPTO_THREAD_lock_free(CRYPTO_RWLOCK *lock) { OPENSSL_free(lock); }

int CRYPTO_THREAD_run_once(CRYPTO_ONCE *once, void (*init)(void)) {
    static const void *done[128];
    static int n_done;
    for (int i = 0; i < n_done; i++)
        if (done[i] == once) return 1;
    if (n_done < 128) done[n_done++] = once;
    init();
    return 1;
}

static void *locals[64];
static int n_locals;
int CRYPTO_THREAD_init_local(CRYPTO_THREAD_LOCAL *key, void (*cleanup)(void *)) {
    if (n_locals >= 64) return 0;
    *key = (CRYPTO_THREAD_LOCAL)(uintptr_t)(++n_locals);
    return 1;
}
void *CRYPTO_THREAD_get_local(CRYPTO_THREAD_LOCAL *key) { return locals[(uintptr_t)*key - 1]; }
int CRYPTO_THREAD_set_local(CRYPTO_THREAD_LOCAL *key, void *val) {
    locals[(uintptr_t)*key - 1] = val;
    return 1;
}
int CRYPTO_THREAD_cleanup_local(CRYPTO_THREAD_LOCAL *key) { return 1; }
CRYPTO_THREAD_ID CRYPTO_THREAD_get_current_id(void) { return pthread_self(); }
int CRYPTO_THREAD_compare_id(CRYPTO_THREAD_ID a, CRYPTO_THREAD_ID b) { return pthread_equal(a, b); }
int CRYPTO_atomic_add(int *val, int amount, int *ret, CRYPTO_RWLOCK *lock) {
    *ret = (*val += amount);
    return 1;
}
int openssl_init_fork_handlers(void) { return 0; }
int openssl_get_fork_id(void) { return 1; }
