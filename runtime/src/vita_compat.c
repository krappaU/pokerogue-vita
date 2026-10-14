// SPDX-License-Identifier: AGPL-3.0-only
// VitaSDK's OpenSSL is built without its console UI, but libcurl references UI_OpenSSL() for
// passphrase-protected client keys (never used here). Hand it OpenSSL's null UI method instead.
#include <openssl/ui.h>

UI_METHOD *UI_OpenSSL(void) { return (UI_METHOD *)UI_null(); }
