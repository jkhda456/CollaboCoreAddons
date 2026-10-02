/* TLS: SecureContext of internalBinding('crypto') (crypto_context.cc) and
 * internalBinding('tls_wrap') (crypto_tls.cc).  A TLSWrap consumes a native
 * stream (TCP / pipe), runs OpenSSL over memory BIOs and is itself a
 * StreamBase towards JS. */
#include <openssl/core_names.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "streams.h"

static JSClassID secure_context_class_id, tls_wrap_class_id;

/* ---------------------------------------------------------------------- */
/* SecureContext */

typedef struct {
  SSL_CTX *ctx;
  X509 *cert, *issuer;
  X509_STORE *own_store; /* the store this context owns (not the shared root) */
  uint8_t ticket_key_name[16], ticket_key_hmac[16], ticket_key_aes[16];
} SecureContext;

static X509_STORE *root_store;

static X509_STORE *get_root_store(void) {
  if (!root_store)
    root_store = crypto_new_root_store();
  return root_store;
}

void crypto_reset_root_store(void) {
  if (root_store) {
    X509_STORE_free(root_store);
    root_store = NULL;
  }
}

static BIO *load_bio(JSContext *ctx, JSValueConst v) {
  size_t len;
  uint8_t *d;
  BIO *bio;
  if (!JS_IsString(v) && !crypto_is_buffer_source(ctx, v))
    return NULL;
  d = crypto_bytes_copy(ctx, v, &len);
  if (!d)
    return NULL;
  bio = BIO_new(BIO_s_mem());
  BIO_write(bio, d, (int)len);
  free(d);
  return bio;
}

static void sc_finalizer(JSRuntime *rt, JSValueConst val) {
  SecureContext *sc = JS_GetOpaque(val, secure_context_class_id);
  if (!sc)
    return;
  SSL_CTX_free(sc->ctx);
  X509_free(sc->cert);
  X509_free(sc->issuer);
  free(sc);
}

static JSValue sc_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, secure_context_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(SecureContext)));
  return obj;
}

static SecureContext *sc_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, secure_context_class_id);
}

static SecureContext *sc_ctx_of(JSContext *ctx, JSValueConst v) {
  SecureContext *sc = sc_of(ctx, v);
  if (sc && !sc->ctx) {
    JS_ThrowTypeError(ctx, "SecureContext is not initialized");
    return NULL;
  }
  return sc;
}

/* session ticket encryption with the context's keys */
static int ticket_compat_cb(SSL *ssl, unsigned char *name, unsigned char *iv,
                            EVP_CIPHER_CTX *ectx, EVP_MAC_CTX *hctx, int enc) {
  SecureContext *sc = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
  OSSL_PARAM params[2];
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, "SHA256", 0);
  params[1] = OSSL_PARAM_construct_end();
  if (enc) {
    memcpy(name, sc->ticket_key_name, 16);
    if (RAND_bytes(iv, 16) <= 0 ||
        EVP_EncryptInit_ex(ectx, EVP_aes_128_cbc(), NULL, sc->ticket_key_aes, iv) <= 0 ||
        EVP_MAC_init(hctx, sc->ticket_key_hmac, 16, params) <= 0)
      return -1;
    return 1;
  }
  if (memcmp(name, sc->ticket_key_name, 16) != 0)
    return 0;
  if (EVP_DecryptInit_ex(ectx, EVP_aes_128_cbc(), NULL, sc->ticket_key_aes, iv) <= 0 ||
      EVP_MAC_init(hctx, sc->ticket_key_hmac, 16, params) <= 0)
    return -1;
  return 1;
}

/* init(secureProtocol, minVersion, maxVersion) */
static JSValue sc_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SecureContext *sc = sc_of(ctx, this_val);
  int min_v = nb_int32(ctx, ARG(1), 0), max_v = nb_int32(ctx, ARG(2), 0);
  const SSL_METHOD *method = TLS_method();
  if (!sc)
    return JS_EXCEPTION;
  if (max_v == 0)
    max_v = TLS1_3_VERSION;
  if (JS_IsString(ARG(0))) {
    const char *m = JS_ToCString(ctx, ARG(0));
    static const struct { const char *name; int min, max, kind; } methods[] = {
      { "SSLv23_method", -1, TLS1_2_VERSION, 0 },
      { "SSLv23_server_method", -1, TLS1_2_VERSION, 1 },
      { "SSLv23_client_method", -1, TLS1_2_VERSION, 2 },
      { "TLS_method", 0, TLS1_3_VERSION, 0 },
      { "TLS_server_method", 0, TLS1_3_VERSION, 1 },
      { "TLS_client_method", 0, TLS1_3_VERSION, 2 },
      { "TLSv1_method", TLS1_VERSION, TLS1_VERSION, 0 },
      { "TLSv1_server_method", TLS1_VERSION, TLS1_VERSION, 1 },
      { "TLSv1_client_method", TLS1_VERSION, TLS1_VERSION, 2 },
      { "TLSv1_1_method", TLS1_1_VERSION, TLS1_1_VERSION, 0 },
      { "TLSv1_1_server_method", TLS1_1_VERSION, TLS1_1_VERSION, 1 },
      { "TLSv1_1_client_method", TLS1_1_VERSION, TLS1_1_VERSION, 2 },
      { "TLSv1_2_method", TLS1_2_VERSION, TLS1_2_VERSION, 0 },
      { "TLSv1_2_server_method", TLS1_2_VERSION, TLS1_2_VERSION, 1 },
      { "TLSv1_2_client_method", TLS1_2_VERSION, TLS1_2_VERSION, 2 },
    };
    size_t i;
    bool found = false;
    if (!strncmp(m, "SSLv2_", 6) || !strncmp(m, "SSLv3_", 6)) {
      JSValue r = node_throw_type_error(ctx, "ERR_TLS_INVALID_PROTOCOL_METHOD",
                                        !strncmp(m, "SSLv2_", 6) ? "SSLv2 methods disabled"
                                                                 : "SSLv3 methods disabled");
      JS_FreeCString(ctx, m);
      return r;
    }
    for (i = 0; i < countof(methods); i++)
      if (!strcmp(m, methods[i].name)) {
        if (methods[i].min >= 0)
          min_v = methods[i].min;
        max_v = methods[i].max;
        method = methods[i].kind == 1 ? TLS_server_method()
                 : methods[i].kind == 2 ? TLS_client_method() : TLS_method();
        found = true;
        break;
      }
    if (!found) {
      JSValue r = node_throw_type_error(ctx, "ERR_TLS_INVALID_PROTOCOL_METHOD",
                                        "Unknown method: %s", m);
      JS_FreeCString(ctx, m);
      return r;
    }
    JS_FreeCString(ctx, m);
  }
  SSL_CTX_free(sc->ctx);
  sc->ctx = SSL_CTX_new(method);
  if (!sc->ctx)
    return crypto_throw(ctx, ERR_get_error(), "SSL_CTX_new");
  SSL_CTX_set_app_data(sc->ctx, sc);
  SSL_CTX_set_options(sc->ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
  SSL_CTX_set_options(sc->ctx, SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
  SSL_CTX_clear_mode(sc->ctx, SSL_MODE_NO_AUTO_CHAIN);
  SSL_CTX_set_session_cache_mode(sc->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_SERVER |
                                              SSL_SESS_CACHE_NO_INTERNAL |
                                              SSL_SESS_CACHE_NO_AUTO_CLEAR);
  SSL_CTX_set_min_proto_version(sc->ctx, min_v);
  SSL_CTX_set_max_proto_version(sc->ctx, max_v);
  if (RAND_bytes(sc->ticket_key_name, 16) <= 0 || RAND_bytes(sc->ticket_key_hmac, 16) <= 0 ||
      RAND_bytes(sc->ticket_key_aes, 16) <= 0)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Error generating ticket keys");
  SSL_CTX_set_tlsext_ticket_key_evp_cb(sc->ctx, ticket_compat_cb);
  return JS_UNDEFINED;
}

static X509_STORE *sc_own_store(SecureContext *sc) {
  X509_STORE *store;
  if (sc->own_store)
    return sc->own_store;
  store = SSL_CTX_get_cert_store(sc->ctx);
  if (store == root_store) {
    store = crypto_new_root_store();
    SSL_CTX_set_cert_store(sc->ctx, store);
  }
  return sc->own_store = store;
}

static int pem_pass_cb(char *buf, int size, int rwflag, void *u) {
  const char *pass = u;
  int len;
  if (!pass)
    return -1;
  len = (int)strlen(pass);
  if (len > size)
    len = size;
  memcpy(buf, pass, len);
  return len;
}

/* setKey(key, passphrase) */
static JSValue sc_set_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *bio;
  EVP_PKEY *key;
  char *pass = NULL;
  if (!sc)
    return JS_EXCEPTION;
  bio = load_bio(ctx, ARG(0));
  if (!bio)
    return JS_EXCEPTION;
  if (JS_IsString(ARG(1))) {
    const char *p = JS_ToCString(ctx, ARG(1));
    pass = strdup(p);
    JS_FreeCString(ctx, p);
  }
  key = PEM_read_bio_PrivateKey(bio, NULL, pem_pass_cb, pass);
  BIO_free(bio);
  free(pass);
  if (!key)
    return crypto_throw(ctx, ERR_get_error(), "PEM_read_bio_PrivateKey");
  if (!SSL_CTX_use_PrivateKey(sc->ctx, key)) {
    EVP_PKEY_free(key);
    return crypto_throw(ctx, ERR_get_error(), "SSL_CTX_use_PrivateKey");
  }
  EVP_PKEY_free(key);
  return JS_UNDEFINED;
}

/* SSL_CTX_use_certificate_chain: leaf + extra certs; finds the issuer */
static int use_cert_chain(SecureContext *sc, X509 *x, STACK_OF(X509) *extra) {
  int i, ok = SSL_CTX_use_certificate(sc->ctx, x);
  X509 *issuer = NULL;
  if (!ok)
    return 0;
  SSL_CTX_clear_extra_chain_certs(sc->ctx);
  for (i = 0; extra && i < sk_X509_num(extra); i++) {
    X509 *ca = sk_X509_value(extra, i);
    if (!SSL_CTX_add1_chain_cert(sc->ctx, ca))
      return 0;
    if (!issuer && X509_check_issued(ca, x) == X509_V_OK)
      issuer = ca;
  }
  X509_free(sc->cert);
  sc->cert = X509_dup(x);
  X509_free(sc->issuer);
  sc->issuer = issuer ? X509_dup(issuer) : NULL;
  if (!sc->issuer) {
    /* the issuer from the trust store */
    X509_STORE *store = SSL_CTX_get_cert_store(sc->ctx);
    X509_STORE_CTX *sctx = X509_STORE_CTX_new();
    X509 *found = NULL;
    if (sctx && X509_STORE_CTX_init(sctx, store, NULL, NULL) == 1 &&
        X509_STORE_CTX_get1_issuer(&found, sctx, x) == 1)
      sc->issuer = found;
    X509_STORE_CTX_free(sctx);
    ERR_clear_error();
  }
  return 1;
}

static JSValue sc_set_cert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *bio;
  X509 *x, *ca;
  STACK_OF(X509) *extra;
  int ok;
  if (!sc)
    return JS_EXCEPTION;
  bio = load_bio(ctx, ARG(0));
  if (!bio)
    return JS_UNDEFINED;
  x = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
  if (!x) {
    BIO_free(bio);
    return crypto_throw(ctx, ERR_get_error(), "SSL_CTX_use_certificate_chain");
  }
  extra = sk_X509_new_null();
  while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL)
    sk_X509_push(extra, ca);
  ERR_clear_error();
  BIO_free(bio);
  ok = use_cert_chain(sc, x, extra);
  X509_free(x);
  sk_X509_pop_free(extra, X509_free);
  if (!ok)
    return crypto_throw(ctx, ERR_get_error(), "SSL_CTX_use_certificate_chain");
  return JS_UNDEFINED;
}

static JSValue sc_add_ca_cert(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *bio;
  X509 *x;
  if (!sc)
    return JS_EXCEPTION;
  bio = load_bio(ctx, ARG(0));
  if (!bio)
    return JS_UNDEFINED;
  while ((x = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL)) != NULL) {
    X509_STORE_add_cert(sc_own_store(sc), x);
    SSL_CTX_add_client_CA(sc->ctx, x);
    X509_free(x);
  }
  ERR_clear_error();
  BIO_free(bio);
  return JS_UNDEFINED;
}

static JSValue sc_add_crl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *bio;
  X509_CRL *crl;
  X509_STORE *store;
  if (!sc)
    return JS_EXCEPTION;
  bio = load_bio(ctx, ARG(0));
  if (!bio)
    return JS_UNDEFINED;
  crl = PEM_read_bio_X509_CRL(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (!crl)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to parse CRL");
  store = sc_own_store(sc);
  X509_STORE_add_crl(store, crl);
  X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
  X509_CRL_free(crl);
  return JS_UNDEFINED;
}

static JSValue sc_add_root_certs(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  X509_STORE *store;
  if (!sc)
    return JS_EXCEPTION;
  store = get_root_store();
  X509_STORE_up_ref(store);
  SSL_CTX_set_cert_store(sc->ctx, store);
  sc->own_store = NULL;
  ERR_clear_error();
  return JS_UNDEFINED;
}

static JSValue sc_set_allow_partial(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  if (!sc)
    return JS_EXCEPTION;
  X509_STORE_set_flags(sc_own_store(sc), X509_V_FLAG_PARTIAL_CHAIN);
  return JS_UNDEFINED;
}

static JSValue sc_set_str(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  const char *s;
  int ok;
  unsigned long err;
  if (!sc)
    return JS_EXCEPTION;
  s = JS_ToCString(ctx, ARG(0));
  if (!s)
    return JS_EXCEPTION;
  switch (magic) {
  case 0: /* setCipherSuites */
    ok = SSL_CTX_set_ciphersuites(sc->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok)
      return crypto_throw(ctx, ERR_get_error(), "Failed to set ciphers");
    break;
  case 1: /* setCiphers */
    ok = SSL_CTX_set_cipher_list(sc->ctx, s);
    if (!ok) {
      err = ERR_get_error();
      if (!*s && ERR_GET_REASON(err) == SSL_R_NO_CIPHER_MATCH) {
        JS_FreeCString(ctx, s);
        break;
      }
      JS_FreeCString(ctx, s);
      return crypto_throw(ctx, err, "Failed to set ciphers");
    }
    JS_FreeCString(ctx, s);
    break;
  case 2: /* setSigalgs */
    ok = SSL_CTX_set1_sigalgs_list(sc->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok)
      return crypto_throw(ctx, ERR_get_error(), NULL);
    break;
  case 3: /* setECDHCurve */
    ok = !strcmp(s, "auto") || SSL_CTX_set1_curves_list(sc->ctx, s);
    JS_FreeCString(ctx, s);
    if (!ok)
      return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set ECDH curve");
    break;
  default: { /* setSessionIdContext */
    size_t len = strlen(s);
    ok = SSL_CTX_set_session_id_context(sc->ctx, (const unsigned char *)s, (unsigned)len);
    JS_FreeCString(ctx, s);
    if (!ok)
      return JS_ThrowTypeError(ctx, "SSL_CTX_set_session_id_context error");
  }
  }
  ERR_clear_error();
  return JS_UNDEFINED;
}

static JSValue sc_set_dh_param(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *bio;
  EVP_PKEY *params;
  int bits;
  JSValue ret = JS_UNDEFINED;
  if (!sc)
    return JS_EXCEPTION;
  if (JS_IsBool(ARG(0)) && JS_ToBool(ctx, ARG(0))) {
    SSL_CTX_set_dh_auto(sc->ctx, 1);
    return JS_UNDEFINED;
  }
  bio = load_bio(ctx, ARG(0));
  if (!bio)
    return JS_UNDEFINED;
  params = PEM_read_bio_Parameters(bio, NULL);
  BIO_free(bio);
  if (!params || EVP_PKEY_get_base_id(params) != EVP_PKEY_DH) {
    EVP_PKEY_free(params);
    ERR_clear_error();
    return JS_UNDEFINED;
  }
  bits = EVP_PKEY_get_bits(params);
  if (bits < 1024) {
    EVP_PKEY_free(params);
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE", "DH parameter is less than 1024 bits");
  }
  if (bits < 2048)
    ret = JS_NewString(ctx, "DH parameter is less than 2048 bits");
  if (!SSL_CTX_set0_tmp_dh_pkey(sc->ctx, params)) {
    EVP_PKEY_free(params);
    JS_FreeValue(ctx, ret);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Error setting temp DH parameter");
  }
  return ret;
}

static JSValue sc_proto(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int magic) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  if (!sc)
    return JS_EXCEPTION;
  switch (magic) {
  case 0: SSL_CTX_set_min_proto_version(sc->ctx, nb_int32(ctx, ARG(0), 0)); break;
  case 1: SSL_CTX_set_max_proto_version(sc->ctx, nb_int32(ctx, ARG(0), 0)); break;
  case 2: return JS_NewUint32(ctx, (uint32_t)SSL_CTX_get_min_proto_version(sc->ctx));
  default: return JS_NewUint32(ctx, (uint32_t)SSL_CTX_get_max_proto_version(sc->ctx));
  }
  return JS_UNDEFINED;
}

static JSValue sc_set_options(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  if (!sc)
    return JS_EXCEPTION;
  SSL_CTX_set_options(sc->ctx, (uint64_t)nb_int64(ctx, ARG(0), 0));
  return JS_UNDEFINED;
}

static JSValue sc_set_session_timeout(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  if (!sc)
    return JS_EXCEPTION;
  SSL_CTX_set_timeout(sc->ctx, nb_int32(ctx, ARG(0), 0));
  return JS_UNDEFINED;
}

static JSValue sc_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SecureContext *sc = sc_of(ctx, this_val);
  if (!sc)
    return JS_EXCEPTION;
  SSL_CTX_free(sc->ctx);
  sc->ctx = NULL;
  sc->own_store = NULL;
  return JS_UNDEFINED;
}

/* loadPKCS12(pfx, passphrase) */
static JSValue sc_load_pkcs12(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  SecureContext *sc = sc_ctx_of(ctx, this_val);
  BIO *in;
  PKCS12 *p12 = NULL;
  EVP_PKEY *pkey = NULL;
  X509 *cert = NULL;
  STACK_OF(X509) *extra = NULL;
  char *pass = NULL;
  bool ok = false;
  int i;
  if (!sc)
    return JS_EXCEPTION;
  if (argc < 1)
    return node_throw_type_error(ctx, "ERR_MISSING_ARGS", "PFX certificate argument is mandatory");
  in = load_bio(ctx, ARG(0));
  if (!in)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Unable to load PFX certificate");
  if (argc >= 2 && crypto_is_buffer_source(ctx, ARG(1))) {
    size_t l;
    uint8_t *d = crypto_buffer_source(ctx, ARG(1), &l);
    pass = malloc(l + 1);
    memcpy(pass, d, l);
    pass[l] = 0;
  }
  if (d2i_PKCS12_bio(in, &p12) && PKCS12_parse(p12, pass ? pass : "", &pkey, &cert, &extra)) {
    if (!pkey || !cert) {
      BIO_free(in);
      PKCS12_free(p12);
      EVP_PKEY_free(pkey);
      X509_free(cert);
      sk_X509_pop_free(extra, X509_free);
      free(pass);
      return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED",
                               !pkey ? "Unable to load private key from PFX data"
                                     : "Unable to load certificate from PFX data");
    }
    ok = use_cert_chain(sc, cert, extra) && SSL_CTX_use_PrivateKey(sc->ctx, pkey);
    for (i = 0; ok && extra && i < sk_X509_num(extra); i++) {
      X509_STORE_add_cert(sc_own_store(sc), sk_X509_value(extra, i));
      SSL_CTX_add_client_CA(sc->ctx, sk_X509_value(extra, i));
    }
  }
  BIO_free(in);
  PKCS12_free(p12);
  EVP_PKEY_free(pkey);
  X509_free(cert);
  sk_X509_pop_free(extra, X509_free);
  free(pass);
  if (!ok) {
    unsigned long err = ERR_get_error();
    const char *str;
    if (ERR_GET_REASON(err) == ERR_R_UNSUPPORTED)
      return crypto_throw_code(ctx, "ERR_CRYPTO_UNSUPPORTED_OPERATION", "Unsupported PKCS12 PFX data");
    str = ERR_reason_error_string(err);
    return JS_ThrowPlainError(ctx, "%s", str ? str : "Unknown error");
  }
  return JS_UNDEFINED;
}

static JSValue sc_get_ticket_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  SecureContext *sc = sc_of(ctx, this_val);
  uint8_t buf[48];
  if (!sc)
    return JS_EXCEPTION;
  memcpy(buf, sc->ticket_key_name, 16);
  memcpy(buf + 16, sc->ticket_key_hmac, 16);
  memcpy(buf + 32, sc->ticket_key_aes, 16);
  return nb_new_buffer(ctx, buf, 48);
}

static JSValue sc_set_ticket_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  SecureContext *sc = sc_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  if (!sc)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  if (!d || len != 48)
    return JS_ThrowTypeError(ctx, "Ticket keys must be 48 bytes");
  memcpy(sc->ticket_key_name, d, 16);
  memcpy(sc->ticket_key_hmac, d + 16, 16);
  memcpy(sc->ticket_key_aes, d + 32, 16);
  return JS_TRUE;
}

static JSValue sc_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue sc_engine_unsupported(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return crypto_throw_code(ctx, "ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED",
                           "Custom engines not supported by this OpenSSL");
}

static JSValue sc_get_cert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic) {
  SecureContext *sc = sc_of(ctx, this_val);
  X509 *x;
  int n;
  uint8_t *buf, *p;
  JSValue r;
  if (!sc)
    return JS_EXCEPTION;
  x = magic ? sc->issuer : sc->cert;
  if (!x)
    return JS_NULL;
  n = i2d_X509(x, NULL);
  buf = malloc(n);
  p = buf;
  i2d_X509(x, &p);
  r = nb_new_buffer(ctx, buf, n);
  free(buf);
  return r;
}

static JSValue sc_set_cert_compression(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue sc_external(JSContext *ctx, JSValueConst this_val) {
  SecureContext *sc = JS_GetOpaque(this_val, secure_context_class_id);
  return JS_NewBigUint64(ctx, sc ? (uintptr_t)sc->ctx : 0);
}

static const JSCFunctionListEntry sc_proto_funcs[] = {
  JS_CFUNC_DEF("init", 3, sc_init),
  JS_CFUNC_DEF("setKey", 2, sc_set_key),
  JS_CFUNC_DEF("setCert", 1, sc_set_cert),
  JS_CFUNC_DEF("addCACert", 1, sc_add_ca_cert),
  JS_CFUNC_DEF("setAllowPartialTrustChain", 0, sc_set_allow_partial),
  JS_CFUNC_DEF("addCRL", 1, sc_add_crl),
  JS_CFUNC_DEF("addRootCerts", 0, sc_add_root_certs),
  JS_CFUNC_MAGIC_DEF("setCipherSuites", 1, sc_set_str, 0),
  JS_CFUNC_MAGIC_DEF("setCiphers", 1, sc_set_str, 1),
  JS_CFUNC_MAGIC_DEF("setSigalgs", 1, sc_set_str, 2),
  JS_CFUNC_MAGIC_DEF("setECDHCurve", 1, sc_set_str, 3),
  JS_CFUNC_MAGIC_DEF("setSessionIdContext", 1, sc_set_str, 4),
  JS_CFUNC_DEF("setDHParam", 1, sc_set_dh_param),
  JS_CFUNC_MAGIC_DEF("setMinProto", 1, sc_proto, 0),
  JS_CFUNC_MAGIC_DEF("setMaxProto", 1, sc_proto, 1),
  JS_CFUNC_MAGIC_DEF("getMinProto", 0, sc_proto, 2),
  JS_CFUNC_MAGIC_DEF("getMaxProto", 0, sc_proto, 3),
  JS_CFUNC_DEF("setOptions", 1, sc_set_options),
  JS_CFUNC_DEF("setSessionTimeout", 1, sc_set_session_timeout),
  JS_CFUNC_DEF("setCertificateCompression", 1, sc_set_cert_compression),
  JS_CFUNC_DEF("close", 0, sc_close),
  JS_CFUNC_DEF("loadPKCS12", 2, sc_load_pkcs12),
  JS_CFUNC_DEF("setTicketKeys", 1, sc_set_ticket_keys),
  JS_CFUNC_DEF("enableTicketKeyCallback", 0, sc_noop),
  JS_CFUNC_DEF("getTicketKeys", 0, sc_get_ticket_keys),
  JS_CFUNC_MAGIC_DEF("getCertificate", 0, sc_get_cert, 0),
  JS_CFUNC_MAGIC_DEF("getIssuer", 0, sc_get_cert, 1),
  JS_CFUNC_DEF("setEngineKey", 2, sc_engine_unsupported),
  JS_CFUNC_DEF("setClientCertEngine", 1, sc_engine_unsupported),
  JS_CGETSET_DEF("_external", sc_external, NULL),
};

/* ---------------------------------------------------------------------- */
/* TLSWrap */

#define kClearOutChunkSize 16384
#define kMaxWriteChunk (1 << 20)

typedef struct TLSWrap {
  StreamWrap s; /* the JS-facing stream: must be first */
  Env *env;
  StreamWrap *under;
  JSValue under_obj, sc_obj, sni_obj;
  SecureContext *sc;
  SSL *ssl;
  BIO *enc_in, *enc_out;
  bool is_server, started, established, eof, shutdown, in_dowrite, destroyed;
  bool write_callback_scheduled, awaiting_new_session, session_callbacks;
  bool alpn_callback_enabled, cert_cb_waiting, cert_cb_running;
  int cycle_depth;
  JSValue current_write, current_empty_write;
  uint8_t *pending;
  size_t pending_len;
  size_t write_size; /* encrypted bytes handed to the underlying stream */
  uint8_t *alpn_protos;
  size_t alpn_len;
  JSValue ocsp_response;
  SSL_SESSION *next_sess;
  char error[256];
  /* deferred work (Node's SetImmediate) */
  uv_idle_t immediate;
  bool immediate_inited, pending_after_write, pending_invoke, pending_empty_done;
  /* the underlying stream's listener we replaced */
  void (*prev_alloc)(StreamWrap *, size_t, uv_buf_t *);
  void (*prev_read)(StreamWrap *, ssize_t, const uv_buf_t *);
  void *prev_listener;
  bool attached;
} TLSWrap;

static void tls_cycle(TLSWrap *t);
static void tls_enc_out(TLSWrap *t);
static void tls_clear_in(TLSWrap *t);
static void tls_clear_out(TLSWrap *t);
static void tls_after_under_write(TLSWrap *t, int status);

static JSValue tls_callback(TLSWrap *t, const char *name, int argc, JSValueConst *argv) {
  return async_wrap_make_callback_name(&t->s.hw.aw, name, argc, argv);
}

static void tls_callback_void(TLSWrap *t, const char *name, int argc, JSValueConst *argv) {
  JSValue r = tls_callback(t, name, argc, argv);
  if (!JS_IsException(r) && !JS_IsUninitialized(r))
    JS_FreeValue(t->env->ctx, r);
}

/* a WriteWrap's Done(status, error) */
static void write_done(TLSWrap *t, JSValue req, int status, const char *error) {
  JSContext *ctx = t->env->ctx;
  JSValue args[3];
  if (error)
    JS_SetPropertyStr(ctx, req, "error", JS_NewString(ctx, error));
  args[0] = JS_NewInt32(ctx, status);
  args[1] = JS_DupValue(ctx, t->s.hw.aw.object);
  args[2] = JS_UNDEFINED;
  node_req_complete(t->env, req, 3, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
}

static void invoke_queued(TLSWrap *t, int status, const char *error) {
  JSValue req;
  if (!t->write_callback_scheduled)
    return;
  if (!JS_IsUndefined(t->current_write)) {
    req = t->current_write;
    t->current_write = JS_UNDEFINED;
    write_done(t, req, status, error);
    JS_FreeValue(t->env->ctx, req);
  }
}

static void immediate_cb(uv_idle_t *h) {
  TLSWrap *t = h->data;
  JSContext *ctx = t->env->ctx;
  JSValue keep = JS_DupValue(ctx, t->s.hw.aw.object);
  uv_idle_stop(h);
  if (t->pending_empty_done) {
    JSValue req = t->current_empty_write;
    t->pending_empty_done = false;
    t->current_empty_write = JS_UNDEFINED;
    if (!JS_IsUndefined(req)) {
      write_done(t, req, 0, NULL);
      JS_FreeValue(ctx, req);
    }
  }
  if (t->pending_after_write) {
    t->pending_after_write = false;
    tls_after_under_write(t, 0);
  }
  if (t->pending_invoke) {
    t->pending_invoke = false;
    invoke_queued(t, 0, NULL);
  }
  async_wrap_unref(&t->s.hw.aw);
  JS_FreeValue(ctx, keep);
}

static void set_immediate(TLSWrap *t) {
  if (!t->immediate_inited) {
    uv_idle_init(t->env->loop, &t->immediate);
    t->immediate.data = t;
    t->immediate_inited = true;
  }
  if (!uv_is_active((uv_handle_t *)&t->immediate)) {
    async_wrap_ref(&t->s.hw.aw);
    uv_idle_start(&t->immediate, immediate_cb);
  }
}

/* ---- writing encrypted data to the underlying stream ---- */

typedef struct {
  uv_write_t req;
  TLSWrap *t;
  char *data;
} UnderWrite;

static void under_write_cb(uv_write_t *req, int status) {
  UnderWrite *w = req->data;
  TLSWrap *t = w->t;
  JSContext *ctx = t->env->ctx;
  JSValue keep = JS_DupValue(ctx, t->s.hw.aw.object);
  free(w->data);
  free(w);
  env_internal_callback_scope_enter(t->env);
  tls_after_under_write(t, status);
  env_internal_callback_scope_exit(t->env, false);
  async_wrap_unref(&t->s.hw.aw);
  JS_FreeValue(ctx, keep);
}

/* 0 and *async; or an error */
static int under_write(TLSWrap *t, char *data, size_t len, bool *async) {
  StreamWrap *u = t->under;
  uv_buf_t buf = uv_buf_init(data, (unsigned)len);
  UnderWrite *w;
  int r;
  *async = false;
  if (!u || u->hw.state != HW_INITIALIZED || !u->stream) {
    free(data);
    return UV_EBADF;
  }
  r = uv_try_write(u->stream, &buf, 1);
  if (r >= 0) {
    u->bytes_written += r;
    if ((size_t)r == len) {
      free(data);
      return 0;
    }
  } else if (r != UV_EAGAIN && r != UV_ENOSYS) {
    free(data);
    return r;
  } else {
    r = 0;
  }
  w = calloc(1, sizeof(*w));
  w->t = t;
  w->data = data;
  w->req.data = w;
  buf = uv_buf_init(data + r, (unsigned)(len - r));
  r = uv_write(&w->req, u->stream, &buf, 1, under_write_cb);
  if (r) {
    free(data);
    free(w);
    return r;
  }
  u->bytes_written += buf.len;
  async_wrap_ref(&t->s.hw.aw);
  *async = true;
  return 0;
}

/* ---- the TLS engine (EncOut / ClearOut / ClearIn / Cycle) ---- */

static void tls_enc_out(TLSWrap *t) {
  int pending;
  char *data;
  bool async;
  int err;
  if (t->write_size != 0 || t->awaiting_new_session)
    return;
  if (t->established && !JS_IsUndefined(t->current_write))
    t->write_callback_scheduled = true;
  if (!t->ssl)
    return;
  pending = (int)BIO_pending(t->enc_out);
  if (pending == 0) {
    if (!t->pending || t->pending_len == 0) {
      if (!t->in_dowrite) {
        invoke_queued(t, 0, NULL);
      } else {
        t->pending_invoke = true;
        set_immediate(t);
      }
    }
    return;
  }
  data = malloc(pending);
  pending = BIO_read(t->enc_out, data, pending);
  t->write_size = pending > 0 ? pending : 0;
  if (pending <= 0) {
    free(data);
    t->write_size = 0;
    return;
  }
  err = under_write(t, data, pending, &async);
  if (err) {
    t->write_size = 0;
    invoke_queued(t, err, NULL);
    return;
  }
  if (!async) {
    t->pending_after_write = true;
    set_immediate(t);
  }
}

static void tls_after_under_write(TLSWrap *t, int status) {
  if (!t->ssl)
    status = UV_ECANCELED;
  t->write_size = 0;
  if (status) {
    if (t->shutdown)
      return;
    invoke_queued(t, status, NULL);
    return;
  }
  tls_clear_in(t);
  tls_enc_out(t);
}

static void ssl_error_string(char *buf, size_t size) {
  unsigned long e;
  buf[0] = 0;
  /* the last (most specific) error, as ERR_print_errors would end with */
  while ((e = ERR_get_error()) != 0) {
    char line[256];
    ERR_error_string_n(e, line, sizeof(line));
    snprintf(buf, size, "%s:../deps/openssl/openssl/ssl/record/rec_layer_s3.c:0:\n", line);
  }
}

static void tls_clear_out(TLSWrap *t) {
  JSContext *ctx = t->env->ctx;
  int read;
  if (t->eof || !t->ssl)
    return;
  for (;;) {
    char *out = malloc(kClearOutChunkSize);
    read = SSL_read(t->ssl, out, kClearOutChunkSize);
    if (read <= 0) {
      free(out);
      break;
    }
    node_stream_emit_read(&t->s, read, out);
    if (!t->ssl)
      return;
  }
  {
    int err = SSL_get_error(t->ssl, read);
    JSValue error;
    switch (err) {
    case SSL_ERROR_ZERO_RETURN:
      if (!t->eof) {
        t->eof = true;
        node_stream_emit_read(&t->s, UV_EOF, NULL);
      }
      return;
    case SSL_ERROR_SSL:
    case SSL_ERROR_SYSCALL: {
      unsigned long e = ERR_peek_error();
      const char *ls = ERR_lib_error_string(e), *rs = ERR_reason_error_string(e);
      char msg[512];
      ssl_error_string(msg, sizeof(msg));
      error = JS_NewError(ctx);
      JS_DefinePropertyValueStr(ctx, error, "message", JS_NewString(ctx, msg),
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
      if (ls)
        JS_SetPropertyStr(ctx, error, "library", JS_NewString(ctx, ls));
      JS_SetPropertyStr(ctx, error, "reason", rs ? JS_NewString(ctx, rs) : JS_UNDEFINED);
      if (rs) {
        char code[256];
        size_t i;
        snprintf(code, sizeof(code), "ERR_SSL_%s", rs);
        for (i = 8; code[i]; i++)
          code[i] = code[i] == ' ' ? '_' : (char)toupper((unsigned char)code[i]);
        JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
      }
      break;
    }
    default:
      return;
    }
    if (BIO_pending(t->enc_out) != 0)
      tls_enc_out(t);
    tls_callback_void(t, "onerror", 1, (JSValueConst *)&error);
    JS_FreeValue(ctx, error);
  }
}

static void tls_clear_in(TLSWrap *t) {
  int written, err;
  uint8_t *data;
  size_t len;
  if (!t->ssl || !t->pending || t->pending_len == 0)
    return;
  data = t->pending;
  len = t->pending_len;
  t->pending = NULL;
  t->pending_len = 0;
  written = SSL_write(t->ssl, data, (int)len);
  if (written != -1 && written == (int)len) {
    free(data);
    return;
  }
  err = SSL_get_error(t->ssl, written);
  if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
    char msg[512];
    ssl_error_string(msg, sizeof(msg));
    free(data);
    t->write_callback_scheduled = true;
    invoke_queued(t, UV_EPROTO, msg);
    return;
  }
  t->pending = data;
  t->pending_len = len;
  ERR_clear_error();
}

static void tls_cycle(TLSWrap *t) {
  if (++t->cycle_depth > 1)
    return;
  for (; t->cycle_depth > 0; t->cycle_depth--) {
    tls_clear_in(t);
    tls_clear_out(t);
    tls_enc_out(t);
  }
}

/* ---- the underlying stream's listener ---- */

static void under_alloc(StreamWrap *u, size_t suggested, uv_buf_t *buf) {
  buf->base = malloc(65536);
  buf->len = buf->base ? 65536 : 0;
}

static void under_read(StreamWrap *u, ssize_t nread, const uv_buf_t *buf) {
  TLSWrap *t = u->listener;
  JSContext *ctx = t->env->ctx;
  JSValue keep;
  if (t->eof) {
    free(buf->base);
    return;
  }
  keep = JS_DupValue(ctx, t->s.hw.aw.object);
  env_internal_callback_scope_enter(t->env);
  if (nread < 0) {
    free(buf->base);
    tls_clear_out(t);
    if (nread == UV_EOF)
      t->eof = true;
    node_stream_emit_read(&t->s, nread, NULL);
  } else if (nread > 0 && t->ssl) {
    u->bytes_read += nread;
    BIO_write(t->enc_in, buf->base, (int)nread);
    free(buf->base);
    tls_cycle(t);
  } else {
    free(buf->base);
  }
  env_internal_callback_scope_exit(t->env, false);
  JS_FreeValue(ctx, keep);
}

static void tls_detach(TLSWrap *t) {
  if (!t->attached || !t->under)
    return;
  t->under->alloc_override = t->prev_alloc;
  t->under->read_override = t->prev_read;
  t->under->listener = t->prev_listener;
  t->attached = false;
}

static void tls_destroy(TLSWrap *t) {
  if (!t->ssl)
    return;
  t->write_callback_scheduled = true;
  invoke_queued(t, UV_ECANCELED, "Canceled because of SSL destruction");
  SSL_free(t->ssl); /* frees the BIOs */
  t->ssl = NULL;
  t->enc_in = t->enc_out = NULL;
  tls_detach(t);
  free(t->pending);
  t->pending = NULL;
  t->pending_len = 0;
}

/* ---- StreamBase overrides ---- */

static int tls_read_start(StreamWrap *s) {
  TLSWrap *t = (TLSWrap *)s;
  if (t->under && !t->eof)
    return node_stream_read_start(t->under);
  return 0;
}

static int tls_read_stop(StreamWrap *s) {
  TLSWrap *t = (TLSWrap *)s;
  return t->under ? node_stream_read_stop(t->under) : 0;
}

static int tls_do_write(StreamWrap *s, JSValueConst req, uv_buf_t *bufs, unsigned nbufs,
                        JSValueConst keep) {
  TLSWrap *t = (TLSWrap *)s;
  Env *env = t->env;
  JSContext *ctx = env->ctx;
  size_t length = 0, i, off = 0;
  int written;
  uint8_t *data;
  for (i = 0; i < nbufs; i++)
    length += bufs[i].len;
  env->stream_base_state[kBytesWritten] = (int32_t)length;
  env->stream_base_state[kLastWriteWasAsync] = 0;
  if (!t->ssl) {
    JS_SetPropertyStr(ctx, req, "error", JS_NewString(ctx, "Write after DestroySSL"));
    return UV_EPROTO;
  }
  if (length == 0) {
    tls_clear_out(t);
    if (BIO_pending(t->enc_out) == 0) {
      /* an empty write: completes after the underlying stream's turn */
      JS_FreeValue(ctx, t->current_empty_write);
      t->current_empty_write = JS_DupValue(ctx, req);
      t->pending_empty_done = true;
      set_immediate(t);
      env->stream_base_state[kLastWriteWasAsync] = 1;
      return 0;
    }
  }
  JS_FreeValue(ctx, t->current_write);
  t->current_write = JS_DupValue(ctx, req);
  env->stream_base_state[kLastWriteWasAsync] = 1;
  if (length == 0) {
    tls_enc_out(t);
    return 0;
  }
  data = malloc(length);
  for (i = 0; i < nbufs; i++) {
    memcpy(data + off, bufs[i].base, bufs[i].len);
    off += bufs[i].len;
  }
  written = SSL_write(t->ssl, data, (int)length);
  if (written == -1) {
    int err = SSL_get_error(t->ssl, written);
    if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
      free(data);
      JS_FreeValue(ctx, t->current_write);
      t->current_write = JS_UNDEFINED;
      ERR_clear_error();
      env->stream_base_state[kLastWriteWasAsync] = 0;
      return UV_EPROTO;
    }
    free(t->pending);
    t->pending = data;
    t->pending_len = length;
    ERR_clear_error();
  } else {
    free(data);
  }
  t->in_dowrite = true;
  tls_enc_out(t);
  t->in_dowrite = false;
  return 0;
}

static int tls_do_shutdown(StreamWrap *s, JSValueConst req) {
  TLSWrap *t = (TLSWrap *)s;
  if (t->ssl && SSL_shutdown(t->ssl) == 0)
    SSL_shutdown(t->ssl);
  ERR_clear_error();
  t->shutdown = true;
  tls_enc_out(t);
  return t->under ? node_stream_shutdown(t->under, req) : UV_EBADF;
}

/* ---- OpenSSL callbacks ---- */

static void info_cb(const SSL *ssl, int where, int ret) {
  TLSWrap *t = SSL_get_app_data(ssl);
  JSContext *ctx;
  if (!(where & (SSL_CB_HANDSHAKE_START | SSL_CB_HANDSHAKE_DONE)) || !t)
    return;
  ctx = t->env->ctx;
  if (where & SSL_CB_HANDSHAKE_START) {
    JSValue now = JS_NewFloat64(ctx, (double)uv_now(t->env->loop));
    tls_callback_void(t, "onhandshakestart", 1, (JSValueConst *)&now);
  }
  if ((where & SSL_CB_HANDSHAKE_DONE) && !SSL_renegotiate_pending((SSL *)ssl)) {
    t->established = true;
    tls_callback_void(t, "onhandshakedone", 0, NULL);
  }
}

static int verify_cb(int preverify_ok, X509_STORE_CTX *x) {
  return 1; /* checked later by verifyError() */
}

static SSL_SESSION *get_session_cb(SSL *s, const unsigned char *key, int len, int *copy) {
  TLSWrap *t = SSL_get_app_data(s);
  SSL_SESSION *sess = t ? t->next_sess : NULL;
  *copy = 0;
  if (t)
    t->next_sess = NULL;
  return sess;
}

static int new_session_cb(SSL *s, SSL_SESSION *sess) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  JSValue args[2];
  int size;
  uint8_t *buf, *p;
  unsigned idlen;
  const unsigned char *id;
  if (!t || !t->session_callbacks)
    return 0;
  ctx = t->env->ctx;
  size = i2d_SSL_SESSION(sess, NULL);
  if (size <= 0 || size > 10 * 1024)
    return 0;
  buf = malloc(size);
  p = buf;
  i2d_SSL_SESSION(sess, &p);
  id = SSL_SESSION_get_id(sess, &idlen);
  args[0] = nb_new_buffer(ctx, id, idlen);
  args[1] = nb_new_buffer(ctx, buf, size);
  free(buf);
  if (t->is_server)
    t->awaiting_new_session = true;
  tls_callback_void(t, "onnewsession", 2, (JSValueConst *)args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  return 0;
}

static void keylog_cb(const SSL *s, const char *line) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  size_t n = strlen(line);
  char *buf;
  JSValue b;
  if (!t)
    return;
  ctx = t->env->ctx;
  buf = malloc(n + 1);
  memcpy(buf, line, n);
  buf[n] = '\n';
  b = nb_new_buffer(ctx, buf, n + 1);
  free(buf);
  tls_callback_void(t, "onkeylog", 1, (JSValueConst *)&b);
  JS_FreeValue(ctx, b);
}

static int cert_cb(SSL *s, void *arg) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  JSValue info;
  const char *sn;
  if (!t || !t->is_server || !t->cert_cb_waiting)
    return 1;
  if (t->cert_cb_running)
    return -1;
  ctx = t->env->ctx;
  t->cert_cb_running = true;
  info = JS_NewObject(ctx);
  sn = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
  JS_SetPropertyStr(ctx, info, "servername", JS_NewString(ctx, sn ? sn : ""));
  JS_SetPropertyStr(ctx, info, "OCSPRequest",
                    JS_NewBool(ctx, SSL_get_tlsext_status_type(s) == TLSEXT_STATUSTYPE_ocsp));
  tls_callback_void(t, "oncertcb", 1, (JSValueConst *)&info);
  JS_FreeValue(ctx, info);
  return t->cert_cb_running ? -1 : 1;
}

static int alpn_select_cb(SSL *s, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg) {
  TLSWrap *t = SSL_get_app_data(s);
  if (!t)
    return SSL_TLSEXT_ERR_NOACK;
  if (t->alpn_callback_enabled) {
    JSContext *ctx = t->env->ctx;
    JSValue a = nb_new_buffer(ctx, in, inlen), r;
    double idx;
    r = tls_callback(t, "ALPNCallback", 1, (JSValueConst *)&a);
    JS_FreeValue(ctx, a);
    if (JS_IsException(r) || JS_IsUninitialized(r) || !JS_IsNumber(r)) {
      if (!JS_IsException(r) && !JS_IsUninitialized(r))
        JS_FreeValue(ctx, r);
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    JS_ToFloat64(ctx, &idx, r);
    *outlen = in[(unsigned)idx];
    *out = in + (unsigned)idx + 1;
    return SSL_TLSEXT_ERR_OK;
  }
  if (!t->alpn_protos || !t->alpn_len)
    return SSL_TLSEXT_ERR_NOACK;
  return SSL_select_next_proto((unsigned char **)out, outlen, t->alpn_protos,
                               (unsigned)t->alpn_len, in, inlen) == OPENSSL_NPN_NEGOTIATED
             ? SSL_TLSEXT_ERR_OK
             : SSL_TLSEXT_ERR_ALERT_FATAL;
}

static int status_cb(SSL *s, void *arg) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  if (!t)
    return 1;
  ctx = t->env->ctx;
  if (!t->is_server) {
    const unsigned char *resp;
    long len = SSL_get_tlsext_status_ocsp_resp(s, &resp);
    JSValue a = resp ? nb_new_buffer(ctx, resp, len) : JS_NULL;
    tls_callback_void(t, "onocspresponse", 1, (JSValueConst *)&a);
    JS_FreeValue(ctx, a);
    return 1;
  }
  if (JS_IsUndefined(t->ocsp_response))
    return SSL_TLSEXT_ERR_NOACK;
  {
    size_t len;
    uint8_t *d = crypto_buffer_source(ctx, t->ocsp_response, &len), *copy;
    copy = OPENSSL_malloc(len ? len : 1);
    memcpy(copy, d, len);
    if (!SSL_set_tlsext_status_ocsp_resp(s, copy, (long)len))
      OPENSSL_free(copy);
    JS_FreeValue(ctx, t->ocsp_response);
    t->ocsp_response = JS_UNDEFINED;
  }
  return SSL_TLSEXT_ERR_OK;
}

static int set_ca_certs(TLSWrap *t, SecureContext *sc) {
  STACK_OF(X509_NAME) *list;
  if (SSL_set1_verify_cert_store(t->ssl, SSL_CTX_get_cert_store(sc->ctx)) != 1)
    return 0;
  list = SSL_dup_CA_list(SSL_CTX_get_client_CA_list(sc->ctx));
  SSL_set_client_CA_list(t->ssl, list);
  return 1;
}

static int sni_cb(SSL *s, int *ad, void *arg) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  const char *sn;
  JSValue owner, sc_obj;
  SecureContext *sc;
  if (!t)
    return SSL_TLSEXT_ERR_NOACK;
  ctx = t->env->ctx;
  sn = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
  if (!sn)
    return SSL_TLSEXT_ERR_NOACK;
  /* GetOwner(): the JS socket (owner_symbol) */
  {
    JSValue sym = env_get_symbol(t->env, "owner_symbol");
    JSAtom a = JS_ValueToAtom(ctx, sym);
    owner = JS_GetProperty(ctx, t->s.hw.aw.object, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, sym);
  }
  if (JS_IsObject(owner))
    JS_SetPropertyStr(ctx, owner, "servername", JS_NewString(ctx, sn));
  JS_FreeValue(ctx, owner);
  sc_obj = JS_GetPropertyStr(ctx, t->s.hw.aw.object, "sni_context");
  if (!JS_IsObject(sc_obj)) {
    JS_FreeValue(ctx, sc_obj);
    return SSL_TLSEXT_ERR_NOACK;
  }
  sc = JS_GetOpaque(sc_obj, secure_context_class_id);
  if (!sc || !sc->ctx) {
    JSValue err;
    JS_FreeValue(ctx, sc_obj);
    JS_ThrowTypeError(ctx, "Invalid SNI context");
    err = JS_GetException(ctx);
    tls_callback_void(t, "onerror", 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, err);
    return SSL_TLSEXT_ERR_NOACK;
  }
  JS_FreeValue(ctx, t->sni_obj);
  t->sni_obj = sc_obj;
  SSL_CTX_set_tlsext_status_cb(sc->ctx, status_cb);
  SSL_set_SSL_CTX(t->ssl, sc->ctx);
  set_ca_certs(t, sc);
  return SSL_TLSEXT_ERR_OK;
}

#ifndef OPENSSL_NO_PSK
static unsigned int psk_server_cb(SSL *s, const char *identity, unsigned char *psk,
                                  unsigned int max_psk_len) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  JSValue args[2], r, sym, fn;
  JSAtom a;
  size_t len;
  uint8_t *d;
  if (!t)
    return 0;
  ctx = t->env->ctx;
  args[0] = JS_NewString(ctx, identity);
  args[1] = JS_NewUint32(ctx, max_psk_len);
  sym = env_get_symbol(t->env, "onpskexchange");
  a = JS_ValueToAtom(ctx, sym);
  fn = JS_GetProperty(ctx, t->s.hw.aw.object, a);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
  r = JS_IsFunction(ctx, fn) ? async_wrap_make_callback(&t->s.hw.aw, fn, 2, (JSValueConst *)args)
                             : JS_UNDEFINED;
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, args[0]);
  if (JS_IsException(r) || JS_IsUninitialized(r))
    return 0;
  d = nb_buffer_data(ctx, r, &len);
  if (!d || len > max_psk_len) {
    JS_FreeValue(ctx, r);
    return 0;
  }
  memcpy(psk, d, len);
  JS_FreeValue(ctx, r);
  return (unsigned)len;
}

static unsigned int psk_client_cb(SSL *s, const char *hint, char *identity,
                                  unsigned int max_identity_len, unsigned char *psk,
                                  unsigned int max_psk_len) {
  TLSWrap *t = SSL_get_app_data(s);
  JSContext *ctx;
  JSValue args[3], r, sym, fn, pv, iv;
  JSAtom a;
  size_t len;
  uint8_t *d;
  const char *id;
  size_t idlen;
  if (!t)
    return 0;
  ctx = t->env->ctx;
  args[0] = hint ? JS_NewString(ctx, hint) : JS_NULL;
  args[1] = JS_NewUint32(ctx, max_psk_len);
  args[2] = JS_NewUint32(ctx, max_identity_len);
  sym = env_get_symbol(t->env, "onpskexchange");
  a = JS_ValueToAtom(ctx, sym);
  fn = JS_GetProperty(ctx, t->s.hw.aw.object, a);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
  r = JS_IsFunction(ctx, fn) ? async_wrap_make_callback(&t->s.hw.aw, fn, 3, (JSValueConst *)args)
                             : JS_UNDEFINED;
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, args[0]);
  if (JS_IsException(r) || JS_IsUninitialized(r) || !JS_IsObject(r))
    return 0;
  pv = JS_GetPropertyStr(ctx, r, "psk");
  iv = JS_GetPropertyStr(ctx, r, "identity");
  JS_FreeValue(ctx, r);
  d = nb_buffer_data(ctx, pv, &len);
  if (!d || len > max_psk_len || !JS_IsString(iv)) {
    JS_FreeValue(ctx, pv);
    JS_FreeValue(ctx, iv);
    return 0;
  }
  id = JS_ToCStringLen(ctx, &idlen, iv);
  if (idlen > max_identity_len) {
    JS_FreeCString(ctx, id);
    JS_FreeValue(ctx, pv);
    JS_FreeValue(ctx, iv);
    return 0;
  }
  memcpy(identity, id, idlen);
  memcpy(psk, d, len);
  JS_FreeCString(ctx, id);
  JS_FreeValue(ctx, pv);
  JS_FreeValue(ctx, iv);
  return (unsigned)len;
}
#endif

/* ---- the TLSWrap object ---- */

static void tls_finalizer(JSRuntime *rt, JSValueConst val) {
  TLSWrap *t = JS_GetOpaque(val, tls_wrap_class_id);
  if (!t)
    return;
  t->write_callback_scheduled = false; /* no JS from a finalizer */
  if (t->ssl) {
    SSL_free(t->ssl);
    t->ssl = NULL;
  }
  tls_detach(t);
  free(t->pending);
  free(t->alpn_protos);
  if (t->next_sess)
    SSL_SESSION_free(t->next_sess);
  JS_FreeValueRT(rt, t->under_obj);
  JS_FreeValueRT(rt, t->sc_obj);
  JS_FreeValueRT(rt, t->sni_obj);
  JS_FreeValueRT(rt, t->current_write);
  JS_FreeValueRT(rt, t->current_empty_write);
  JS_FreeValueRT(rt, t->ocsp_response);
  async_wrap_destroy(&t->s.hw.aw);
  if (t->immediate_inited) {
    /* the handle is inside t: free t when libuv is done with it */
    uv_idle_stop(&t->immediate);
    node_close_and_free((uv_handle_t *)&t->immediate, t);
    return;
  }
  free(t);
}

static void tls_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  TLSWrap *t = JS_GetOpaque(val, tls_wrap_class_id);
  if (!t)
    return;
  JS_MarkValue(rt, t->under_obj, mark);
  JS_MarkValue(rt, t->sc_obj, mark);
  JS_MarkValue(rt, t->sni_obj, mark);
  JS_MarkValue(rt, t->current_write, mark);
  JS_MarkValue(rt, t->current_empty_write, mark);
  JS_MarkValue(rt, t->ocsp_response, mark);
}

static JSValue tls_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* wrap(handle, secureContext, isServer, hasActiveWriteFromPrevOwner) */
static JSValue tls_wrap_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  StreamWrap *under = node_stream_wrap_of(ARG(0));
  SecureContext *sc = JS_GetOpaque(ARG(1), secure_context_class_id);
  JSValue obj;
  TLSWrap *t;
  if (!under)
    return JS_ThrowTypeError(ctx, "handle must be a native stream");
  if (!sc || !sc->ctx)
    return JS_ThrowTypeError(ctx, "Must give a SecureContext as second argument");
  obj = JS_NewObjectClass(ctx, tls_wrap_class_id);
  if (JS_IsException(obj))
    return obj;
  t = calloc(1, sizeof(*t));
  t->env = env;
  t->under = under;
  t->under_obj = JS_DupValue(ctx, ARG(0));
  t->sc_obj = JS_DupValue(ctx, ARG(1));
  t->sni_obj = JS_UNDEFINED;
  t->sc = sc;
  t->is_server = JS_ToBool(ctx, ARG(2));
  t->current_write = t->current_empty_write = t->ocsp_response = JS_UNDEFINED;
  async_wrap_init(&t->s.hw.aw, env, obj, PROVIDER_TLSWRAP, -1);
  t->s.hw.state = HW_INITIALIZED;
  t->s.hw.handle = NULL;
  t->s.stream = NULL;
  t->s.write_override = tls_do_write;
  t->s.shutdown_override = tls_do_shutdown;
  t->s.read_start_override = tls_read_start;
  t->s.read_stop_override = tls_read_stop;
  JS_SetOpaque(obj, t);

  t->ssl = SSL_new(sc->ctx);
  if (!t->ssl) {
    JS_FreeValue(ctx, obj);
    return crypto_throw(ctx, ERR_get_error(), "SSL_new");
  }
  SSL_CTX_sess_set_get_cb(sc->ctx, get_session_cb);
  SSL_CTX_sess_set_new_cb(sc->ctx, new_session_cb);
  t->enc_in = BIO_new(BIO_s_mem());
  t->enc_out = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(t->enc_in, -1);
  BIO_set_mem_eof_return(t->enc_out, -1);
  SSL_set_bio(t->ssl, t->enc_in, t->enc_out);
  SSL_set_verify(t->ssl, SSL_VERIFY_NONE, verify_cb);
  SSL_set_mode(t->ssl, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_AUTO_RETRY);
  SSL_set_app_data(t->ssl, t);
  SSL_set_info_callback(t->ssl, info_cb);
  if (t->is_server)
    SSL_CTX_set_tlsext_servername_callback(sc->ctx, sni_cb);
  SSL_CTX_set_tlsext_status_cb(sc->ctx, status_cb);
  SSL_set_cert_cb(t->ssl, cert_cb, t);
  if (t->is_server)
    SSL_set_accept_state(t->ssl);
  else
    SSL_set_connect_state(t->ssl);

  /* become the underlying stream's listener */
  t->prev_alloc = under->alloc_override;
  t->prev_read = under->read_override;
  t->prev_listener = under->listener;
  under->alloc_override = under_alloc;
  under->read_override = under_read;
  under->listener = t;
  t->attached = true;
  return obj;
}

static TLSWrap *tls_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, tls_wrap_class_id);
}

#define TLS_SSL(t)                                                     \
  do {                                                                 \
    if (!(t))                                                          \
      return JS_EXCEPTION;                                             \
    if (!(t)->ssl)                                                     \
      return JS_ThrowTypeError(ctx, "TLS socket is already destroyed"); \
  } while (0)

static JSValue tls_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  t->started = true;
  tls_clear_out(t);
  tls_enc_out(t);
  return JS_UNDEFINED;
}

/* receive(buffer): data from a JS stream */
static JSValue tls_receive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  TLS_SSL(t);
  d = crypto_buffer_source(ctx, ARG(0), &len);
  if (d && len) {
    BIO_write(t->enc_in, d, (int)len);
    tls_cycle(t);
  }
  return JS_UNDEFINED;
}

static JSValue tls_set_verify_mode(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  int mode = SSL_VERIFY_NONE;
  TLS_SSL(t);
  if (t->is_server && JS_ToBool(ctx, ARG(0))) {
    mode = SSL_VERIFY_PEER;
    if (JS_ToBool(ctx, ARG(1)))
      mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  SSL_set_verify(t->ssl, mode, verify_cb);
  return JS_UNDEFINED;
}

static JSValue tls_enable_session_callbacks(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  t->session_callbacks = true;
  return JS_UNDEFINED;
}

static JSValue tls_enable_keylog(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  if (!t)
    return JS_EXCEPTION;
  SSL_CTX_set_keylog_callback(t->sc->ctx, keylog_cb);
  return JS_UNDEFINED;
}

static JSValue tls_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue tls_destroy_ssl(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  if (!t)
    return JS_EXCEPTION;
  tls_destroy(t);
  return JS_UNDEFINED;
}

static JSValue tls_enable_cert_cb(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  if (!t)
    return JS_EXCEPTION;
  t->cert_cb_waiting = true;
  return JS_UNDEFINED;
}

static JSValue tls_cert_cb_done(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  JSValue sc_obj;
  TLS_SSL(t);
  sc_obj = JS_GetPropertyStr(ctx, this_val, "sni_context");
  if (JS_IsObject(sc_obj)) {
    SecureContext *sc = JS_GetOpaque(sc_obj, secure_context_class_id);
    if (sc && sc->ctx) {
      JS_FreeValue(ctx, t->sni_obj);
      t->sni_obj = JS_DupValue(ctx, sc_obj);
      if (SSL_set_SSL_CTX(t->ssl, sc->ctx) && !set_ca_certs(t, sc)) {
        JS_FreeValue(ctx, sc_obj);
        return crypto_throw(ctx, ERR_get_error(), "CertCbDone");
      }
    } else {
      JSValue err;
      JS_FreeValue(ctx, sc_obj);
      JS_ThrowTypeError(ctx, "Invalid SNI context");
      err = JS_GetException(ctx);
      tls_callback_void(t, "onerror", 1, (JSValueConst *)&err);
      JS_FreeValue(ctx, err);
      return JS_UNDEFINED;
    }
  }
  JS_FreeValue(ctx, sc_obj);
  t->cert_cb_running = false;
  t->cert_cb_waiting = false;
  tls_cycle(t);
  return JS_UNDEFINED;
}

static JSValue tls_enable_alpn_cb(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  t->alpn_callback_enabled = true;
  SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(t->ssl), alpn_select_cb, NULL);
  return JS_UNDEFINED;
}

static JSValue tls_get_servername(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  const char *sn;
  TLS_SSL(t);
  sn = SSL_get_servername(t->ssl, TLSEXT_NAMETYPE_host_name);
  return sn ? JS_NewString(ctx, sn) : JS_FALSE;
}

static JSValue tls_set_servername(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  const char *sn;
  TLS_SSL(t);
  sn = JS_ToCString(ctx, ARG(0));
  if (!sn)
    return JS_EXCEPTION;
  SSL_set_tlsext_host_name(t->ssl, sn);
  JS_FreeCString(ctx, sn);
  return JS_UNDEFINED;
}

static JSValue tls_set_psk_hint(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  const char *h;
  TLS_SSL(t);
  h = JS_ToCString(ctx, ARG(0));
#ifndef OPENSSL_NO_PSK
  if (!SSL_use_psk_identity_hint(t->ssl, h)) {
    JSValue err = crypto_error_code(ctx, "ERR_TLS_PSK_SET_IDENTITY_HINT_FAILED",
                                    "Failed to set PSK identity hint");
    tls_callback_void(t, "onerror", 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, err);
  }
#endif
  JS_FreeCString(ctx, h);
  return JS_UNDEFINED;
}

static JSValue tls_enable_psk(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
#ifndef OPENSSL_NO_PSK
  SSL_set_psk_server_callback(t->ssl, psk_server_cb);
  SSL_set_psk_client_callback(t->ssl, psk_client_cb);
#endif
  return JS_UNDEFINED;
}

static JSValue tls_write_queue_size(JSContext *ctx, JSValueConst this_val) {
  TLSWrap *t = JS_GetOpaque(this_val, tls_wrap_class_id);
  return JS_NewUint32(ctx, t && t->ssl ? (uint32_t)BIO_pending(t->enc_out) : 0);
}

static JSValue tls_fd(JSContext *ctx, JSValueConst this_val) {
  TLSWrap *t = JS_GetOpaque(this_val, tls_wrap_class_id);
  uv_os_fd_t fd = -1;
  if (t && t->under && t->under->hw.handle && t->under->hw.state == HW_INITIALIZED)
    uv_fileno(t->under->hw.handle, &fd);
  return JS_NewInt32(ctx, fd);
}

static JSValue tls_set_alpn_protocols(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  TLS_SSL(t);
  d = nb_buffer_data(ctx, ARG(0), &len);
  if (!d || !nb_is_array_buffer_view(ctx, ARG(0)))
    return JS_ThrowTypeError(ctx, "Must give a Buffer as first argument");
  if (!t->is_server) {
    SSL_set_alpn_protos(t->ssl, d, (unsigned)len);
  } else {
    free(t->alpn_protos);
    t->alpn_protos = malloc(len ? len : 1);
    memcpy(t->alpn_protos, d, len);
    t->alpn_len = len;
    SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(t->ssl), alpn_select_cb, NULL);
  }
  return JS_UNDEFINED;
}

static JSValue tls_set_key_cert(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  SecureContext *sc;
  TLS_SSL(t);
  sc = JS_GetOpaque(ARG(0), secure_context_class_id);
  if (!sc || !sc->ctx)
    return JS_ThrowTypeError(ctx, "Must give a SecureContext as first argument");
  if (!t->is_server)
    return JS_UNDEFINED;
  JS_FreeValue(ctx, t->sni_obj);
  t->sni_obj = JS_DupValue(ctx, ARG(0));
  if (SSL_set_SSL_CTX(t->ssl, sc->ctx) != sc->ctx || !set_ca_certs(t, sc))
    return crypto_throw(ctx, ERR_get_error(), "SetKeyCert");
  return JS_UNDEFINED;
}

/* the legacy dictionaries of the peer chain, linked by issuerCertificate */
static JSValue peer_certificate(JSContext *ctx, TLSWrap *t, bool detailed) {
  X509 *cert = t->is_server ? SSL_get1_peer_certificate(t->ssl) : NULL;
  STACK_OF(X509) *chain = SSL_get_peer_cert_chain(t->ssl), *peers;
  JSValue result, obj;
  int i;
  ERR_set_mark();
  if (!cert && (!chain || sk_X509_num(chain) == 0)) {
    ERR_pop_to_mark();
    return JS_UNDEFINED;
  }
  if (!detailed) {
    result = crypto_x509_to_object(ctx, cert ? cert : sk_X509_value(chain, 0));
    X509_free(cert);
    ERR_pop_to_mark();
    return result;
  }
  /* CloneSSLCerts: the peer cert first, then the chain */
  peers = sk_X509_new_null();
  if (cert)
    sk_X509_push(peers, cert);
  for (i = 0; chain && i < sk_X509_num(chain); i++)
    sk_X509_push(peers, X509_dup(sk_X509_value(chain, i)));
  result = crypto_x509_to_object(ctx, sk_X509_value(peers, 0));
  obj = JS_DupValue(ctx, result);
  cert = sk_X509_shift(peers);
  /* AddIssuerChainToObject */
  for (;;) {
    for (i = 0; i < sk_X509_num(peers); i++) {
      X509 *ca = sk_X509_value(peers, i);
      JSValue ca_info;
      if (X509_check_issued(ca, cert) != X509_V_OK)
        continue;
      ca_info = crypto_x509_to_object(ctx, ca);
      JS_SetPropertyStr(ctx, obj, "issuerCertificate", JS_DupValue(ctx, ca_info));
      JS_FreeValue(ctx, obj);
      obj = ca_info;
      X509_free(cert);
      cert = sk_X509_delete(peers, i);
      break;
    }
    if (i == sk_X509_num(peers))
      break;
  }
  /* GetLastIssuedCert: continue from the trust store */
  while (X509_check_issued(cert, cert) != X509_V_OK) {
    X509_STORE *store = SSL_CTX_get_cert_store(SSL_get_SSL_CTX(t->ssl));
    X509_STORE_CTX *sctx = X509_STORE_CTX_new();
    X509 *ca = NULL;
    JSValue ca_info;
    if (!sctx || X509_STORE_CTX_init(sctx, store, NULL, NULL) != 1 ||
        X509_STORE_CTX_get1_issuer(&ca, sctx, cert) != 1) {
      X509_STORE_CTX_free(sctx);
      break;
    }
    X509_STORE_CTX_free(sctx);
    ca_info = crypto_x509_to_object(ctx, ca);
    JS_SetPropertyStr(ctx, obj, "issuerCertificate", JS_DupValue(ctx, ca_info));
    JS_FreeValue(ctx, obj);
    obj = ca_info;
    if (X509_cmp(cert, ca) == 0) {
      X509_free(ca);
      break;
    }
    X509_free(cert);
    cert = ca;
  }
  if (X509_check_issued(cert, cert) == X509_V_OK)
    JS_SetPropertyStr(ctx, obj, "issuerCertificate", JS_DupValue(ctx, obj));
  JS_FreeValue(ctx, obj);
  X509_free(cert);
  sk_X509_pop_free(peers, X509_free);
  ERR_pop_to_mark();
  return result;
}

static JSValue tls_get_peer_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  return peer_certificate(ctx, t, argc >= 1 && JS_ToBool(ctx, ARG(0)));
}

static JSValue tls_get_peer_x509(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  X509 *cert;
  STACK_OF(X509) *chain, *copy = NULL;
  int i;
  JSValue r;
  TLS_SSL(t);
  cert = t->is_server ? SSL_get1_peer_certificate(t->ssl) : NULL;
  chain = SSL_get_peer_cert_chain(t->ssl);
  if (!cert && (!chain || sk_X509_num(chain) == 0))
    return JS_UNDEFINED;
  copy = sk_X509_new_null();
  for (i = 0; chain && i < sk_X509_num(chain); i++)
    sk_X509_push(copy, X509_dup(sk_X509_value(chain, i)));
  if (!cert)
    cert = sk_X509_shift(copy);
  r = crypto_x509_new_handle(ctx, cert, sk_X509_num(copy) ? copy : NULL);
  sk_X509_pop_free(copy, X509_free);
  return r;
}

static JSValue tls_get_certificate(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  X509 *c;
  TLS_SSL(t);
  c = SSL_get_certificate(t->ssl);
  return c ? crypto_x509_to_object(ctx, c) : JS_UNDEFINED;
}

static JSValue tls_get_x509(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  X509 *c;
  TLS_SSL(t);
  c = SSL_get_certificate(t->ssl);
  return c ? crypto_x509_new_handle(ctx, X509_dup(c), NULL) : JS_UNDEFINED;
}

static JSValue tls_finished(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int peer) {
  TLSWrap *t = tls_of(ctx, this_val);
  char buf[128];
  size_t len;
  TLS_SSL(t);
  len = peer ? SSL_get_peer_finished(t->ssl, buf, sizeof(buf))
             : SSL_get_finished(t->ssl, buf, sizeof(buf));
  if (len == 0)
    return JS_UNDEFINED;
  return nb_new_buffer(ctx, buf, len < sizeof(buf) ? len : sizeof(buf));
}

static JSValue tls_get_session(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  SSL_SESSION *sess;
  int n;
  uint8_t *buf, *p;
  JSValue r;
  TLS_SSL(t);
  sess = SSL_get_session(t->ssl);
  if (!sess)
    return JS_UNDEFINED;
  n = i2d_SSL_SESSION(sess, NULL);
  if (n <= 0)
    return JS_UNDEFINED;
  buf = malloc(n);
  p = buf;
  i2d_SSL_SESSION(sess, &p);
  r = nb_new_buffer(ctx, buf, n);
  free(buf);
  return r;
}

static JSValue tls_set_session(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  size_t len;
  const uint8_t *d;
  SSL_SESSION *sess;
  int ok;
  TLS_SSL(t);
  if (argc < 1)
    return node_throw_type_error(ctx, "ERR_MISSING_ARGS", "Session argument is mandatory");
  d = nb_buffer_data(ctx, ARG(0), &len);
  if (!d)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "Session must be a buffer");
  sess = d2i_SSL_SESSION(NULL, &d, (long)len);
  if (!sess)
    return JS_UNDEFINED;
  ok = SSL_set_session(t->ssl, sess);
  SSL_SESSION_free(sess);
  if (!ok)
    return JS_ThrowPlainError(ctx, "SSL_set_session error");
  return JS_UNDEFINED;
}

static JSValue tls_is_session_reused(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  return JS_NewBool(ctx, SSL_session_reused(t->ssl));
}

static const char *x509_error_code(long err) {
  switch (err) {
#define C(x) case X509_V_ERR_##x: return #x;
  C(UNABLE_TO_GET_ISSUER_CERT) C(UNABLE_TO_GET_CRL) C(UNABLE_TO_DECRYPT_CERT_SIGNATURE)
  C(UNABLE_TO_DECRYPT_CRL_SIGNATURE) C(UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY)
  C(CERT_SIGNATURE_FAILURE) C(CRL_SIGNATURE_FAILURE) C(CERT_NOT_YET_VALID)
  C(CERT_HAS_EXPIRED) C(CRL_NOT_YET_VALID) C(CRL_HAS_EXPIRED)
  C(ERROR_IN_CERT_NOT_BEFORE_FIELD) C(ERROR_IN_CERT_NOT_AFTER_FIELD)
  C(ERROR_IN_CRL_LAST_UPDATE_FIELD) C(ERROR_IN_CRL_NEXT_UPDATE_FIELD) C(OUT_OF_MEM)
  C(DEPTH_ZERO_SELF_SIGNED_CERT) C(SELF_SIGNED_CERT_IN_CHAIN)
  C(UNABLE_TO_GET_ISSUER_CERT_LOCALLY) C(UNABLE_TO_VERIFY_LEAF_SIGNATURE)
  C(CERT_CHAIN_TOO_LONG) C(CERT_REVOKED) C(INVALID_CA) C(PATH_LENGTH_EXCEEDED)
  C(INVALID_PURPOSE) C(CERT_UNTRUSTED) C(CERT_REJECTED) C(HOSTNAME_MISMATCH)
#undef C
  }
  return "UNSPECIFIED";
}

static JSValue tls_verify_error(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  long err;
  X509 *peer;
  char reason[512];
  JSValue e;
  Env *env;
  TLS_SSL(t);
  env = t->env;
  peer = SSL_get1_peer_certificate(t->ssl);
  if (peer) {
    err = SSL_get_verify_result(t->ssl);
    X509_free(peer);
  } else {
    const SSL_CIPHER *c = SSL_get_current_cipher(t->ssl);
    SSL_SESSION *sess = SSL_get_session(t->ssl);
    if ((c && SSL_CIPHER_get_auth_nid(c) == NID_auth_psk) ||
        (sess && SSL_SESSION_get_protocol_version(sess) == TLS1_3_VERSION &&
         SSL_session_reused(t->ssl)))
      err = X509_V_OK;
    else
      err = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT;
  }
  if (err == X509_V_OK)
    return JS_NULL;
  snprintf(reason, sizeof(reason), "%s", X509_verify_cert_error_string(err));
  if (err == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE ||
      err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
      (err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT &&
       !(env->options && node_option_bool(env->options, "--use-system-ca"))))
    strncat(reason, "; if the root CA is installed locally, try running Node.js with --use-system-ca",
            sizeof(reason) - strlen(reason) - 1);
  e = JS_NewError(ctx);
  JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, reason),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, x509_error_code(err)));
  return e;
}

static JSValue tls_get_cipher(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  const SSL_CIPHER *c;
  JSValue o;
  TLS_SSL(t);
  c = SSL_get_current_cipher(t->ssl);
  if (!c)
    return JS_UNDEFINED;
  o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, SSL_CIPHER_get_name(c)));
  JS_SetPropertyStr(ctx, o, "standardName", JS_NewString(ctx, SSL_CIPHER_standard_name(c)));
  JS_SetPropertyStr(ctx, o, "version", JS_NewString(ctx, SSL_CIPHER_get_version(c)));
  return o;
}

static JSValue tls_load_session(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  size_t len;
  const uint8_t *d;
  if (!t)
    return JS_EXCEPTION;
  d = nb_buffer_data(ctx, ARG(0), &len);
  if (d && nb_is_array_buffer_view(ctx, ARG(0))) {
    if (t->next_sess)
      SSL_SESSION_free(t->next_sess);
    t->next_sess = d2i_SSL_SESSION(NULL, &d, (long)len);
  }
  return JS_UNDEFINED;
}

static JSValue tls_get_shared_sigalgs(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  JSValue arr;
  int n, i;
  TLS_SSL(t);
  n = SSL_get_shared_sigalgs(t->ssl, 0, NULL, NULL, NULL, NULL, NULL);
  arr = JS_NewArray(ctx);
  for (i = 0; i < n; i++) {
    int hash_nid = 0, sign_nid = 0;
    char buf[128];
    const char *sig, *h;
    SSL_get_shared_sigalgs(t->ssl, i, &sign_nid, &hash_nid, NULL, NULL, NULL);
    switch (sign_nid) {
    case EVP_PKEY_RSA: sig = "RSA"; break;
    case EVP_PKEY_RSA_PSS: sig = "RSA-PSS"; break;
    case EVP_PKEY_DSA: sig = "DSA"; break;
    case EVP_PKEY_EC: sig = "ECDSA"; break;
    case NID_ED25519: sig = "Ed25519"; break;
    case NID_ED448: sig = "Ed448"; break;
    default: sig = OBJ_nid2sn(sign_nid); if (!sig) sig = "UNDEF";
    }
    h = OBJ_nid2sn(hash_nid);
    snprintf(buf, sizeof(buf), "%s+%s", sig, h ? h : "UNDEF");
    JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, buf));
  }
  return arr;
}

static JSValue tls_export_keying_material(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  uint32_t olen;
  size_t llen, clen = 0;
  const char *label;
  uint8_t *out, *cd = NULL;
  bool use_ctx;
  JSValue r;
  TLS_SSL(t);
  olen = nb_uint32(ctx, ARG(0), 0);
  label = JS_ToCStringLen(ctx, &llen, ARG(1));
  use_ctx = !JS_IsUndefined(ARG(2));
  if (use_ctx)
    cd = nb_buffer_data(ctx, ARG(2), &clen);
  out = malloc(olen ? olen : 1);
  if (SSL_export_keying_material(t->ssl, out, olen, label, llen, cd, clen, use_ctx) != 1) {
    free(out);
    JS_FreeCString(ctx, label);
    return crypto_throw(ctx, ERR_get_error(), "SSL_export_keying_material");
  }
  JS_FreeCString(ctx, label);
  r = nb_new_buffer(ctx, out, olen);
  free(out);
  return r;
}

static JSValue tls_renegotiate(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  if (SSL_renegotiate(t->ssl) != 1)
    return crypto_throw(ctx, ERR_get_error(), NULL);
  return JS_UNDEFINED;
}

static JSValue tls_get_ticket(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  SSL_SESSION *sess;
  const unsigned char *ticket;
  size_t len;
  TLS_SSL(t);
  sess = SSL_get_session(t->ssl);
  if (!sess)
    return JS_UNDEFINED;
  SSL_SESSION_get0_ticket(sess, &ticket, &len);
  return ticket ? nb_new_buffer(ctx, ticket, len) : JS_UNDEFINED;
}

static JSValue tls_new_session_done(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  if (!t)
    return JS_EXCEPTION;
  t->awaiting_new_session = false;
  tls_cycle(t);
  return JS_UNDEFINED;
}

static JSValue tls_set_ocsp_response(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  if (!t)
    return JS_EXCEPTION;
  if (argc < 1)
    return node_throw_type_error(ctx, "ERR_MISSING_ARGS", "OCSP response argument is mandatory");
  JS_FreeValue(ctx, t->ocsp_response);
  t->ocsp_response = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue tls_request_ocsp(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  SSL_set_tlsext_status_type(t->ssl, TLSEXT_STATUSTYPE_ocsp);
  return JS_UNDEFINED;
}

static JSValue tls_get_ephemeral_key_info(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  EVP_PKEY *key = NULL;
  JSValue o;
  bool found = false;
  TLS_SSL(t);
  if (t->is_server)
    return JS_NULL;
  o = JS_NewObject(ctx);
  if (SSL_get_peer_tmp_key(t->ssl, &key) && key) {
    int kid = EVP_PKEY_get_base_id(key);
    if (kid == EVP_PKEY_DH) {
      JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "DH"));
      JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, EVP_PKEY_get_bits(key)));
      found = true;
    } else if (kid == EVP_PKEY_EC || kid == EVP_PKEY_X25519 || kid == EVP_PKEY_X448) {
      const char *name = NULL;
      if (kid == EVP_PKEY_EC) {
        const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(key);
        int nid = ec ? EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) : NID_undef;
        name = nid != NID_undef ? OBJ_nid2sn(nid) : NULL;
      } else {
        name = OBJ_nid2sn(kid);
      }
      if (name) {
        JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "ECDH"));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, EVP_PKEY_get_bits(key)));
        found = true;
      }
    }
    EVP_PKEY_free(key);
  }
  if (!found) {
    const char *g = SSL_get0_group_name(t->ssl);
    if (g) {
      JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "TLSGroup"));
      JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, g));
    }
  }
  ERR_clear_error();
  return o;
}

static JSValue tls_get_protocol(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  return JS_NewString(ctx, SSL_get_version(t->ssl));
}

static JSValue tls_get_alpn(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  const unsigned char *p;
  unsigned len;
  TLS_SSL(t);
  SSL_get0_alpn_selected(t->ssl, &p, &len);
  if (!len)
    return JS_FALSE;
  return JS_NewStringLen(ctx, (const char *)p, len);
}

static JSValue tls_set_max_send_fragment(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  TLSWrap *t = tls_of(ctx, this_val);
  TLS_SSL(t);
  return JS_NewInt32(ctx, (int)SSL_set_max_send_fragment(t->ssl, nb_int32(ctx, ARG(0), 16384)));
}

static JSValue tls_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TLSWrap *t = JS_GetOpaque(this_val, tls_wrap_class_id);
  return JS_NewFloat64(ctx, t ? t->s.hw.aw.async_id : -1);
}

static JSValue tls_get_provider_type(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return JS_NewInt32(ctx, PROVIDER_TLSWRAP);
}

static const JSCFunctionListEntry tls_proto[] = {
  JS_CFUNC_DEF("receive", 1, tls_receive),
  JS_CFUNC_DEF("start", 0, tls_start),
  JS_CFUNC_DEF("setVerifyMode", 2, tls_set_verify_mode),
  JS_CFUNC_DEF("enableSessionCallbacks", 0, tls_enable_session_callbacks),
  JS_CFUNC_DEF("enableKeylogCallback", 0, tls_enable_keylog),
  JS_CFUNC_DEF("enableTrace", 0, tls_noop),
  JS_CFUNC_DEF("destroySSL", 0, tls_destroy_ssl),
  JS_CFUNC_DEF("enableCertCb", 0, tls_enable_cert_cb),
  JS_CFUNC_DEF("certCbDone", 0, tls_cert_cb_done),
  JS_CFUNC_DEF("enableALPNCb", 0, tls_enable_alpn_cb),
  JS_CFUNC_DEF("endParser", 0, tls_noop),
  JS_CFUNC_DEF("getServername", 0, tls_get_servername),
  JS_CFUNC_DEF("setServername", 1, tls_set_servername),
  JS_CFUNC_DEF("setPskIdentityHint", 1, tls_set_psk_hint),
  JS_CFUNC_DEF("enablePskCallback", 0, tls_enable_psk),
  JS_CFUNC_DEF("setALPNProtocols", 1, tls_set_alpn_protocols),
  JS_CFUNC_DEF("setKeyCert", 1, tls_set_key_cert),
  JS_CFUNC_DEF("getPeerCertificate", 1, tls_get_peer_certificate),
  JS_CFUNC_DEF("getPeerX509Certificate", 0, tls_get_peer_x509),
  JS_CFUNC_DEF("getCertificate", 0, tls_get_certificate),
  JS_CFUNC_DEF("getX509Certificate", 0, tls_get_x509),
  JS_CFUNC_MAGIC_DEF("getFinished", 0, tls_finished, 0),
  JS_CFUNC_MAGIC_DEF("getPeerFinished", 0, tls_finished, 1),
  JS_CFUNC_DEF("getSession", 0, tls_get_session),
  JS_CFUNC_DEF("setSession", 1, tls_set_session),
  JS_CFUNC_DEF("isSessionReused", 0, tls_is_session_reused),
  JS_CFUNC_DEF("verifyError", 0, tls_verify_error),
  JS_CFUNC_DEF("getCipher", 0, tls_get_cipher),
  JS_CFUNC_DEF("loadSession", 1, tls_load_session),
  JS_CFUNC_DEF("getSharedSigalgs", 0, tls_get_shared_sigalgs),
  JS_CFUNC_DEF("exportKeyingMaterial", 3, tls_export_keying_material),
  JS_CFUNC_DEF("renegotiate", 0, tls_renegotiate),
  JS_CFUNC_DEF("getTLSTicket", 0, tls_get_ticket),
  JS_CFUNC_DEF("newSessionDone", 0, tls_new_session_done),
  JS_CFUNC_DEF("setOCSPResponse", 1, tls_set_ocsp_response),
  JS_CFUNC_DEF("requestOCSP", 0, tls_request_ocsp),
  JS_CFUNC_DEF("getEphemeralKeyInfo", 0, tls_get_ephemeral_key_info),
  JS_CFUNC_DEF("getProtocol", 0, tls_get_protocol),
  JS_CFUNC_DEF("getALPNNegotiatedProtocol", 0, tls_get_alpn),
  JS_CFUNC_DEF("setMaxSendFragment", 1, tls_set_max_send_fragment),
  JS_CFUNC_DEF("writesIssuedByPrevListenerDone", 0, tls_noop),
  JS_CFUNC_DEF("getAsyncId", 0, tls_get_async_id),
  JS_CFUNC_DEF("getProviderType", 0, tls_get_provider_type),
  JS_CGETSET_DEF("writeQueueSize", tls_write_queue_size, NULL),
  JS_CGETSET_DEF("fd", tls_fd, NULL),
};

void crypto_init_tls(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef def = { .name = "SecureContext", .class_id = &secure_context_class_id,
                       .ctor = sc_ctor, .finalizer = sc_finalizer,
                       .proto_funcs = sc_proto_funcs, .proto_funcs_count = countof(sc_proto_funcs),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, target, &def));
}

JSValue binding_init_tls_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ctor, proto;
  NodeClassDef def = { .name = "TLSWrap", .class_id = &tls_wrap_class_id, .ctor = tls_ctor,
                       .finalizer = tls_finalizer, .gc_mark = tls_gc_mark,
                       .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &def);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_stream_base_funcs, node_stream_base_funcs_count);
  JS_SetPropertyFunctionList(ctx, proto, tls_proto, countof(tls_proto));
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  node_register_stream_class(tls_wrap_class_id);
  nb_set_method(ctx, t, "wrap", tls_wrap_fn, 4);
  nb_set_int(ctx, t, "HAVE_SSL_TRACE", 0);
  return t;
}
