/* Keys for internalBinding('crypto') (crypto_keys.cc, the JWK parts of
 * crypto_rsa.cc / crypto_ec.cc): KeyObjectHandle, NativeKeyObject,
 * NativeCryptoKey, parsing PEM/DER/JWK/raw input and encoding output. */
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

JSClassID crypto_key_handle_class_id;
static JSClassID native_key_object_class_id, native_crypto_key_class_id;

/* ---------------------------------------------------------------------- */
/* KeyData */

KeyData *key_data_new_secret(const uint8_t *data, size_t len) {
  KeyData *k = calloc(1, sizeof(*k));
  k->refs = 1;
  k->type = kKeyTypeSecret;
  k->secret = malloc(len ? len : 1);
  if (len)
    memcpy(k->secret, data, len);
  k->secret_len = len;
  return k;
}

KeyData *key_data_new_pkey(int type, EVP_PKEY *pkey) {
  KeyData *k = calloc(1, sizeof(*k));
  k->refs = 1;
  k->type = type;
  k->pkey = pkey;
  return k;
}

KeyData *key_data_ref(KeyData *k) {
  if (k)
    __atomic_add_fetch(&k->refs, 1, __ATOMIC_SEQ_CST);
  return k;
}

void key_data_unref(KeyData *k) {
  if (!k || __atomic_sub_fetch(&k->refs, 1, __ATOMIC_SEQ_CST) > 0)
    return;
  EVP_PKEY_free(k->pkey);
  if (k->secret) {
    OPENSSL_cleanse(k->secret, k->secret_len);
    free(k->secret);
  }
  free(k);
}

KeyData *key_data_with_type(KeyData *k, int type) {
  if (k->type == type)
    return key_data_ref(k);
  EVP_PKEY_up_ref(k->pkey);
  return key_data_new_pkey(type, k->pkey);
}

/* ---------------------------------------------------------------------- */
/* small helpers */

static JSValue throw_code(JSContext *ctx, const char *code, const char *msg) {
  return crypto_throw_code(ctx, code, msg ? "%s" : NULL, msg);
}

static char *b64url(const uint8_t *d, size_t len) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char *out = malloc(len * 4 / 3 + 4), *o = out;
  size_t i;
  for (i = 0; i + 2 < len; i += 3) {
    uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
    *o++ = tab[v >> 18];
    *o++ = tab[(v >> 12) & 63];
    *o++ = tab[(v >> 6) & 63];
    *o++ = tab[v & 63];
  }
  if (len - i == 1) {
    uint32_t v = d[i] << 16;
    *o++ = tab[v >> 18];
    *o++ = tab[(v >> 12) & 63];
  } else if (len - i == 2) {
    uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
    *o++ = tab[v >> 18];
    *o++ = tab[(v >> 12) & 63];
    *o++ = tab[(v >> 6) & 63];
  }
  *o = 0;
  return out;
}

/* base64 or base64url, padding optional */
static uint8_t *b64decode(const char *s, size_t *len) {
  size_t n = strlen(s), i;
  uint8_t *out = malloc(n * 3 / 4 + 3);
  uint32_t acc = 0;
  int bits = 0;
  *len = 0;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)s[i], v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[(*len)++] = (uint8_t)(acc >> bits);
    }
  }
  return out;
}

static void set_b64url(JSContext *ctx, JSValueConst obj, const char *name, const uint8_t *d,
                       size_t len) {
  char *s = b64url(d, len);
  JS_SetPropertyStr(ctx, obj, name, JS_NewString(ctx, s));
  free(s);
}

static void set_bn(JSContext *ctx, JSValueConst obj, const char *name, const BIGNUM *bn,
                   int pad) {
  int n = BN_num_bytes(bn);
  uint8_t *buf;
  if (pad < n)
    pad = n;
  buf = malloc(pad ? pad : 1);
  BN_bn2binpad(bn, buf, pad);
  set_b64url(ctx, obj, name, buf, pad);
  free(buf);
}

/* a string property decoded from base64url into a BIGNUM */
static BIGNUM *get_bn(JSContext *ctx, JSValueConst obj, const char *name, bool *present) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  const char *s;
  uint8_t *d;
  size_t len;
  BIGNUM *bn;
  *present = JS_IsString(v);
  if (!*present) {
    JS_FreeValue(ctx, v);
    return NULL;
  }
  s = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  d = b64decode(s, &len);
  JS_FreeCString(ctx, s);
  bn = BN_bin2bn(d, (int)len, NULL);
  free(d);
  return bn;
}

static uint8_t *get_b64(JSContext *ctx, JSValueConst obj, const char *name, size_t *len) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  const char *s;
  uint8_t *d;
  if (!JS_IsString(v)) {
    JS_FreeValue(ctx, v);
    return NULL;
  }
  s = JS_ToCString(ctx, v);
  JS_FreeValue(ctx, v);
  d = b64decode(s, len);
  JS_FreeCString(ctx, s);
  return d;
}

static char *get_str(JSContext *ctx, JSValueConst obj, const char *name) {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  char *r = NULL;
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    r = strdup(s);
    JS_FreeCString(ctx, s);
  }
  JS_FreeValue(ctx, v);
  return r;
}

static int curve_field_bytes(const EC_GROUP *g) {
  return (EC_GROUP_get_degree(g) + 7) / 8;
}

/* JWK curve names <-> NIDs */
static int jwk_curve_nid(const char *crv) {
  if (!strcmp(crv, "P-256")) return NID_X9_62_prime256v1;
  if (!strcmp(crv, "P-384")) return NID_secp384r1;
  if (!strcmp(crv, "P-521")) return NID_secp521r1;
  if (!strcmp(crv, "secp256k1")) return NID_secp256k1;
  return NID_undef;
}

static const char *nid_jwk_curve(int nid) {
  switch (nid) {
  case NID_X9_62_prime256v1: return "P-256";
  case NID_secp384r1: return "P-384";
  case NID_secp521r1: return "P-521";
  case NID_secp256k1: return "secp256k1";
  }
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* building EVP_PKEYs from components (legacy key structs keep this short) */

static EVP_PKEY *pkey_from_rsa(RSA *rsa) {
  EVP_PKEY *p = EVP_PKEY_new();
  if (!p || EVP_PKEY_assign_RSA(p, rsa) != 1) {
    EVP_PKEY_free(p);
    RSA_free(rsa);
    return NULL;
  }
  return p;
}

static EVP_PKEY *pkey_from_ec(EC_KEY *ec) {
  EVP_PKEY *p = EVP_PKEY_new();
  if (!p || EVP_PKEY_assign_EC_KEY(p, ec) != 1) {
    EVP_PKEY_free(p);
    EC_KEY_free(ec);
    return NULL;
  }
  return p;
}

static KeyData *import_jwk_rsa(JSContext *ctx, JSValueConst jwk) {
  bool has_n, has_e, has_d;
  BIGNUM *n = get_bn(ctx, jwk, "n", &has_n), *e = get_bn(ctx, jwk, "e", &has_e);
  BIGNUM *d = get_bn(ctx, jwk, "d", &has_d);
  RSA *rsa;
  EVP_PKEY *pkey;
  if (!has_n || !has_e) {
    BN_free(n);
    BN_free(e);
    BN_free(d);
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK RSA key");
    return NULL;
  }
  rsa = RSA_new();
  RSA_set0_key(rsa, n, e, d);
  if (has_d) {
    bool hp, hq, hdp, hdq, hqi;
    BIGNUM *p = get_bn(ctx, jwk, "p", &hp), *q = get_bn(ctx, jwk, "q", &hq);
    BIGNUM *dp = get_bn(ctx, jwk, "dp", &hdp), *dq = get_bn(ctx, jwk, "dq", &hdq);
    BIGNUM *qi = get_bn(ctx, jwk, "qi", &hqi);
    if (!hp || !hq || !hdp || !hdq || !hqi) {
      BN_free(p); BN_free(q); BN_free(dp); BN_free(dq); BN_free(qi);
      RSA_free(rsa);
      throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK RSA key");
      return NULL;
    }
    RSA_set0_factors(rsa, p, q);
    RSA_set0_crt_params(rsa, dp, dq, qi);
  }
  pkey = pkey_from_rsa(rsa);
  if (!pkey) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK RSA key");
    return NULL;
  }
  return key_data_new_pkey(has_d ? kKeyTypePrivate : kKeyTypePublic, pkey);
}

static KeyData *import_jwk_ec(JSContext *ctx, JSValueConst jwk) {
  char *crv = get_str(ctx, jwk, "crv");
  bool hx, hy, hd;
  BIGNUM *x, *y, *d;
  EC_KEY *ec;
  int nid;
  EVP_PKEY *pkey;
  if (!crv) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK EC key");
    return NULL;
  }
  nid = jwk_curve_nid(crv);
  free(crv);
  if (nid == NID_undef) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK EC key");
    return NULL;
  }
  x = get_bn(ctx, jwk, "x", &hx);
  y = get_bn(ctx, jwk, "y", &hy);
  d = get_bn(ctx, jwk, "d", &hd);
  if (!hx || !hy) {
    BN_free(x); BN_free(y); BN_free(d);
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK EC key");
    return NULL;
  }
  ec = EC_KEY_new_by_curve_name(nid);
  if (!ec || EC_KEY_set_public_key_affine_coordinates(ec, x, y) != 1 ||
      (hd && EC_KEY_set_private_key(ec, d) != 1)) {
    EC_KEY_free(ec);
    BN_free(x); BN_free(y); BN_clear_free(d);
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK EC key");
    return NULL;
  }
  BN_free(x); BN_free(y); BN_clear_free(d);
  pkey = pkey_from_ec(ec);
  if (!pkey) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK EC key");
    return NULL;
  }
  return key_data_new_pkey(hd ? kKeyTypePrivate : kKeyTypePublic, pkey);
}

static int okp_nid(const char *crv) {
  if (!strcmp(crv, "Ed25519")) return EVP_PKEY_ED25519;
  if (!strcmp(crv, "Ed448")) return EVP_PKEY_ED448;
  if (!strcmp(crv, "X25519")) return EVP_PKEY_X25519;
  if (!strcmp(crv, "X448")) return EVP_PKEY_X448;
  return NID_undef;
}

static KeyData *import_jwk_okp(JSContext *ctx, JSValueConst jwk) {
  char *crv = get_str(ctx, jwk, "crv");
  size_t xl, dl;
  uint8_t *x, *d;
  int nid;
  EVP_PKEY *pkey;
  if (!crv || (nid = okp_nid(crv)) == NID_undef) {
    free(crv);
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK OKP key");
    return NULL;
  }
  free(crv);
  x = get_b64(ctx, jwk, "x", &xl);
  d = get_b64(ctx, jwk, "d", &dl);
  if (!x) {
    free(d);
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK OKP key");
    return NULL;
  }
  if (d)
    pkey = EVP_PKEY_new_raw_private_key(nid, NULL, d, dl);
  else
    pkey = EVP_PKEY_new_raw_public_key(nid, NULL, x, xl);
  free(x);
  if (d)
    OPENSSL_cleanse(d, dl);
  free(d);
  if (!pkey) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK OKP key");
    return NULL;
  }
  return key_data_new_pkey(d ? kKeyTypePrivate : kKeyTypePublic, pkey);
}

static KeyData *import_jwk(JSContext *ctx, JSValueConst jwk) {
  char *kty = get_str(ctx, jwk, "kty");
  KeyData *k = NULL;
  if (!kty) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", NULL);
    return NULL;
  }
  if (!strcmp(kty, "RSA"))
    k = import_jwk_rsa(ctx, jwk);
  else if (!strcmp(kty, "EC"))
    k = import_jwk_ec(ctx, jwk);
  else if (!strcmp(kty, "OKP"))
    k = import_jwk_okp(ctx, jwk);
  else if (!strcmp(kty, "AKP"))
    node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE", "Unsupported key type");
  else
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "%s is not a supported JWK key type", kty);
  free(kty);
  return k;
}

static KeyData *import_jwk_secret(JSContext *ctx, JSValueConst jwk) {
  size_t len;
  uint8_t *k = get_b64(ctx, jwk, "k", &len);
  KeyData *r;
  if (!k) {
    throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "Invalid JWK secret key format");
    return NULL;
  }
  r = key_data_new_secret(k, len);
  OPENSSL_cleanse(k, len);
  free(k);
  return r;
}

/* raw public / private / seed key bytes (ImportRawKey) */
static KeyData *import_raw(JSContext *ctx, const uint8_t *d, size_t len, int format,
                           const char *type_name, const char *curve, int target_type) {
  int id = NID_undef;
  EVP_PKEY *pkey = NULL;
  if (!strcmp(type_name, "ec")) {
    int nid = crypto_nid_from_curve(curve ? curve : "");
    EC_KEY *ec;
    if (nid == NID_undef) {
      throw_code(ctx, "ERR_CRYPTO_INVALID_CURVE", NULL);
      return NULL;
    }
    ec = EC_KEY_new_by_curve_name(nid);
    if (!ec)
      goto invalid;
    if (format == kKeyFormatRawPublic) {
      const EC_GROUP *g = EC_KEY_get0_group(ec);
      EC_POINT *pt = EC_POINT_new(g);
      if (!pt || EC_POINT_oct2point(g, pt, d, len, NULL) != 1 ||
          EC_KEY_set_public_key(ec, pt) != 1) {
        EC_POINT_free(pt);
        EC_KEY_free(ec);
        goto invalid;
      }
      EC_POINT_free(pt);
    } else {
      const EC_GROUP *g = EC_KEY_get0_group(ec);
      BIGNUM *priv = BN_bin2bn(d, (int)len, NULL);
      EC_POINT *pub = EC_POINT_new(g);
      if (!priv || EC_KEY_set_private_key(ec, priv) != 1 || !pub ||
          EC_POINT_mul(g, pub, priv, NULL, NULL, NULL) != 1 ||
          EC_KEY_set_public_key(ec, pub) != 1) {
        BN_clear_free(priv);
        EC_POINT_free(pub);
        EC_KEY_free(ec);
        goto invalid;
      }
      BN_clear_free(priv);
      EC_POINT_free(pub);
    }
    pkey = pkey_from_ec(ec);
    if (!pkey)
      goto invalid;
    return key_data_new_pkey(target_type, pkey);
  }
  if (!strcmp(type_name, "ed25519")) id = EVP_PKEY_ED25519;
  else if (!strcmp(type_name, "ed448")) id = EVP_PKEY_ED448;
  else if (!strcmp(type_name, "x25519")) id = EVP_PKEY_X25519;
  else if (!strcmp(type_name, "x448")) id = EVP_PKEY_X448;
  if (id == NID_undef || format == kKeyFormatRawSeed) {
    throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
    return NULL;
  }
  pkey = target_type == kKeyTypePrivate ? EVP_PKEY_new_raw_private_key(id, NULL, d, len)
                                        : EVP_PKEY_new_raw_public_key(id, NULL, d, len);
  if (!pkey)
    goto invalid;
  return key_data_new_pkey(target_type, pkey);
invalid:
  ERR_clear_error();
  node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE", "Invalid key data");
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* parsing PEM / DER */

typedef struct {
  const uint8_t *pass;
  size_t len;
  bool has;
  bool needed;
} PassCb;

static int pass_cb(char *buf, int size, int rwflag, void *u) {
  PassCb *p = u;
  if (!p || !p->has) {
    if (p)
      p->needed = true;
    return -1;
  }
  if ((int)p->len > size)
    return -1;
  memcpy(buf, p->pass, p->len);
  return (int)p->len;
}

enum { PARSE_OK, PARSE_NOT_RECOGNIZED, PARSE_NEED_PASSPHRASE, PARSE_FAILED };

static int parse_public_pem(const uint8_t *d, size_t len, EVP_PKEY **out) {
  BIO *bio;
  X509 *x;
  RSA *rsa;
  ERR_set_mark();
  bio = BIO_new_mem_buf(d, (int)len);
  *out = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (*out)
    goto ok;
  bio = BIO_new_mem_buf(d, (int)len);
  rsa = PEM_read_bio_RSAPublicKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (rsa) {
    *out = pkey_from_rsa(rsa);
    goto ok;
  }
  bio = BIO_new_mem_buf(d, (int)len);
  x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (x) {
    *out = X509_get_pubkey(x);
    X509_free(x);
    if (*out)
      goto ok;
  }
  ERR_pop_to_mark();
  return PARSE_NOT_RECOGNIZED;
ok:
  ERR_pop_to_mark();
  return PARSE_OK;
}

static int parse_public_der(const uint8_t *d, size_t len, int type, EVP_PKEY **out) {
  const uint8_t *p = d;
  if (type == kKeyEncodingPKCS1) {
    RSA *rsa = d2i_RSAPublicKey(NULL, &p, (long)len);
    if (!rsa)
      return PARSE_FAILED;
    *out = pkey_from_rsa(rsa);
  } else {
    *out = d2i_PUBKEY(NULL, &p, (long)len);
  }
  return *out ? PARSE_OK : PARSE_FAILED;
}

static bool is_rsa_private_der(const uint8_t *d, size_t len) {
  const uint8_t *p = d;
  RSA *rsa;
  ERR_set_mark();
  rsa = d2i_RSAPrivateKey(NULL, &p, (long)len);
  ERR_pop_to_mark();
  RSA_free(rsa);
  return rsa != NULL;
}

static int parse_private(const uint8_t *d, size_t len, int format, int type, PassCb *pc,
                         EVP_PKEY **out) {
  *out = NULL;
  if (format == kKeyFormatPEM) {
    BIO *bio = BIO_new_mem_buf(d, (int)len);
    *out = PEM_read_bio_PrivateKey(bio, NULL, pass_cb, pc);
    BIO_free(bio);
  } else {
    const uint8_t *p = d;
    if (type == kKeyEncodingPKCS1) {
      RSA *rsa = d2i_RSAPrivateKey(NULL, &p, (long)len);
      if (rsa)
        *out = pkey_from_rsa(rsa);
    } else if (type == kKeyEncodingPKCS8) {
      BIO *bio = BIO_new_mem_buf(d, (int)len);
      /* encrypted PKCS#8 first, then plain */
      ERR_set_mark();
      *out = d2i_PKCS8PrivateKey_bio(bio, NULL, pass_cb, pc);
      BIO_free(bio);
      if (*out || pc->needed) {
        ERR_pop_to_mark();
      } else {
        PKCS8_PRIV_KEY_INFO *p8;
        ERR_pop_to_mark();
        p = d;
        p8 = d2i_PKCS8_PRIV_KEY_INFO(NULL, &p, (long)len);
        if (p8) {
          *out = EVP_PKCS82PKEY(p8);
          PKCS8_PRIV_KEY_INFO_free(p8);
        }
      }
    } else if (type == kKeyEncodingSEC1) {
      EC_KEY *ec = d2i_ECPrivateKey(NULL, &p, (long)len);
      if (ec)
        *out = pkey_from_ec(ec);
    }
  }
  if (*out)
    return PARSE_OK;
  /* ncrypto's TryParsePrivateKey: only PEM's "bad password read" without a
     passphrase is ERR_MISSING_PASSPHRASE; OpenSSL 3's decoders report a
     refused callback as "interrupted or cancelled", which goes as it is */
  if (ERR_GET_LIB(ERR_peek_error()) == ERR_LIB_PEM &&
      ERR_GET_REASON(ERR_peek_error()) == PEM_R_BAD_PASSWORD_READ && !pc->has)
    return PARSE_NEED_PASSPHRASE;
  return PARSE_FAILED;
}

static KeyData *private_result(JSContext *ctx, int r, EVP_PKEY *pkey) {
  if (r == PARSE_OK)
    return key_data_new_pkey(kKeyTypePrivate, pkey);
  if (r == PARSE_NEED_PASSPHRASE) {
    ERR_clear_error();
    node_throw_type_error(ctx, "ERR_MISSING_PASSPHRASE", "Passphrase required for encrypted key");
    return NULL;
  }
  /* keyOrError: the oldest error is the one reported */
  crypto_throw(ctx, ERR_peek_error(), "Failed to read private key");
  return NULL;
}

/* one input key group: argv[o]=data, o+1 format, o+2 type, o+3 passphrase,
   o+4 namedCurve */
static KeyData *key_from_group(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                               bool private_only, bool allow_key_object) {
  int o = *offset;
  JSValueConst data = o < argc ? argv[o] : JS_UNDEFINED;
  JSValueConst fmtv = o + 1 < argc ? argv[o + 1] : JS_UNDEFINED;
  JSValueConst typev = o + 2 < argc ? argv[o + 2] : JS_UNDEFINED;
  JSValueConst passv = o + 3 < argc ? argv[o + 3] : JS_UNDEFINED;
  JSValueConst curvev = o + 4 < argc ? argv[o + 4] : JS_UNDEFINED;
  int format = JS_IsNumber(fmtv) ? nb_int32(ctx, fmtv, kKeyFormatPEM) : kKeyFormatPEM;
  KeyData *k;
  *offset = o + 5;

  if (JS_IsObject(data) && !crypto_is_buffer_source(ctx, data)) {
    KeyData *h = crypto_key_handle_data(data);
    if (h) {
      if (private_only && h->type != kKeyTypePrivate) {
        node_throw_type_error(ctx, "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
                              "Invalid key object type %s, expected private.",
                              h->type == kKeyTypePublic ? "public" : "secret");
        return NULL;
      }
      return key_data_ref(h);
    }
    if (format == kKeyFormatJWK)
      return import_jwk(ctx, data);
    if (format == kKeyFormatStore) {
      throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED",
                 "No private key found through the OpenSSL STORE loader");
      return NULL;
    }
  }
  if (format == kKeyFormatRawPublic || format == kKeyFormatRawPrivate ||
      format == kKeyFormatRawSeed) {
    size_t len;
    uint8_t *d = crypto_buffer_source(ctx, data, &len);
    const char *tn = JS_ToCString(ctx, typev);
    const char *cv = JS_IsString(curvev) ? JS_ToCString(ctx, curvev) : NULL;
    k = import_raw(ctx, d ? d : (uint8_t *)"", len, format, tn ? tn : "", cv,
                   format == kKeyFormatRawPublic ? kKeyTypePublic : kKeyTypePrivate);
    JS_FreeCString(ctx, tn);
    if (cv)
      JS_FreeCString(ctx, cv);
    return k;
  }
  {
    size_t len;
    uint8_t *d = crypto_bytes_copy(ctx, data, &len);
    int type = JS_IsNumber(typev) ? nb_int32(ctx, typev, kKeyEncodingPKCS1) : kKeyEncodingPKCS1;
    PassCb pc = { 0 };
    EVP_PKEY *pkey = NULL;
    int r;
    uint8_t *pass = NULL;
    if (!d)
      return NULL;
    if (crypto_is_buffer_source(ctx, passv)) {
      size_t pl;
      uint8_t *pd = crypto_buffer_source(ctx, passv, &pl);
      pass = malloc(pl ? pl : 1);
      memcpy(pass, pd, pl);
      pc.pass = pass;
      pc.len = pl;
      pc.has = true;
    }
    k = NULL;
    if (!private_only) {
      if (format == kKeyFormatPEM) {
        r = parse_public_pem(d, len, &pkey);
        if (r == PARSE_OK) {
          k = key_data_new_pkey(kKeyTypePublic, pkey);
          goto done;
        }
      } else {
        bool is_public = type == kKeyEncodingSPKI ||
                         (type == kKeyEncodingPKCS1 && !is_rsa_private_der(d, len));
        if (is_public) {
          r = parse_public_der(d, len, type, &pkey);
          if (r == PARSE_OK)
            k = key_data_new_pkey(kKeyTypePublic, pkey);
          else
            crypto_throw(ctx, ERR_peek_last_error(), "Failed to read asymmetric key");
          goto done;
        }
      }
    }
    r = parse_private(d, len, format, type, &pc, &pkey);
    k = private_result(ctx, r, pkey);
  done:
    OPENSSL_cleanse(d, len);
    free(d);
    if (pass) {
      OPENSSL_cleanse(pass, pc.len);
      free(pass);
    }
    return k;
  }
}

KeyData *crypto_get_public_or_private_key(JSContext *ctx, JSValueConst *argv, int argc,
                                          int *offset) {
  return key_from_group(ctx, argv, argc, offset, false, true);
}

KeyData *crypto_get_private_key(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                bool allow_key_object) {
  return key_from_group(ctx, argv, argc, offset, true, allow_key_object);
}

/* ---------------------------------------------------------------------- */
/* output encodings (GetKeyFormatAndTypeFromJs and friends) */

static int parse_format_type(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                             KeyEncoding *enc, bool generate) {
  int o = *offset;
  JSValueConst f = o < argc ? argv[o] : JS_UNDEFINED, t = o + 1 < argc ? argv[o + 1] : JS_UNDEFINED;
  memset(enc, 0, sizeof(*enc));
  enc->ec_point_form = -1;
  if (JS_IsUndefined(f)) {
    enc->output_key_object = true;
  } else {
    enc->format = nb_int32(ctx, f, kKeyFormatPEM);
    if (enc->format == kKeyFormatRawPublic || enc->format == kKeyFormatRawPrivate ||
        enc->format == kKeyFormatRawSeed) {
      if (JS_IsNumber(t))
        enc->ec_point_form = nb_int32(ctx, t, -1);
    } else if (JS_IsNumber(t)) {
      enc->type = nb_int32(ctx, t, kKeyEncodingPKCS1);
    } else {
      enc->type = kKeyEncodingPKCS1;
    }
  }
  *offset = o + 2;
  return 0;
}

int crypto_parse_public_encoding(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                 KeyEncoding *enc, bool generate) {
  return parse_format_type(ctx, argv, argc, offset, enc, generate);
}

int crypto_parse_private_encoding(JSContext *ctx, JSValueConst *argv, int argc, int *offset,
                                  KeyEncoding *enc, bool generate) {
  JSValueConst v;
  parse_format_type(ctx, argv, argc, offset, enc, generate);
  if (enc->output_key_object) {
    (*offset)++;
    return 0;
  }
  if (enc->format == kKeyFormatRawPrivate || enc->format == kKeyFormatRawSeed) {
    (*offset) += 2;
    return 0;
  }
  v = *offset < argc ? argv[*offset] : JS_UNDEFINED;
  if (JS_IsString(v)) {
    const char *name = JS_ToCString(ctx, v);
    enc->cipher = EVP_get_cipherbyname(name);
    JS_FreeCString(ctx, name);
    if (!enc->cipher) {
      throw_code(ctx, "ERR_CRYPTO_UNKNOWN_CIPHER", NULL);
      return -1;
    }
  }
  (*offset)++;
  v = *offset < argc ? argv[*offset] : JS_UNDEFINED;
  if (crypto_is_buffer_source(ctx, v)) {
    size_t len;
    uint8_t *d = crypto_buffer_source(ctx, v, &len);
    enc->passphrase = malloc(len ? len : 1);
    memcpy(enc->passphrase, d, len);
    enc->passphrase_len = len;
    enc->has_passphrase = true;
  }
  (*offset)++;
  return 0;
}

void crypto_key_encoding_free(KeyEncoding *enc) {
  if (enc->passphrase) {
    OPENSSL_cleanse(enc->passphrase, enc->passphrase_len);
    free(enc->passphrase);
  }
  enc->passphrase = NULL;
}

static JSValue bio_result(JSContext *ctx, BIO *bio, bool pem) {
  BUF_MEM *mem;
  JSValue r;
  BIO_get_mem_ptr(bio, &mem);
  if (pem)
    r = JS_NewStringLen(ctx, mem->data, mem->length);
  else
    r = nb_new_buffer(ctx, mem->data, mem->length);
  BIO_free(bio);
  return r;
}

static JSValue export_jwk_obj(JSContext *ctx, KeyData *k, JSValueConst target, bool rsa_pss);

JSValue crypto_export_public(JSContext *ctx, KeyData *k, const KeyEncoding *enc) {
  BIO *bio;
  int ok;
  bool pem = enc->format == kKeyFormatPEM;
  if (enc->format == kKeyFormatJWK) {
    JSValue o = JS_NewObject(ctx), r = export_jwk_obj(ctx, k, o, false);
    if (JS_IsException(r)) {
      JS_FreeValue(ctx, o);
      return r;
    }
    return o;
  }
  bio = BIO_new(BIO_s_mem());
  if (enc->type == kKeyEncodingPKCS1) {
    RSA *rsa = EVP_PKEY_get1_RSA(k->pkey);
    if (!rsa) {
      BIO_free(bio);
      return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
    }
    ok = pem ? PEM_write_bio_RSAPublicKey(bio, rsa) : i2d_RSAPublicKey_bio(bio, rsa);
    RSA_free(rsa);
  } else {
    ok = pem ? PEM_write_bio_PUBKEY(bio, k->pkey) : i2d_PUBKEY_bio(bio, k->pkey);
  }
  if (ok != 1) {
    BIO_free(bio);
    return crypto_throw(ctx, ERR_get_error(), "Failed to encode public key");
  }
  return bio_result(ctx, bio, pem);
}

JSValue crypto_export_private(JSContext *ctx, KeyData *k, const KeyEncoding *enc) {
  BIO *bio;
  int ok = 0;
  bool pem = enc->format == kKeyFormatPEM;
  const uint8_t *pass = enc->passphrase;
  int passlen = (int)enc->passphrase_len;
  if (enc->format == kKeyFormatJWK) {
    JSValue o = JS_NewObject(ctx), r = export_jwk_obj(ctx, k, o, false);
    if (JS_IsException(r)) {
      JS_FreeValue(ctx, o);
      return r;
    }
    return o;
  }
  bio = BIO_new(BIO_s_mem());
  if (enc->type == kKeyEncodingPKCS1) {
    RSA *rsa = EVP_PKEY_get1_RSA(k->pkey);
    if (!rsa) {
      BIO_free(bio);
      return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
    }
    if (pem)
      ok = PEM_write_bio_RSAPrivateKey(bio, rsa, enc->cipher, (unsigned char *)pass, passlen,
                                       NULL, NULL);
    else
      ok = i2d_RSAPrivateKey_bio(bio, rsa);
    RSA_free(rsa);
  } else if (enc->type == kKeyEncodingPKCS8) {
    if (pem)
      ok = PEM_write_bio_PKCS8PrivateKey(bio, k->pkey, enc->cipher, (const char *)pass, passlen,
                                         NULL, NULL);
    else
      ok = i2d_PKCS8PrivateKey_bio(bio, k->pkey, enc->cipher, (const char *)pass, passlen, NULL,
                                   NULL);
  } else if (enc->type == kKeyEncodingSEC1) {
    EC_KEY *ec = EVP_PKEY_get1_EC_KEY(k->pkey);
    if (!ec) {
      BIO_free(bio);
      return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
    }
    if (pem)
      ok = PEM_write_bio_ECPrivateKey(bio, ec, enc->cipher, (unsigned char *)pass, passlen, NULL,
                                      NULL);
    else
      ok = i2d_ECPrivateKey_bio(bio, ec);
    EC_KEY_free(ec);
  }
  if (ok != 1) {
    BIO_free(bio);
    return crypto_throw(ctx, ERR_get_error(), "Failed to encode private key");
  }
  return bio_result(ctx, bio, pem);
}

/* ---------------------------------------------------------------------- */
/* JWK export */

static JSValue export_jwk_obj(JSContext *ctx, KeyData *k, JSValueConst target, bool rsa_pss) {
  if (k->type == kKeyTypeSecret) {
    JS_SetPropertyStr(ctx, target, "kty", JS_NewString(ctx, "oct"));
    set_b64url(ctx, target, "k", k->secret, k->secret_len);
    return JS_UNDEFINED;
  }
  switch (EVP_PKEY_get_base_id(k->pkey)) {
  case EVP_PKEY_RSA_PSS:
    if (!rsa_pss)
      break;
    /* fallthrough */
  case EVP_PKEY_RSA: {
    const RSA *rsa = EVP_PKEY_get0_RSA(k->pkey);
    const BIGNUM *n, *e, *d, *p, *q, *dp, *dq, *qi;
    RSA_get0_key(rsa, &n, &e, &d);
    JS_SetPropertyStr(ctx, target, "kty", JS_NewString(ctx, "RSA"));
    set_bn(ctx, target, "n", n, 0);
    set_bn(ctx, target, "e", e, 0);
    if (k->type == kKeyTypePrivate) {
      RSA_get0_factors(rsa, &p, &q);
      RSA_get0_crt_params(rsa, &dp, &dq, &qi);
      set_bn(ctx, target, "d", d, 0);
      set_bn(ctx, target, "p", p, 0);
      set_bn(ctx, target, "q", q, 0);
      set_bn(ctx, target, "dp", dp, 0);
      set_bn(ctx, target, "dq", dq, 0);
      set_bn(ctx, target, "qi", qi, 0);
    }
    return JS_UNDEFINED;
  }
  case EVP_PKEY_EC: {
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(k->pkey);
    const EC_GROUP *g = EC_KEY_get0_group(ec);
    const char *crv = nid_jwk_curve(EC_GROUP_get_curve_name(g));
    BIGNUM *x, *y;
    int deg;
    if (!crv)
      return crypto_throw_code(ctx, "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE",
                               "Unsupported JWK EC curve: %s.",
                               OBJ_nid2sn(EC_GROUP_get_curve_name(g)));
    x = BN_new();
    y = BN_new();
    EC_POINT_get_affine_coordinates(g, EC_KEY_get0_public_key(ec), x, y, NULL);
    deg = curve_field_bytes(g);
    JS_SetPropertyStr(ctx, target, "kty", JS_NewString(ctx, "EC"));
    JS_SetPropertyStr(ctx, target, "crv", JS_NewString(ctx, crv));
    set_bn(ctx, target, "x", x, deg);
    set_bn(ctx, target, "y", y, deg);
    if (k->type == kKeyTypePrivate)
      set_bn(ctx, target, "d", EC_KEY_get0_private_key(ec), deg);
    BN_free(x);
    BN_free(y);
    return JS_UNDEFINED;
  }
  case EVP_PKEY_ED25519:
  case EVP_PKEY_ED448:
  case EVP_PKEY_X25519:
  case EVP_PKEY_X448: {
    int id = EVP_PKEY_get_base_id(k->pkey);
    const char *crv = id == EVP_PKEY_ED25519 ? "Ed25519" : id == EVP_PKEY_ED448 ? "Ed448"
                      : id == EVP_PKEY_X25519 ? "X25519" : "X448";
    uint8_t buf[128];
    size_t len = sizeof(buf);
    JS_SetPropertyStr(ctx, target, "crv", JS_NewString(ctx, crv));
    if (k->type == kKeyTypePrivate && EVP_PKEY_get_raw_private_key(k->pkey, buf, &len) == 1) {
      set_b64url(ctx, target, "d", buf, len);
      OPENSSL_cleanse(buf, len);
    }
    len = sizeof(buf);
    if (EVP_PKEY_get_raw_public_key(k->pkey, buf, &len) == 1)
      set_b64url(ctx, target, "x", buf, len);
    JS_SetPropertyStr(ctx, target, "kty", JS_NewString(ctx, "OKP"));
    return JS_UNDEFINED;
  }
  }
  return throw_code(ctx, "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE", NULL);
}

/* ---------------------------------------------------------------------- */
/* KeyObjectHandle */

typedef struct {
  KeyData *data;
} KeyHandle;

KeyData *crypto_key_handle_data(JSValueConst obj) {
  KeyHandle *h = JS_GetOpaque(obj, crypto_key_handle_class_id);
  return h ? h->data : NULL;
}

static void key_handle_finalizer(JSRuntime *rt, JSValueConst val) {
  KeyHandle *h = JS_GetOpaque(val, crypto_key_handle_class_id);
  if (!h)
    return;
  key_data_unref(h->data);
  free(h);
}

static JSValue key_handle_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, crypto_key_handle_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(KeyHandle)));
  return obj;
}

JSValue crypto_key_handle_new(JSContext *ctx, KeyData *k) {
  JSValue obj = JS_NewObjectClass(ctx, crypto_key_handle_class_id);
  KeyHandle *h;
  if (JS_IsException(obj))
    return obj;
  h = calloc(1, sizeof(*h));
  h->data = key_data_ref(k);
  JS_SetOpaque(obj, h);
  return obj;
}

static KeyHandle *handle_of(JSContext *ctx, JSValueConst v) {
  KeyHandle *h = JS_GetOpaque2(ctx, v, crypto_key_handle_class_id);
  if (h && !h->data) {
    JS_ThrowTypeError(ctx, "KeyObjectHandle is not initialized");
    return NULL;
  }
  return h;
}

/* init(type, ...) */
static JSValue kh_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  KeyHandle *h = JS_GetOpaque2(ctx, this_val, crypto_key_handle_class_id);
  int type = nb_int32(ctx, ARG(0), 0), offset;
  KeyData *k = NULL;
  if (!h)
    return JS_EXCEPTION;
  if (type == kKeyTypeSecret) {
    if (argc == 5 && JS_IsNumber(ARG(2)) && nb_int32(ctx, ARG(2), -1) == kKeyFormatJWK) {
      k = import_jwk_secret(ctx, ARG(1));
    } else {
      size_t len;
      uint8_t *d = crypto_bytes_copy(ctx, ARG(1), &len);
      if (!d)
        return JS_EXCEPTION;
      k = key_data_new_secret(d, len);
      OPENSSL_cleanse(d, len);
      free(d);
    }
    if (!k)
      return JS_EXCEPTION;
  } else {
    int fmt = JS_IsNumber(ARG(2)) ? nb_int32(ctx, ARG(2), -1) : -1;
    if (fmt == kKeyFormatJWK) {
      k = import_jwk(ctx, ARG(1));
      if (!k)
        return JS_EXCEPTION;
      if (type == kKeyTypePublic && k->type == kKeyTypePrivate) {
        KeyData *pub = key_data_with_type(k, kKeyTypePublic);
        key_data_unref(k);
        k = pub;
      } else if (type == kKeyTypePrivate && k->type == kKeyTypePublic) {
        key_data_unref(k);
        return throw_code(ctx, "ERR_CRYPTO_INVALID_JWK", "JWK does not contain private key material");
      }
      key_data_unref(h->data);
      h->data = k;
      return JS_NewInt32(ctx, k->type);
    }
    offset = 1;
    if (type == kKeyTypePublic) {
      KeyData *any = crypto_get_public_or_private_key(ctx, argv, argc, &offset);
      if (!any)
        return JS_EXCEPTION;
      k = key_data_with_type(any, kKeyTypePublic);
      key_data_unref(any);
    } else {
      k = crypto_get_private_key(ctx, argv, argc, &offset, false);
      if (!k)
        return JS_EXCEPTION;
    }
  }
  key_data_unref(h->data);
  h->data = k;
  return JS_UNDEFINED;
}

static JSValue kh_get_key_type(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  return h ? JS_NewUint32(ctx, h->data->type) : JS_EXCEPTION;
}

static JSValue kh_symmetric_size(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  return h ? JS_NewUint32(ctx, (uint32_t)h->data->secret_len) : JS_EXCEPTION;
}

const char *crypto_asymmetric_key_type(EVP_PKEY *pkey) {
  switch (EVP_PKEY_get_base_id(pkey)) {
  case EVP_PKEY_RSA: return "rsa";
  case EVP_PKEY_RSA_PSS: return "rsa-pss";
  case EVP_PKEY_DSA: return "dsa";
  case EVP_PKEY_DH: return "dh";
  case EVP_PKEY_EC: return "ec";
  case EVP_PKEY_ED25519: return "ed25519";
  case EVP_PKEY_ED448: return "ed448";
  case EVP_PKEY_X25519: return "x25519";
  case EVP_PKEY_X448: return "x448";
  }
  return NULL;
}

static JSValue kh_asymmetric_type(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  const char *t;
  if (!h)
    return JS_EXCEPTION;
  if (!h->data->pkey)
    return JS_UNDEFINED;
  t = crypto_asymmetric_key_type(h->data->pkey);
  return t ? JS_NewString(ctx, t) : JS_UNDEFINED;
}

static JSValue kh_check_ec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  EVP_PKEY_CTX *c;
  int ok;
  if (!h)
    return JS_EXCEPTION;
  c = EVP_PKEY_CTX_new(h->data->pkey, NULL);
  ok = h->data->type == kKeyTypePrivate ? EVP_PKEY_private_check(c) : EVP_PKEY_public_check(c);
  EVP_PKEY_CTX_free(c);
  ERR_clear_error();
  return JS_NewBool(ctx, ok == 1);
}

static JSValue kh_export(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  KeyEncoding enc;
  int offset = 0;
  JSValue r;
  if (!h)
    return JS_EXCEPTION;
  if (h->data->type == kKeyTypeSecret)
    return nb_new_buffer(ctx, h->data->secret, h->data->secret_len);
  if (h->data->type == kKeyTypePublic) {
    crypto_parse_public_encoding(ctx, argv, argc, &offset, &enc, false);
    return crypto_export_public(ctx, h->data, &enc);
  }
  if (crypto_parse_private_encoding(ctx, argv, argc, &offset, &enc, false) < 0)
    return JS_EXCEPTION;
  r = crypto_export_private(ctx, h->data, &enc);
  crypto_key_encoding_free(&enc);
  return r;
}

/* exportJwk(target, handleRsaPss) */
static JSValue kh_export_jwk(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  JSValue r;
  if (!h)
    return JS_EXCEPTION;
  r = export_jwk_obj(ctx, h->data, ARG(0), JS_ToBool(ctx, ARG(1)));
  if (JS_IsException(r))
    return r;
  return JS_DupValue(ctx, ARG(0));
}

static bool raw_supported(EVP_PKEY *pkey) {
  int id = EVP_PKEY_get_base_id(pkey);
  return id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448 || id == EVP_PKEY_X25519 ||
         id == EVP_PKEY_X448;
}

static JSValue kh_raw_public(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  uint8_t buf[256];
  size_t len = sizeof(buf);
  if (!h)
    return JS_EXCEPTION;
  if (!h->data->pkey || !raw_supported(h->data->pkey))
    return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
  if (EVP_PKEY_get_raw_public_key(h->data->pkey, buf, &len) != 1)
    return throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to get raw public key");
  return nb_new_buffer(ctx, buf, len);
}

static JSValue kh_raw_private(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  uint8_t buf[256];
  size_t len = sizeof(buf);
  JSValue r;
  if (!h)
    return JS_EXCEPTION;
  if (!h->data->pkey || !raw_supported(h->data->pkey))
    return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
  if (EVP_PKEY_get_raw_private_key(h->data->pkey, buf, &len) != 1)
    return throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to get raw private key");
  r = nb_new_buffer(ctx, buf, len);
  OPENSSL_cleanse(buf, len);
  return r;
}

static JSValue kh_raw_seed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
}

JSValue crypto_ec_point_to_buffer(JSContext *ctx, const EC_GROUP *g, const EC_POINT *pt,
                                  int form) {
  size_t len = EC_POINT_point2oct(g, pt, form, NULL, 0, NULL);
  uint8_t *buf;
  JSValue r;
  if (!len)
    return crypto_throw(ctx, ERR_get_error(), "Failed to get public key length");
  buf = malloc(len);
  EC_POINT_point2oct(g, pt, form, buf, len, NULL);
  r = nb_new_buffer(ctx, buf, len);
  free(buf);
  return r;
}

static JSValue kh_export_ec_public_raw(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  const EC_KEY *ec;
  if (!h)
    return JS_EXCEPTION;
  if (!h->data->pkey || EVP_PKEY_get_base_id(h->data->pkey) != EVP_PKEY_EC ||
      !(ec = EVP_PKEY_get0_EC_KEY(h->data->pkey)))
    return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
  if (!EC_KEY_get0_public_key(ec))
    return throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to export EC public key");
  return crypto_ec_point_to_buffer(ctx, EC_KEY_get0_group(ec), EC_KEY_get0_public_key(ec),
                                   nb_int32(ctx, ARG(0), POINT_CONVERSION_UNCOMPRESSED));
}

static JSValue kh_export_ec_private_raw(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  const EC_KEY *ec;
  const BIGNUM *priv;
  BIGNUM *order;
  int n;
  uint8_t *buf;
  JSValue r;
  if (!h)
    return JS_EXCEPTION;
  if (!h->data->pkey || EVP_PKEY_get_base_id(h->data->pkey) != EVP_PKEY_EC ||
      !(ec = EVP_PKEY_get0_EC_KEY(h->data->pkey)))
    return throw_code(ctx, "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", NULL);
  priv = EC_KEY_get0_private_key(ec);
  order = BN_new();
  if (!priv || !EC_GROUP_get_order(EC_KEY_get0_group(ec), order, NULL)) {
    BN_free(order);
    return throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to export EC private key");
  }
  n = BN_num_bytes(order);
  BN_free(order);
  buf = malloc(n);
  BN_bn2binpad(priv, buf, n);
  r = nb_new_buffer(ctx, buf, n);
  OPENSSL_cleanse(buf, n);
  free(buf);
  return r;
}

static JSValue kh_key_detail(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  KeyHandle *h = handle_of(ctx, this_val);
  JSValueConst t = ARG(0);
  EVP_PKEY *pkey;
  if (!h)
    return JS_EXCEPTION;
  if (h->data->type == kKeyTypeSecret) {
    JS_SetPropertyStr(ctx, t, "length", JS_NewFloat64(ctx, (double)h->data->secret_len * 8));
    return JS_DupValue(ctx, t);
  }
  pkey = h->data->pkey;
  switch (EVP_PKEY_get_base_id(pkey)) {
  case EVP_PKEY_RSA:
  case EVP_PKEY_RSA2:
  case EVP_PKEY_RSA_PSS: {
    BIGNUM *n = NULL, *e = NULL;
    uint8_t *buf;
    int el;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
    JS_SetPropertyStr(ctx, t, "modulusLength", JS_NewFloat64(ctx, n ? BN_num_bits(n) : 0));
    el = e ? BN_num_bytes(e) : 0;
    buf = malloc(el ? el : 1);
    if (e)
      BN_bn2binpad(e, buf, el);
    JS_SetPropertyStr(ctx, t, "publicExponent", crypto_new_array_buffer(ctx, buf, el));
    free(buf);
    BN_free(n);
    BN_free(e);
    if (EVP_PKEY_get_base_id(pkey) == EVP_PKEY_RSA_PSS) {
      char md[64] = "", mgf[64] = "";
      int salt = -1;
      if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_RSA_DIGEST, md, sizeof(md), NULL) == 1
          && md[0]) {
        char low[64];
        size_t i;
        snprintf(low, sizeof(low), "%s", md);
        for (i = 0; low[i]; i++)
          low[i] = (char)tolower((unsigned char)low[i]);
        /* OpenSSL names SHA2-256: Node reports sha256 */
        if (!strncmp(low, "sha2-", 5))
          memmove(low + 3, low + 5, strlen(low + 5) + 1);
        JS_SetPropertyStr(ctx, t, "hashAlgorithm", JS_NewString(ctx, low));
        if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_RSA_MGF1_DIGEST, mgf,
                                           sizeof(mgf), NULL) == 1 && mgf[0]) {
          snprintf(low, sizeof(low), "%s", mgf);
          for (i = 0; low[i]; i++)
            low[i] = (char)tolower((unsigned char)low[i]);
          if (!strncmp(low, "sha2-", 5))
            memmove(low + 3, low + 5, strlen(low + 5) + 1);
          JS_SetPropertyStr(ctx, t, "mgf1HashAlgorithm", JS_NewString(ctx, low));
        }
        if (EVP_PKEY_get_int_param(pkey, OSSL_PKEY_PARAM_RSA_PSS_SALTLEN, &salt) == 1)
          JS_SetPropertyStr(ctx, t, "saltLength", JS_NewInt32(ctx, salt));
      }
      ERR_clear_error();
    }
    break;
  }
  case EVP_PKEY_DSA: {
    BIGNUM *p = NULL, *q = NULL;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_P, &p);
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_Q, &q);
    JS_SetPropertyStr(ctx, t, "modulusLength", JS_NewFloat64(ctx, p ? BN_num_bits(p) : 0));
    JS_SetPropertyStr(ctx, t, "divisorLength", JS_NewFloat64(ctx, q ? BN_num_bits(q) : 0));
    BN_free(p);
    BN_free(q);
    break;
  }
  case EVP_PKEY_EC: {
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
    int nid = ec ? EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) : NID_undef;
    if (nid != NID_undef)
      JS_SetPropertyStr(ctx, t, "namedCurve", JS_NewString(ctx, OBJ_nid2sn(nid)));
    break;
  }
  case EVP_PKEY_DH:
  case EVP_PKEY_ED25519:
  case EVP_PKEY_ED448:
  case EVP_PKEY_X25519:
  case EVP_PKEY_X448:
    break;
  default:
    return throw_code(ctx, "ERR_CRYPTO_INVALID_KEYTYPE", NULL);
  }
  return JS_DupValue(ctx, t);
}

static JSValue kh_equals(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  KeyHandle *a = handle_of(ctx, this_val), *b;
  if (!a)
    return JS_EXCEPTION;
  b = handle_of(ctx, ARG(0));
  if (!b)
    return JS_EXCEPTION;
  if (a->data->type != b->data->type)
    return JS_FALSE;
  if (a->data->type == kKeyTypeSecret)
    return JS_NewBool(ctx, a->data->secret_len == b->data->secret_len &&
                               CRYPTO_memcmp(a->data->secret, b->data->secret,
                                             a->data->secret_len) == 0);
  {
    int ok = EVP_PKEY_eq(a->data->pkey, b->data->pkey);
    if (ok == -2)
      return throw_code(ctx, "ERR_CRYPTO_UNSUPPORTED_OPERATION", NULL);
    return JS_NewBool(ctx, ok == 1);
  }
}

static const JSCFunctionListEntry key_handle_proto[] = {
  JS_CFUNC_DEF("init", 6, kh_init),
  JS_CFUNC_DEF("getKeyType", 0, kh_get_key_type),
  JS_CFUNC_DEF("getSymmetricKeySize", 0, kh_symmetric_size),
  JS_CFUNC_DEF("getAsymmetricKeyType", 0, kh_asymmetric_type),
  JS_CFUNC_DEF("checkEcKeyData", 0, kh_check_ec),
  JS_CFUNC_DEF("export", 4, kh_export),
  JS_CFUNC_DEF("exportJwk", 2, kh_export_jwk),
  JS_CFUNC_DEF("rawPublicKey", 0, kh_raw_public),
  JS_CFUNC_DEF("rawPrivateKey", 0, kh_raw_private),
  JS_CFUNC_DEF("rawSeed", 0, kh_raw_seed),
  JS_CFUNC_DEF("exportECPublicRaw", 1, kh_export_ec_public_raw),
  JS_CFUNC_DEF("exportECPrivateRaw", 0, kh_export_ec_private_raw),
  JS_CFUNC_DEF("keyDetail", 1, kh_key_detail),
  JS_CFUNC_DEF("equals", 1, kh_equals),
};

/* ---------------------------------------------------------------------- */
/* NativeKeyObject: the base class of KeyObject */

typedef struct {
  KeyData *data;
} NativeKey;

static void native_key_finalizer(JSRuntime *rt, JSValueConst val) {
  NativeKey *n = JS_GetOpaque(val, native_key_object_class_id);
  if (!n)
    return;
  key_data_unref(n->data);
  free(n);
}

static JSValue native_key_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  KeyData *k = crypto_key_handle_data(ARG(0));
  JSValue obj;
  NativeKey *n;
  if (!k)
    return JS_ThrowTypeError(ctx, "handle must be a KeyObjectHandle");
  obj = nb_new_instance(ctx, nt, native_key_object_class_id);
  if (JS_IsException(obj))
    return obj;
  n = calloc(1, sizeof(*n));
  n->data = key_data_ref(k);
  JS_SetOpaque(obj, n);
  return obj;
}

static JSValue create_native_key_object_class(JSContext *ctx, JSValueConst this_val, int argc,
                                              JSValueConst *argv) {
  Env *env = env_get(ctx);
  NodeClassDef def = { .name = "NativeKeyObject", .class_id = &native_key_object_class_id,
                       .ctor = native_key_ctor, .ctor_length = 1,
                       .finalizer = native_key_finalizer, .parent_ctor = JS_UNDEFINED };
  JSValue ctor = nb_define_class(ctx, JS_UNDEFINED, &def), ret;
  ret = JS_Call(ctx, ARG(0), JS_UNDEFINED, 1, (JSValueConst *)&ctor);
  JS_FreeValue(ctx, ctor);
  if (JS_IsException(ret))
    return ret;
  JS_SetPropertyStr(ctx, env->binding_data, "cryptoKeyObjectSecret",
                    JS_GetPropertyUint32(ctx, ret, 1));
  JS_SetPropertyStr(ctx, env->binding_data, "cryptoKeyObjectPublic",
                    JS_GetPropertyUint32(ctx, ret, 2));
  JS_SetPropertyStr(ctx, env->binding_data, "cryptoKeyObjectPrivate",
                    JS_GetPropertyUint32(ctx, ret, 3));
  return ret;
}

static JSValue get_key_object_slots(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  NativeKey *n = JS_GetOpaque(ARG(0), native_key_object_class_id);
  JSValue arr;
  if (!n || !n->data)
    return node_throw_type_error(ctx, "ERR_INVALID_THIS",
                                 "Value of \"this\" must be of type KeyObject");
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewUint32(ctx, n->data->type));
  JS_SetPropertyUint32(ctx, arr, 1, crypto_key_handle_new(ctx, n->data));
  return arr;
}

/* ---------------------------------------------------------------------- */
/* NativeCryptoKey: the base class of CryptoKey */

typedef struct {
  KeyData *data;
  JSValue algorithm;
  uint32_t usages;
  bool extractable;
} NativeCryptoKey;

static void native_crypto_key_finalizer(JSRuntime *rt, JSValueConst val) {
  NativeCryptoKey *n = JS_GetOpaque(val, native_crypto_key_class_id);
  if (!n)
    return;
  key_data_unref(n->data);
  JS_FreeValueRT(rt, n->algorithm);
  free(n);
}

static void native_crypto_key_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  NativeCryptoKey *n = JS_GetOpaque(val, native_crypto_key_class_id);
  if (n)
    JS_MarkValue(rt, n->algorithm, mark);
}

static JSValue native_crypto_key_ctor(JSContext *ctx, JSValueConst nt, int argc,
                                      JSValueConst *argv) {
  KeyData *k = crypto_key_handle_data(ARG(0));
  JSValue obj;
  NativeCryptoKey *n;
  if (!k)
    return JS_ThrowTypeError(ctx, "handle must be a KeyObjectHandle");
  obj = nb_new_instance(ctx, nt, native_crypto_key_class_id);
  if (JS_IsException(obj))
    return obj;
  n = calloc(1, sizeof(*n));
  n->data = key_data_ref(k);
  n->algorithm = JS_UNDEFINED;
  if (!JS_IsUndefined(ARG(1))) {
    n->algorithm = JS_DupValue(ctx, ARG(1));
    n->usages = nb_uint32(ctx, ARG(2), 0);
    n->extractable = JS_ToBool(ctx, ARG(3));
  }
  JS_SetOpaque(obj, n);
  return obj;
}

static JSValue create_crypto_key_class(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  Env *env = env_get(ctx);
  NodeClassDef def = { .name = "NativeCryptoKey", .class_id = &native_crypto_key_class_id,
                       .ctor = native_crypto_key_ctor, .ctor_length = 4,
                       .finalizer = native_crypto_key_finalizer,
                       .gc_mark = native_crypto_key_mark, .parent_ctor = JS_UNDEFINED };
  JSValue ctor = nb_define_class(ctx, JS_UNDEFINED, &def), ret;
  ret = JS_Call(ctx, ARG(0), JS_UNDEFINED, 1, (JSValueConst *)&ctor);
  JS_FreeValue(ctx, ctor);
  if (JS_IsException(ret))
    return ret;
  JS_SetPropertyStr(ctx, env->binding_data, "internalCryptoKey",
                    JS_GetPropertyUint32(ctx, ret, 1));
  return ret;
}

static JSValue get_crypto_key_slots(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  NativeCryptoKey *n = JS_GetOpaque(ARG(0), native_crypto_key_class_id);
  JSValue arr;
  if (!n)
    return node_throw_type_error(ctx, "ERR_INVALID_THIS",
                                 "Value of \"this\" must be of type CryptoKey");
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewUint32(ctx, n->data->type));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewBool(ctx, n->extractable));
  JS_SetPropertyUint32(ctx, arr, 2, JS_DupValue(ctx, n->algorithm));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewUint32(ctx, n->usages));
  JS_SetPropertyUint32(ctx, arr, 4, crypto_key_handle_new(ctx, n->data));
  return arr;
}

JSValue crypto_new_crypto_key(JSContext *ctx, KeyData *k, JSValueConst algorithm,
                              uint32_t usages, bool extractable) {
  Env *env = env_get(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, env->binding_data, "internalCryptoKey"), args[4], r;
  if (!JS_IsFunction(ctx, ctor)) {
    JSValue id = JS_NewString(ctx, "internal/crypto/keys"), m;
    JS_FreeValue(ctx, ctor);
    m = JS_Call(ctx, env->builtin_require, JS_NULL, 1, (JSValueConst *)&id);
    JS_FreeValue(ctx, id);
    if (JS_IsException(m))
      return m;
    JS_FreeValue(ctx, m);
    ctor = JS_GetPropertyStr(ctx, env->binding_data, "internalCryptoKey");
  }
  args[0] = crypto_key_handle_new(ctx, k);
  args[1] = (JSValue)algorithm;
  args[2] = JS_NewUint32(ctx, usages);
  args[3] = JS_NewBool(ctx, extractable);
  r = JS_CallConstructor(ctx, ctor, 4, (JSValueConst *)args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, ctor);
  return r;
}

void crypto_init_keys(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef def = { .name = "KeyObjectHandle", .class_id = &crypto_key_handle_class_id,
                       .ctor = key_handle_ctor, .finalizer = key_handle_finalizer,
                       .proto_funcs = key_handle_proto,
                       .proto_funcs_count = countof(key_handle_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, target, &def));
  nb_set_method(ctx, target, "createNativeKeyObjectClass", create_native_key_object_class, 1);
  nb_set_method(ctx, target, "getKeyObjectSlots", get_key_object_slots, 1);
  nb_set_method(ctx, target, "createCryptoKeyClass", create_crypto_key_class, 1);
  nb_set_method(ctx, target, "getCryptoKeySlots", get_crypto_key_slots, 1);
}
