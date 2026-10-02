/* Shared pieces of internalBinding('crypto') and ('tls_wrap'): OpenSSL
 * error reporting, key data, crypto jobs (src/crypto/crypto_util.h). */
#ifndef COLLABO_NODE_CRYPTO_H
#define COLLABO_NODE_CRYPTO_H

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "node.h"

/* Node's enums (crypto_keys.h, crypto_util.h) */
enum { kKeyTypeSecret = 0, kKeyTypePublic = 1, kKeyTypePrivate = 2 };
enum { kKeyFormatDER = 0, kKeyFormatPEM, kKeyFormatJWK, kKeyFormatRawPublic,
       kKeyFormatRawPrivate, kKeyFormatRawSeed, kKeyFormatStore };
enum { kKeyEncodingPKCS1 = 0, kKeyEncodingPKCS8, kKeyEncodingSPKI, kKeyEncodingSEC1 };
enum { kCryptoJobAsync = 0, kCryptoJobSync = 1, kCryptoJobWebCrypto = 2 };
enum { kSigEncDER = 0, kSigEncP1363 = 1 };
enum { kSignJobModeSign = 0, kSignJobModeVerify = 1 };
enum { kWebCryptoKeyFormatRaw = 0, kWebCryptoKeyFormatPKCS8, kWebCryptoKeyFormatSPKI,
       kWebCryptoKeyFormatJWK };
enum { kWebCryptoCipherEncrypt = 0, kWebCryptoCipherDecrypt = 1 };

/* ---- errors ---- */

/* ThrowCryptoError: message from err (or msg when err is 0), decorated
   with library/reason/code and opensslErrorStack */
JSValue crypto_throw(JSContext *ctx, unsigned long err, const char *msg);
/* the same Error object, not thrown */
JSValue crypto_error(JSContext *ctx, unsigned long err, const char *msg);
/* a plain Error with a Node error code (ERR_CRYPTO_...) */
JSValue crypto_throw_code(JSContext *ctx, const char *code, const char *fmt, ...);
JSValue crypto_error_code(JSContext *ctx, const char *code, const char *msg);

/* ---- key data: shared between handles, KeyObjects, CryptoKeys, jobs ---- */

typedef struct KeyData {
  int refs; /* atomic */
  int type; /* kKeyType* */
  EVP_PKEY *pkey;
  uint8_t *secret;
  size_t secret_len;
} KeyData;

KeyData *key_data_new_secret(const uint8_t *data, size_t len);
KeyData *key_data_new_pkey(int type, EVP_PKEY *pkey); /* takes pkey */
KeyData *key_data_ref(KeyData *k);
void key_data_unref(KeyData *k);
/* same key, another type (a public view of a private key) */
KeyData *key_data_with_type(KeyData *k, int type);

extern JSClassID crypto_key_handle_class_id;
KeyData *crypto_key_handle_data(JSValueConst obj); /* NULL if not a handle */
JSValue crypto_key_handle_new(JSContext *ctx, KeyData *k); /* refs k */
/* new InternalCryptoKey(handle, algorithm, usages, extractable) */
JSValue crypto_new_crypto_key(JSContext *ctx, KeyData *k, JSValueConst algorithm,
                              uint32_t usages, bool extractable);

/* the 5-argument key groups the JS side passes (data, format, type,
   passphrase, namedCurve); *offset advances past the group */
KeyData *crypto_get_public_or_private_key(JSContext *ctx, JSValueConst *argv, int argc,
                                          int *offset);
KeyData *crypto_get_private_key(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                bool allow_key_object);

/* key encodings for output: format, type, cipher, passphrase */
typedef struct {
  bool output_key_object;
  int format, type;
  const EVP_CIPHER *cipher;
  uint8_t *passphrase;
  size_t passphrase_len;
  bool has_passphrase;
  int ec_point_form;
} KeyEncoding;
int crypto_parse_public_encoding(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                 KeyEncoding *enc, bool generate);
int crypto_parse_private_encoding(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                  KeyEncoding *enc, bool generate);
void crypto_key_encoding_free(KeyEncoding *enc);
/* encodes a key as PEM string / DER buffer / JWK / raw per enc */
JSValue crypto_export_public(JSContext *ctx, KeyData *k, const KeyEncoding *enc);
JSValue crypto_export_private(JSContext *ctx, KeyData *k, const KeyEncoding *enc);

const char *crypto_asymmetric_key_type(EVP_PKEY *pkey); /* "rsa", "ec", ... or NULL */
JSValue crypto_ec_point_to_buffer(JSContext *ctx, const EC_GROUP *g, const EC_POINT *pt,
                                  int form);

/* ---- byte input helpers ---- */

/* bytes of a string (utf8) or any buffer source; malloc'ed copy */
uint8_t *crypto_bytes_copy(JSContext *ctx, JSValueConst v, size_t *len);
/* direct pointer into an ArrayBuffer/view, or NULL */
uint8_t *crypto_buffer_source(JSContext *ctx, JSValueConst v, size_t *len);
bool crypto_is_buffer_source(JSContext *ctx, JSValueConst v);
JSValue crypto_new_array_buffer(JSContext *ctx, const void *data, size_t len);
const EVP_MD *crypto_get_digest(const char *name);
int crypto_nid_from_curve(const char *name);

/* ---- crypto jobs ---- */

typedef struct CryptoJob CryptoJob;
typedef struct {
  const char *name;
  ProviderType provider;
  size_t params_size;
  /* parse argv[1..] into job->params; return -1 with a JS exception */
  int (*config)(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv);
  /* off the JS thread: no JS, records errors in job */
  void (*work)(CryptoJob *job);
  /* [err, result]; -1 on a JS exception */
  int (*result)(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res);
  void (*cleanup)(CryptoJob *job);
} CryptoJobTraits;

struct CryptoJob {
  AsyncWrap aw;
  const CryptoJobTraits *traits;
  int mode;
  uv_work_t work;
  bool scheduled;
  JSValue resolving_funcs[2]; /* WebCrypto */
  /* CryptoErrorStore */
  unsigned long errors[8];
  int nerrors;
  char message[160]; /* a non-OpenSSL error, or "" */
  const char *message_code;
  void *params;
};

/* record the OpenSSL error queue (and an optional message) as the error */
void crypto_job_capture_errors(CryptoJob *job, const char *message);
bool crypto_job_failed(CryptoJob *job);
/* the job's error as a JS value */
JSValue crypto_job_error(JSContext *ctx, CryptoJob *job);
/* defines a job class on the binding object */
void crypto_define_job(JSContext *ctx, JSValueConst target, const CryptoJobTraits *traits);
int crypto_job_mode(CryptoJob *job);

/* ---- per-file binding parts ---- */
void crypto_init_keys(Env *env, JSValueConst target);
void crypto_init_cipher(Env *env, JSValueConst target);
void crypto_init_sig(Env *env, JSValueConst target);
void crypto_init_x509(Env *env, JSValueConst target);
void crypto_init_tls(Env *env, JSValueConst target);

/* X509 objects (crypto_x509.c) */
JSValue crypto_x509_to_object(JSContext *ctx, X509 *cert); /* legacy cert dict */
JSValue crypto_x509_new_handle(JSContext *ctx, X509 *cert, STACK_OF(X509) *issuer_chain);
X509_STORE *crypto_new_root_store(void);
JSValue crypto_root_certificates(JSContext *ctx);

#endif
