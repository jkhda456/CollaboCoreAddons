/* Signatures, key generation and key agreement for internalBinding('crypto')
 * (crypto_sig.cc, crypto_keygen.cc, crypto_rsa.cc, crypto_ec.cc,
 * crypto_dsa.cc, crypto_dh.cc): Sign, Verify, SignJob, *KeyPairGenJob,
 * SecretKeyGenJob, ECDH, ECDHConvertKey, DiffieHellman(Group), DHBitsJob. */
#include <openssl/core_names.h>
#include <openssl/dh.h>
#include <openssl/dsa.h>
#include <openssl/ec.h>
#include <openssl/rsa.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

static JSClassID sign_class_id, verify_class_id, ecdh_class_id, dh_class_id;

/* ---------------------------------------------------------------------- */
/* signature helpers */

static bool is_one_shot(EVP_PKEY *pkey) {
  int id = EVP_PKEY_get_base_id(pkey);
  return id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448;
}

static bool is_rsa(EVP_PKEY *pkey) {
  int id = EVP_PKEY_get_base_id(pkey);
  return id == EVP_PKEY_RSA || id == EVP_PKEY_RSA2 || id == EVP_PKEY_RSA_PSS;
}

static int default_padding(EVP_PKEY *pkey) {
  return EVP_PKEY_get_base_id(pkey) == EVP_PKEY_RSA_PSS ? RSA_PKCS1_PSS_PADDING
                                                        : RSA_PKCS1_PADDING;
}

/* bytes per r / s for P1363 (GetBytesOfRS) */
static int rs_bytes(EVP_PKEY *pkey) {
  int bits = 0, id = EVP_PKEY_get_base_id(pkey);
  if (id == EVP_PKEY_DSA) {
    BIGNUM *q = NULL;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_Q, &q);
    bits = q ? BN_num_bits(q) : 0;
    BN_free(q);
  } else if (id == EVP_PKEY_EC) {
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
    bits = EC_GROUP_order_bits(EC_KEY_get0_group(ec));
  } else {
    return -1;
  }
  return (bits + 7) / 8;
}

/* DER (EC)DSA signature -> r || s */
static bool der_to_p1363(EVP_PKEY *pkey, uint8_t **sig, size_t *len) {
  int n = rs_bytes(pkey);
  const uint8_t *p = *sig;
  ECDSA_SIG *es;
  const BIGNUM *r, *s;
  uint8_t *out;
  if (n <= 0)
    return true;
  es = d2i_ECDSA_SIG(NULL, &p, (long)*len);
  if (!es)
    return false;
  ECDSA_SIG_get0(es, &r, &s);
  out = malloc(2 * n);
  BN_bn2binpad(r, out, n);
  BN_bn2binpad(s, out + n, n);
  ECDSA_SIG_free(es);
  free(*sig);
  *sig = out;
  *len = 2 * n;
  return true;
}

/* r || s -> DER; NULL if malformed */
static uint8_t *p1363_to_der(EVP_PKEY *pkey, const uint8_t *sig, size_t len, size_t *outlen) {
  int n = rs_bytes(pkey);
  ECDSA_SIG *es;
  uint8_t *out = NULL, *p;
  int l;
  if (n <= 0 || len != (size_t)(2 * n))
    return NULL;
  es = ECDSA_SIG_new();
  ECDSA_SIG_set0(es, BN_bin2bn(sig, n, NULL), BN_bin2bn(sig + n, n, NULL));
  l = i2d_ECDSA_SIG(es, NULL);
  if (l > 0) {
    out = malloc(l);
    p = out;
    i2d_ECDSA_SIG(es, &p);
    *outlen = l;
  }
  ECDSA_SIG_free(es);
  return out;
}

static bool apply_rsa_options(EVP_PKEY *pkey, EVP_PKEY_CTX *pctx, int padding, bool has_salt,
                              int salt) {
  if (!is_rsa(pkey))
    return true;
  if (EVP_PKEY_CTX_set_rsa_padding(pctx, padding) <= 0)
    return false;
  if (padding == RSA_PKCS1_PSS_PADDING && has_salt &&
      EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt) <= 0)
    return false;
  return true;
}

/* ---------------------------------------------------------------------- */
/* Sign / Verify (streaming) */

typedef struct {
  EVP_MD_CTX *mdctx;
} SignBase;

static void sign_finalizer(JSRuntime *rt, JSValueConst val) {
  JSClassID id;
  SignBase *s = JS_GetAnyOpaque(val, &id);
  if (!s)
    return;
  EVP_MD_CTX_free(s->mdctx);
  free(s);
}

static JSValue sign_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, sign_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(SignBase)));
  return obj;
}

static JSValue verify_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, verify_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(SignBase)));
  return obj;
}

static SignBase *sb_of(JSContext *ctx, JSValueConst v) {
  JSClassID id;
  SignBase *s = JS_GetAnyOpaque(v, &id);
  if (!s || (id != sign_class_id && id != verify_class_id)) {
    JS_ThrowTypeError(ctx, "Illegal invocation");
    return NULL;
  }
  return s;
}

static JSValue sign_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SignBase *s = sb_of(ctx, this_val);
  const char *name;
  const EVP_MD *md;
  if (!s)
    return JS_EXCEPTION;
  name = JS_ToCString(ctx, ARG(0));
  md = crypto_get_digest(name);
  JS_FreeCString(ctx, name);
  if (!md)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", NULL);
  EVP_MD_CTX_free(s->mdctx);
  s->mdctx = EVP_MD_CTX_new();
  if (EVP_DigestInit_ex(s->mdctx, md, NULL) != 1) {
    EVP_MD_CTX_free(s->mdctx);
    s->mdctx = NULL;
    return crypto_throw(ctx, ERR_get_error(), NULL);
  }
  return JS_UNDEFINED;
}

static JSValue sign_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SignBase *s = sb_of(ctx, this_val);
  uint8_t *owned = NULL, *d;
  size_t len;
  int ok;
  if (!s)
    return JS_EXCEPTION;
  if (JS_IsString(ARG(0))) {
    int enc = node_parse_encoding(ctx, ARG(1), ENC_UTF8);
    size_t n = node_string_bytes_size(ctx, ARG(0), enc);
    owned = malloc(n ? n : 1);
    len = node_string_write(ctx, owned, n, ARG(0), enc, NULL);
    d = owned;
  } else {
    d = crypto_buffer_source(ctx, ARG(0), &len);
  }
  if (!s->mdctx) {
    free(owned);
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_STATE", "Not initialised");
  }
  ok = EVP_DigestUpdate(s->mdctx, d, len);
  free(owned);
  if (ok != 1)
    return crypto_throw(ctx, ERR_get_error(), NULL);
  return JS_UNDEFINED;
}

/* sign(key group x5, padding, saltLength, dsaSigEnc) */
static JSValue sign_sign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  SignBase *s = sb_of(ctx, this_val);
  int offset = 0, padding, enc;
  bool has_salt;
  int salt;
  KeyData *k;
  uint8_t md[EVP_MAX_MD_SIZE], *sig = NULL;
  unsigned mdlen;
  size_t siglen = 0;
  EVP_PKEY_CTX *pctx = NULL;
  JSValue ret;
  if (!s)
    return JS_EXCEPTION;
  k = crypto_get_private_key(ctx, argv, argc, &offset, true);
  if (!k)
    return JS_EXCEPTION;
  if (is_one_shot(k->pkey)) {
    key_data_unref(k);
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNSUPPORTED_OPERATION", NULL);
  }
  padding = JS_IsUndefined(ARG(offset)) ? default_padding(k->pkey) : nb_int32(ctx, ARG(offset), 0);
  has_salt = !JS_IsUndefined(ARG(offset + 1));
  salt = nb_int32(ctx, ARG(offset + 1), 0);
  enc = JS_IsUndefined(ARG(offset + 2)) ? kSigEncDER : nb_int32(ctx, ARG(offset + 2), 0);
  if (enc != kSigEncDER && enc != kSigEncP1363) {
    key_data_unref(k);
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "invalid signature encoding");
  }
  if (!s->mdctx) {
    key_data_unref(k);
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_STATE", "Not initialised");
  }
  if (EVP_DigestFinal_ex(s->mdctx, md, &mdlen) != 1)
    goto fail;
  pctx = EVP_PKEY_CTX_new(k->pkey, NULL);
  if (!pctx || EVP_PKEY_sign_init(pctx) <= 0 ||
      !apply_rsa_options(k->pkey, pctx, padding, has_salt, salt) ||
      EVP_PKEY_CTX_set_signature_md(pctx, EVP_MD_CTX_get0_md(s->mdctx)) <= 0 ||
      EVP_PKEY_sign(pctx, NULL, &siglen, md, mdlen) <= 0)
    goto fail;
  sig = malloc(siglen);
  if (EVP_PKEY_sign(pctx, sig, &siglen, md, mdlen) <= 0)
    goto fail;
  EVP_MD_CTX_free(s->mdctx);
  s->mdctx = NULL;
  if (enc == kSigEncP1363)
    der_to_p1363(k->pkey, &sig, &siglen);
  ret = nb_new_buffer(ctx, sig, siglen);
  free(sig);
  EVP_PKEY_CTX_free(pctx);
  key_data_unref(k);
  ERR_clear_error();
  return ret;
fail:
  EVP_MD_CTX_free(s->mdctx);
  s->mdctx = NULL;
  free(sig);
  EVP_PKEY_CTX_free(pctx);
  key_data_unref(k);
  {
    unsigned long e = ERR_get_error();
    if (e)
      return crypto_throw(ctx, e, NULL);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "PEM_read_bio_PrivateKey failed");
  }
}

/* verify(key group x5, signature, padding, saltLength, dsaSigEnc) */
static JSValue verify_verify(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  SignBase *s = sb_of(ctx, this_val);
  int offset = 0, padding, enc, r;
  bool has_salt;
  int salt;
  KeyData *k;
  uint8_t md[EVP_MAX_MD_SIZE], *sig, *der = NULL;
  unsigned mdlen;
  size_t siglen;
  EVP_PKEY_CTX *pctx = NULL;
  if (!s)
    return JS_EXCEPTION;
  k = crypto_get_public_or_private_key(ctx, argv, argc, &offset);
  if (!k)
    return JS_EXCEPTION;
  if (is_one_shot(k->pkey)) {
    key_data_unref(k);
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNSUPPORTED_OPERATION", NULL);
  }
  sig = crypto_buffer_source(ctx, ARG(offset), &siglen);
  padding = JS_IsUndefined(ARG(offset + 1)) ? default_padding(k->pkey)
                                            : nb_int32(ctx, ARG(offset + 1), 0);
  has_salt = !JS_IsUndefined(ARG(offset + 2));
  salt = nb_int32(ctx, ARG(offset + 2), 0);
  enc = JS_IsUndefined(ARG(offset + 3)) ? kSigEncDER : nb_int32(ctx, ARG(offset + 3), 0);
  if (!s->mdctx) {
    key_data_unref(k);
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_STATE", "Not initialised");
  }
  if (enc == kSigEncP1363 && rs_bytes(k->pkey) > 0) {
    der = p1363_to_der(k->pkey, sig, siglen, &siglen);
    if (!der) {
      EVP_MD_CTX_free(s->mdctx);
      s->mdctx = NULL;
      key_data_unref(k);
      return JS_FALSE;
    }
    sig = der;
  }
  r = EVP_DigestFinal_ex(s->mdctx, md, &mdlen);
  pctx = EVP_PKEY_CTX_new(k->pkey, NULL);
  if (r == 1)
    r = pctx && EVP_PKEY_verify_init(pctx) > 0 &&
        apply_rsa_options(k->pkey, pctx, padding, has_salt, salt) &&
        EVP_PKEY_CTX_set_signature_md(pctx, EVP_MD_CTX_get0_md(s->mdctx)) > 0;
  if (r == 1)
    r = EVP_PKEY_verify(pctx, sig, siglen, md, mdlen);
  EVP_MD_CTX_free(s->mdctx);
  s->mdctx = NULL;
  EVP_PKEY_CTX_free(pctx);
  free(der);
  key_data_unref(k);
  ERR_clear_error();
  return JS_NewBool(ctx, r == 1);
}

static const JSCFunctionListEntry sign_proto[] = {
  JS_CFUNC_DEF("init", 1, sign_init),
  JS_CFUNC_DEF("update", 2, sign_update),
  JS_CFUNC_DEF("sign", 8, sign_sign),
};

static const JSCFunctionListEntry verify_proto[] = {
  JS_CFUNC_DEF("init", 1, sign_init),
  JS_CFUNC_DEF("update", 2, sign_update),
  JS_CFUNC_DEF("verify", 9, verify_verify),
};

/* ---------------------------------------------------------------------- */
/* SignJob(mode, signMode, key x5, data, algorithm, saltLength, padding,
           dsaSigEnc, context, signature) */

typedef struct {
  int sign_mode;
  KeyData *key;
  uint8_t *data, *sig, *out;
  size_t datalen, siglen, outlen;
  const EVP_MD *md;
  bool has_salt;
  int salt, padding, enc;
  bool verified;
} SignParams;

static int sign_job_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  SignParams *p = job->params;
  int offset = 1;
  p->sign_mode = nb_int32(ctx, ARG(0), kSignJobModeSign);
  if (p->sign_mode == kSignJobModeSign)
    p->key = crypto_get_private_key(ctx, argv, argc, &offset, true);
  else
    p->key = crypto_get_public_or_private_key(ctx, argv, argc, &offset);
  if (!p->key)
    return -1;
  if (!(p->data = crypto_bytes_copy(ctx, ARG(offset), &p->datalen)))
    return -1;
  if (JS_IsString(ARG(offset + 1))) {
    const char *n = JS_ToCString(ctx, ARG(offset + 1));
    p->md = crypto_get_digest(n);
    JS_FreeCString(ctx, n);
    if (!p->md)
      return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", NULL), -1;
  }
  p->has_salt = JS_IsNumber(ARG(offset + 2));
  p->salt = nb_int32(ctx, ARG(offset + 2), 0);
  p->padding = JS_IsNumber(ARG(offset + 3)) ? nb_int32(ctx, ARG(offset + 3), 0)
                                            : default_padding(p->key->pkey);
  p->enc = JS_IsNumber(ARG(offset + 4)) ? nb_int32(ctx, ARG(offset + 4), 0) : kSigEncDER;
  if (crypto_is_buffer_source(ctx, ARG(offset + 5))) {
    size_t cl;
    crypto_buffer_source(ctx, ARG(offset + 5), &cl);
    if (cl)
      return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED",
                               "Context parameter is unsupported"), -1;
  }
  if (p->sign_mode == kSignJobModeVerify &&
      !(p->sig = crypto_bytes_copy(ctx, ARG(offset + 6), &p->siglen)))
    return -1;
  return 0;
}

static void sign_job_work(CryptoJob *job) {
  SignParams *p = job->params;
  EVP_MD_CTX *mc = EVP_MD_CTX_new();
  EVP_PKEY_CTX *pctx = NULL;
  int ok;
  if (p->sign_mode == kSignJobModeVerify) {
    uint8_t *sig = p->sig, *der = NULL;
    size_t siglen = p->siglen;
    ok = EVP_DigestVerifyInit(mc, &pctx, p->md, NULL, p->key->pkey) == 1 &&
         apply_rsa_options(p->key->pkey, pctx, p->padding, p->has_salt, p->salt);
    if (ok && p->enc == kSigEncP1363 && rs_bytes(p->key->pkey) > 0) {
      der = p1363_to_der(p->key->pkey, sig, siglen, &siglen);
      sig = der;
    }
    p->verified = ok && sig && EVP_DigestVerify(mc, sig, siglen, p->data, p->datalen) == 1;
    free(der);
    EVP_MD_CTX_free(mc);
    ERR_clear_error();
    return;
  }
  ok = EVP_DigestSignInit(mc, &pctx, p->md, NULL, p->key->pkey) == 1 &&
       apply_rsa_options(p->key->pkey, pctx, p->padding, p->has_salt, p->salt) &&
       EVP_DigestSign(mc, NULL, &p->outlen, p->data, p->datalen) == 1;
  if (ok) {
    p->out = malloc(p->outlen);
    ok = EVP_DigestSign(mc, p->out, &p->outlen, p->data, p->datalen) == 1;
  }
  EVP_MD_CTX_free(mc);
  if (!ok) {
    crypto_job_capture_errors(job, NULL);
    return;
  }
  if (p->enc == kSigEncP1363)
    der_to_p1363(p->key->pkey, &p->out, &p->outlen);
}

static int sign_job_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  SignParams *p = job->params;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  if (p->sign_mode == kSignJobModeSign)
    *res = crypto_new_array_buffer(ctx, p->out, p->outlen);
  else
    *res = JS_NewBool(ctx, p->verified);
  return 0;
}

static void sign_job_cleanup(CryptoJob *job) {
  SignParams *p = job->params;
  key_data_unref(p->key);
  free(p->data);
  free(p->sig);
  free(p->out);
}

static const CryptoJobTraits sign_job = {
  "SignJob", PROVIDER_SIGNREQUEST, sizeof(SignParams),
  sign_job_config, sign_job_work, sign_job_result, sign_job_cleanup,
};

/* ---------------------------------------------------------------------- */
/* key generation */

enum { KG_RSA, KG_RSA_PSS, KG_DSA, KG_EC, KG_NID, KG_DH };

typedef struct {
  int kind;
  /* parameters */
  uint32_t modulus_bits, exponent, divisor_bits;
  const EVP_MD *md, *mgf1_md;
  int salt;
  bool has_salt;
  int curve_nid, param_encoding, nid;
  BIGNUM *prime;
  int prime_bits, generator;
  char group[32];
  /* output */
  KeyEncoding pub_enc, priv_enc;
  bool webcrypto;
  JSValue algorithm;
  uint32_t pub_usages, priv_usages;
  bool extractable;
  KeyData *key;
} KeyGenParams;

static int keygen_tail(JSContext *ctx, CryptoJob *job, KeyGenParams *p, int argc,
                       JSValueConst *argv, int offset) {
  p->algorithm = JS_UNDEFINED;
  if (job->mode == kCryptoJobWebCrypto) {
    p->webcrypto = true;
    p->algorithm = JS_DupValue(ctx, ARG(offset));
    p->pub_usages = nb_uint32(ctx, ARG(offset + 1), 0);
    p->priv_usages = nb_uint32(ctx, ARG(offset + 2), 0);
    p->extractable = JS_ToBool(ctx, ARG(offset + 3));
    return 0;
  }
  if (crypto_parse_public_encoding(ctx, argv, argc, &offset, &p->pub_enc, true) < 0 ||
      crypto_parse_private_encoding(ctx, argv, argc, &offset, &p->priv_enc, true) < 0)
    return -1;
  return 0;
}

static const EVP_MD *md_arg(JSContext *ctx, JSValueConst v, bool *bad) {
  const char *n;
  const EVP_MD *md;
  *bad = false;
  if (!JS_IsString(v))
    return NULL;
  n = JS_ToCString(ctx, v);
  md = crypto_get_digest(n);
  JS_FreeCString(ctx, n);
  if (!md) {
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid MGF1 digest: %s", "");
    *bad = true;
  }
  return md;
}

/* RsaKeyPairGenJob(mode, variant, modulusLength, publicExponent,
                    [hash, mgf1Hash, saltLength,] ...) */
static int rsa_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KeyGenParams *p = job->params;
  int variant = nb_int32(ctx, ARG(0), 0), offset = 3;
  p->kind = variant == 1 ? KG_RSA_PSS : KG_RSA;
  p->modulus_bits = nb_uint32(ctx, ARG(1), 2048);
  p->exponent = nb_uint32(ctx, ARG(2), 65537);
  if (p->kind == KG_RSA_PSS && job->mode != kCryptoJobWebCrypto) {
    bool bad;
    if (JS_IsString(ARG(3))) {
      const char *n = JS_ToCString(ctx, ARG(3));
      p->md = crypto_get_digest(n);
      if (!p->md) {
        crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s", n);
        JS_FreeCString(ctx, n);
        return -1;
      }
      JS_FreeCString(ctx, n);
    }
    if (JS_IsString(ARG(4))) {
      const char *n = JS_ToCString(ctx, ARG(4));
      p->mgf1_md = crypto_get_digest(n);
      if (!p->mgf1_md) {
        crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid MGF1 digest: %s", n);
        JS_FreeCString(ctx, n);
        return -1;
      }
      JS_FreeCString(ctx, n);
    }
    (void)md_arg;
    (void)bad;
    p->has_salt = JS_IsNumber(ARG(5));
    p->salt = nb_int32(ctx, ARG(5), 0);
    offset = 6;
  }
  return keygen_tail(ctx, job, p, argc, argv, offset);
}

/* DsaKeyPairGenJob(mode, modulusLength, divisorLength, ...) */
static int dsa_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KeyGenParams *p = job->params;
  p->kind = KG_DSA;
  p->modulus_bits = nb_uint32(ctx, ARG(0), 2048);
  p->divisor_bits = JS_IsNumber(ARG(1)) && nb_int32(ctx, ARG(1), -1) >= 0
                        ? nb_uint32(ctx, ARG(1), 0) : 0;
  return keygen_tail(ctx, job, p, argc, argv, 2);
}

/* EcKeyPairGenJob(mode, namedCurve, paramEncoding, ...) */
static int ec_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KeyGenParams *p = job->params;
  const char *c = JS_ToCString(ctx, ARG(0));
  p->kind = KG_EC;
  p->curve_nid = crypto_nid_from_curve(c ? c : "");
  JS_FreeCString(ctx, c);
  if (p->curve_nid == NID_undef)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_CURVE", NULL), -1;
  p->param_encoding = JS_IsNumber(ARG(1)) ? nb_int32(ctx, ARG(1), OPENSSL_EC_NAMED_CURVE)
                                          : OPENSSL_EC_NAMED_CURVE;
  return keygen_tail(ctx, job, p, argc, argv, 2);
}

/* NidKeyPairGenJob(mode, nid, ...) */
static int nid_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KeyGenParams *p = job->params;
  p->kind = KG_NID;
  p->nid = nb_int32(ctx, ARG(0), 0);
  return keygen_tail(ctx, job, p, argc, argv, 1);
}

/* DhKeyPairGenJob(mode, group | prime | primeLength, [generator], ...) */
static int dh_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KeyGenParams *p = job->params;
  p->kind = KG_DH;
  if (JS_IsString(ARG(0))) {
    const char *g = JS_ToCString(ctx, ARG(0));
    snprintf(p->group, sizeof(p->group), "%s", g);
    JS_FreeCString(ctx, g);
    return keygen_tail(ctx, job, p, argc, argv, 1);
  }
  if (crypto_is_buffer_source(ctx, ARG(0))) {
    size_t l;
    uint8_t *d = crypto_buffer_source(ctx, ARG(0), &l);
    p->prime = BN_bin2bn(d, (int)l, NULL);
  } else {
    p->prime_bits = nb_int32(ctx, ARG(0), 2048);
  }
  p->generator = nb_int32(ctx, ARG(1), 2);
  return keygen_tail(ctx, job, p, argc, argv, 2);
}

static const char *modp_name(const char *group) {
  static const struct { const char *node, *ossl; } m[] = {
    { "modp1", "modp_768" }, { "modp2", "modp_1024" }, { "modp5", "modp_1536" },
    { "modp14", "modp_2048" }, { "modp15", "modp_3072" }, { "modp16", "modp_4096" },
    { "modp17", "modp_6144" }, { "modp18", "modp_8192" },
  };
  size_t i;
  for (i = 0; i < countof(m); i++)
    if (!strcmp(group, m[i].node))
      return m[i].ossl;
  return NULL;
}

static EVP_PKEY *dh_params(KeyGenParams *p) {
  EVP_PKEY *params = NULL;
  if (p->group[0]) {
    const char *name = modp_name(p->group);
    EVP_PKEY_CTX *c;
    OSSL_PARAM prm[2];
    if (!name)
      return NULL;
    c = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
    prm[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)name, 0);
    prm[1] = OSSL_PARAM_construct_end();
    if (!c || EVP_PKEY_fromdata_init(c) <= 0 ||
        EVP_PKEY_fromdata(c, &params, EVP_PKEY_KEY_PARAMETERS, prm) <= 0)
      params = NULL;
    EVP_PKEY_CTX_free(c);
    return params;
  }
  if (p->prime) {
    DH *dh = DH_new();
    BIGNUM *g = BN_new();
    BN_set_word(g, p->generator);
    if (DH_set0_pqg(dh, BN_dup(p->prime), NULL, g) != 1) {
      DH_free(dh);
      BN_free(g);
      return NULL;
    }
    params = EVP_PKEY_new();
    EVP_PKEY_assign_DH(params, dh);
    return params;
  }
  {
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_DH, NULL);
    if (!c || EVP_PKEY_paramgen_init(c) <= 0 ||
        EVP_PKEY_CTX_set_dh_paramgen_prime_len(c, p->prime_bits) <= 0 ||
        EVP_PKEY_CTX_set_dh_paramgen_generator(c, p->generator) <= 0 ||
        EVP_PKEY_paramgen(c, &params) <= 0)
      params = NULL;
    EVP_PKEY_CTX_free(c);
    return params;
  }
}

static void keygen_work(CryptoJob *job) {
  KeyGenParams *p = job->params;
  EVP_PKEY_CTX *c = NULL;
  EVP_PKEY *pkey = NULL, *params = NULL;
  int ok = 0;
  switch (p->kind) {
  case KG_RSA:
  case KG_RSA_PSS: {
    BIGNUM *e = BN_new();
    BN_set_word(e, p->exponent);
    c = EVP_PKEY_CTX_new_id(p->kind == KG_RSA_PSS ? EVP_PKEY_RSA_PSS : EVP_PKEY_RSA, NULL);
    ok = c && EVP_PKEY_keygen_init(c) > 0 &&
         EVP_PKEY_CTX_set_rsa_keygen_bits(c, (int)p->modulus_bits) > 0 &&
         EVP_PKEY_CTX_set1_rsa_keygen_pubexp(c, e) > 0;
    BN_free(e);
    if (ok && p->kind == KG_RSA_PSS) {
      if (p->md)
        ok = EVP_PKEY_CTX_set_rsa_pss_keygen_md(c, p->md) > 0;
      if (ok && p->mgf1_md)
        ok = EVP_PKEY_CTX_set_rsa_pss_keygen_mgf1_md(c, p->mgf1_md) > 0;
      if (ok && p->has_salt)
        ok = EVP_PKEY_CTX_set_rsa_pss_keygen_saltlen(c, p->salt) > 0;
    }
    break;
  }
  case KG_DSA: {
    EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new_id(EVP_PKEY_DSA, NULL);
    ok = pc && EVP_PKEY_paramgen_init(pc) > 0 &&
         EVP_PKEY_CTX_set_dsa_paramgen_bits(pc, (int)p->modulus_bits) > 0 &&
         (!p->divisor_bits ||
          EVP_PKEY_CTX_set_dsa_paramgen_q_bits(pc, (int)p->divisor_bits) > 0) &&
         EVP_PKEY_paramgen(pc, &params) > 0;
    EVP_PKEY_CTX_free(pc);
    if (ok) {
      c = EVP_PKEY_CTX_new(params, NULL);
      ok = c && EVP_PKEY_keygen_init(c) > 0;
    }
    break;
  }
  case KG_EC: {
    EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    ok = pc && EVP_PKEY_paramgen_init(pc) > 0 &&
         EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pc, p->curve_nid) > 0 &&
         EVP_PKEY_CTX_set_ec_param_enc(pc, p->param_encoding) > 0 &&
         EVP_PKEY_paramgen(pc, &params) > 0;
    EVP_PKEY_CTX_free(pc);
    if (ok) {
      c = EVP_PKEY_CTX_new(params, NULL);
      ok = c && EVP_PKEY_keygen_init(c) > 0;
    }
    break;
  }
  case KG_NID:
    c = EVP_PKEY_CTX_new_id(p->nid, NULL);
    ok = c && EVP_PKEY_keygen_init(c) > 0;
    break;
  case KG_DH:
    params = dh_params(p);
    ok = params != NULL;
    if (ok) {
      c = EVP_PKEY_CTX_new(params, NULL);
      ok = c && EVP_PKEY_keygen_init(c) > 0;
    }
    break;
  }
  if (ok)
    ok = EVP_PKEY_keygen(c, &pkey) > 0;
  EVP_PKEY_CTX_free(c);
  EVP_PKEY_free(params);
  if (!ok) {
    EVP_PKEY_free(pkey);
    if (p->kind == KG_DH && p->group[0] && !modp_name(p->group))
      crypto_job_capture_errors(job, "Unknown DH group");
    else
      crypto_job_capture_errors(job, NULL);
    return;
  }
  p->key = key_data_new_pkey(kKeyTypePrivate, pkey);
}

static JSValue encode_generated(JSContext *ctx, KeyData *k, const KeyEncoding *enc,
                                bool is_public) {
  if (enc->output_key_object) {
    KeyData *v = key_data_with_type(k, is_public ? kKeyTypePublic : kKeyTypePrivate);
    JSValue h = crypto_key_handle_new(ctx, v);
    key_data_unref(v);
    return h;
  }
  return is_public ? crypto_export_public(ctx, k, enc) : crypto_export_private(ctx, k, enc);
}

static int keygen_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  KeyGenParams *p = job->params;
  *res = JS_UNDEFINED;
  if (crypto_job_failed(job) || !p->key) {
    if (!crypto_job_failed(job))
      crypto_job_capture_errors(job, NULL);
    *err = crypto_job_error(ctx, job);
    return 0;
  }
  *err = JS_UNDEFINED;
  if (p->webcrypto) {
    KeyData *pub = key_data_with_type(p->key, kKeyTypePublic);
    JSValue pk = crypto_new_crypto_key(ctx, pub, p->algorithm, p->pub_usages, true), sk, o;
    key_data_unref(pub);
    if (JS_IsException(pk))
      goto exc;
    sk = crypto_new_crypto_key(ctx, p->key, p->algorithm, p->priv_usages, p->extractable);
    if (JS_IsException(sk)) {
      JS_FreeValue(ctx, pk);
      goto exc;
    }
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "publicKey", pk);
    JS_SetPropertyStr(ctx, o, "privateKey", sk);
    *res = o;
    return 0;
  }
  {
    JSValue pub = encode_generated(ctx, p->key, &p->pub_enc, true), priv, arr;
    if (JS_IsException(pub))
      goto exc;
    priv = encode_generated(ctx, p->key, &p->priv_enc, false);
    if (JS_IsException(priv)) {
      JS_FreeValue(ctx, pub);
      goto exc;
    }
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, pub);
    JS_SetPropertyUint32(ctx, arr, 1, priv);
    *res = arr;
  }
  return 0;
exc:
  *err = JS_GetException(ctx);
  *res = JS_UNDEFINED;
  return 0;
}

static void keygen_cleanup(CryptoJob *job) {
  KeyGenParams *p = job->params;
  key_data_unref(p->key);
  BN_free(p->prime);
  crypto_key_encoding_free(&p->pub_enc);
  crypto_key_encoding_free(&p->priv_enc);
  if (job->aw.env)
    JS_FreeValueRT(job->aw.env->rt, p->algorithm);
}

#define KG_TRAITS(var, name, config)                                  \
  static const CryptoJobTraits var = {                                \
    name, PROVIDER_KEYPAIRGENREQUEST, sizeof(KeyGenParams),           \
    config, keygen_work, keygen_result, keygen_cleanup,               \
  };
KG_TRAITS(rsa_kg_job, "RsaKeyPairGenJob", rsa_kg_config)
KG_TRAITS(dsa_kg_job, "DsaKeyPairGenJob", dsa_kg_config)
KG_TRAITS(ec_kg_job, "EcKeyPairGenJob", ec_kg_config)
KG_TRAITS(nid_kg_job, "NidKeyPairGenJob", nid_kg_config)
KG_TRAITS(dh_kg_job, "DhKeyPairGenJob", dh_kg_config)

/* SecretKeyGenJob(mode, lengthBits[, algorithm, usages, extractable]) */
typedef struct {
  uint32_t bits;
  size_t len;
  uint8_t *out;
  JSValue algorithm;
  uint32_t usages;
  bool extractable;
} SecretGenParams;

static int secret_kg_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  SecretGenParams *p = job->params;
  p->algorithm = JS_UNDEFINED;
  p->bits = nb_uint32(ctx, ARG(0), 0);
  if (job->mode == kCryptoJobWebCrypto) {
    p->len = (p->bits + 7) / 8;
    p->algorithm = JS_DupValue(ctx, ARG(1));
    p->usages = nb_uint32(ctx, ARG(2), 0);
    p->extractable = JS_ToBool(ctx, ARG(3));
  } else {
    p->len = p->bits / 8;
  }
  return 0;
}

static void secret_kg_work(CryptoJob *job) {
  SecretGenParams *p = job->params;
  p->out = malloc(p->len ? p->len : 1);
  if (p->len && RAND_bytes(p->out, (int)p->len) != 1) {
    crypto_job_capture_errors(job, NULL);
    return;
  }
  if (p->bits % 8 && p->len)
    p->out[p->len - 1] &= (uint8_t)(0xff << (8 - p->bits % 8));
}

static int secret_kg_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  SecretGenParams *p = job->params;
  KeyData *k;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  k = key_data_new_secret(p->out, p->len);
  *err = JS_UNDEFINED;
  if (job->mode == kCryptoJobWebCrypto) {
    *res = crypto_new_crypto_key(ctx, k, p->algorithm, p->usages, p->extractable);
    if (JS_IsException(*res)) {
      *err = JS_GetException(ctx);
      *res = JS_UNDEFINED;
    }
  } else {
    *res = crypto_key_handle_new(ctx, k);
  }
  key_data_unref(k);
  return 0;
}

static void secret_kg_cleanup(CryptoJob *job) {
  SecretGenParams *p = job->params;
  if (p->out) {
    OPENSSL_cleanse(p->out, p->len);
    free(p->out);
  }
  if (job->aw.env)
    JS_FreeValueRT(job->aw.env->rt, p->algorithm);
}

static const CryptoJobTraits secret_kg_job = {
  "SecretKeyGenJob", PROVIDER_KEYGENREQUEST, sizeof(SecretGenParams),
  secret_kg_config, secret_kg_work, secret_kg_result, secret_kg_cleanup,
};

/* ---------------------------------------------------------------------- */
/* ECDH */

typedef struct {
  EC_KEY *key;
  const EC_GROUP *group;
} ECDH;

static void ecdh_finalizer(JSRuntime *rt, JSValueConst val) {
  ECDH *e = JS_GetOpaque(val, ecdh_class_id);
  if (!e)
    return;
  EC_KEY_free(e->key);
  free(e);
}

static JSValue ecdh_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  const char *c = JS_ToCString(ctx, ARG(0));
  int nid = crypto_nid_from_curve(c ? c : "");
  JSValue obj;
  ECDH *e;
  JS_FreeCString(ctx, c);
  if (nid == NID_undef)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_CURVE", NULL);
  obj = nb_new_instance(ctx, nt, ecdh_class_id);
  if (JS_IsException(obj))
    return obj;
  e = calloc(1, sizeof(*e));
  e->key = EC_KEY_new_by_curve_name(nid);
  if (!e->key) {
    free(e);
    JS_FreeValue(ctx, obj);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to create key using named curve");
  }
  e->group = EC_KEY_get0_group(e->key);
  JS_SetOpaque(obj, e);
  return obj;
}

static ECDH *ecdh_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, ecdh_class_id);
}

static JSValue ecdh_generate_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  if (!e)
    return JS_EXCEPTION;
  if (EC_KEY_generate_key(e->key) != 1)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to generate key");
  return JS_UNDEFINED;
}

static EC_POINT *buffer_to_point(const EC_GROUP *g, const uint8_t *d, size_t len) {
  EC_POINT *pt = EC_POINT_new(g);
  if (!pt || EC_POINT_oct2point(g, pt, d, len, NULL) != 1) {
    EC_POINT_free(pt);
    return NULL;
  }
  return pt;
}

static JSValue ecdh_compute_secret(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  size_t len;
  uint8_t *d, *out;
  EC_POINT *pub;
  int field;
  JSValue r;
  if (!e)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  ERR_set_mark();
  pub = buffer_to_point(e->group, d ? d : (uint8_t *)"", len);
  ERR_pop_to_mark();
  if (!pub)
    return JS_NewString(ctx, "ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY");
  if (!EC_KEY_check_key(e->key)) {
    EC_POINT_free(pub);
    ERR_clear_error();
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYPAIR", NULL);
  }
  field = (EC_GROUP_get_degree(e->group) + 7) / 8;
  out = malloc(field);
  if (ECDH_compute_key(out, field, pub, e->key, NULL) <= 0) {
    free(out);
    EC_POINT_free(pub);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to compute ECDH key");
  }
  EC_POINT_free(pub);
  r = nb_new_buffer(ctx, out, field);
  OPENSSL_cleanse(out, field);
  free(out);
  return r;
}

static JSValue ecdh_get_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  const EC_POINT *pt;
  if (!e)
    return JS_EXCEPTION;
  pt = EC_KEY_get0_public_key(e->key);
  if (!pt)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to get ECDH public key");
  return crypto_ec_point_to_buffer(ctx, e->group, pt,
                                   nb_int32(ctx, ARG(0), POINT_CONVERSION_UNCOMPRESSED));
}

static JSValue ecdh_get_private_key(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  const BIGNUM *b;
  int n;
  uint8_t *buf;
  JSValue r;
  if (!e)
    return JS_EXCEPTION;
  b = EC_KEY_get0_private_key(e->key);
  if (!b)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to get ECDH private key");
  n = BN_num_bytes(b);
  buf = malloc(n ? n : 1);
  BN_bn2binpad(b, buf, n);
  r = nb_new_buffer(ctx, buf, n);
  free(buf);
  return r;
}

static JSValue ecdh_set_private_key(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  BIGNUM *priv, *order;
  EC_POINT *pub;
  EC_KEY *nk;
  bool ok;
  if (!e)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  priv = BN_bin2bn(d ? d : (uint8_t *)"", (int)len, NULL);
  order = BN_new();
  EC_GROUP_get_order(e->group, order, NULL);
  ok = priv && !BN_is_zero(priv) && BN_cmp(priv, order) < 0;
  BN_free(order);
  if (!ok) {
    BN_clear_free(priv);
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYTYPE",
                             "Private key is not valid for specified curve.");
  }
  nk = EC_KEY_dup(e->key);
  pub = EC_POINT_new(e->group);
  ok = nk && EC_KEY_set_private_key(nk, priv) == 1 && pub &&
       EC_POINT_mul(e->group, pub, priv, NULL, NULL, NULL) == 1 &&
       EC_KEY_set_public_key(nk, pub) == 1;
  BN_clear_free(priv);
  EC_POINT_free(pub);
  if (!ok) {
    EC_KEY_free(nk);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to convert BN to a private key");
  }
  EC_KEY_free(e->key);
  e->key = nk;
  e->group = EC_KEY_get0_group(nk);
  return JS_UNDEFINED;
}

static JSValue ecdh_set_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  ECDH *e = ecdh_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  EC_POINT *pt;
  if (!e)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  pt = buffer_to_point(e->group, d ? d : (uint8_t *)"", len);
  if (!pt)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to convert Buffer to EC_POINT");
  if (EC_KEY_set_public_key(e->key, pt) != 1) {
    EC_POINT_free(pt);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to set EC_POINT as the public key");
  }
  EC_POINT_free(pt);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry ecdh_proto[] = {
  JS_CFUNC_DEF("generateKeys", 0, ecdh_generate_keys),
  JS_CFUNC_DEF("computeSecret", 1, ecdh_compute_secret),
  JS_CFUNC_DEF("getPublicKey", 1, ecdh_get_public_key),
  JS_CFUNC_DEF("getPrivateKey", 0, ecdh_get_private_key),
  JS_CFUNC_DEF("setPublicKey", 1, ecdh_set_public_key),
  JS_CFUNC_DEF("setPrivateKey", 1, ecdh_set_private_key),
};

/* ECDHConvertKey(key, curve, format) */
static JSValue ecdh_convert_key(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, ARG(0), &len);
  const char *c = JS_ToCString(ctx, ARG(1));
  int nid = crypto_nid_from_curve(c ? c : "");
  EC_GROUP *g;
  EC_POINT *pt;
  JSValue r;
  JS_FreeCString(ctx, c);
  if (!len)
    return nb_new_buffer(ctx, "", 0);
  if (nid == NID_undef)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_CURVE", NULL);
  g = EC_GROUP_new_by_curve_name(nid);
  if (!g)
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to get EC_GROUP");
  pt = buffer_to_point(g, d, len);
  if (!pt) {
    EC_GROUP_free(g);
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "Failed to convert Buffer to EC_POINT");
  }
  r = crypto_ec_point_to_buffer(ctx, g, pt, nb_int32(ctx, ARG(2), POINT_CONVERSION_UNCOMPRESSED));
  EC_POINT_free(pt);
  EC_GROUP_free(g);
  return r;
}

/* ---------------------------------------------------------------------- */
/* DiffieHellman */

typedef struct {
  DH *dh;
  int verify_error;
} DHWrap;

static void dh_finalizer(JSRuntime *rt, JSValueConst val) {
  DHWrap *w = JS_GetOpaque(val, dh_class_id);
  if (!w)
    return;
  DH_free(w->dh);
  free(w);
}

static JSValue dh_finish(JSContext *ctx, JSValue obj, DHWrap *w) {
  int codes = 0;
  if (!w->dh) {
    free(w);
    JS_FreeValue(ctx, obj);
    return crypto_throw(ctx, ERR_get_error(), "Initialization failed");
  }
  if (DH_check(w->dh, &codes) != 1)
    codes = 0;
  w->verify_error = codes;
  ERR_clear_error();
  JS_SetOpaque(obj, w);
  JS_DefinePropertyValueStr(ctx, obj, "verifyError", JS_NewInt32(ctx, w->verify_error),
                            JS_PROP_CONFIGURABLE);
  return obj;
}

/* new DiffieHellman(sizeOrPrime, generator) */
static JSValue dh_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, dh_class_id);
  DHWrap *w;
  if (JS_IsException(obj))
    return obj;
  w = calloc(1, sizeof(*w));
  if (JS_IsNumber(ARG(0))) {
    int bits = nb_int32(ctx, ARG(0), 0), gen;
    if (bits < 2) {
      free(w);
      JS_FreeValue(ctx, obj);
      ERR_raise(ERR_LIB_BN, BN_R_BITS_TOO_SMALL);
      return crypto_throw(ctx, ERR_get_error(), "Invalid prime length");
    }
    if (crypto_is_buffer_source(ctx, ARG(1))) {
      size_t gl;
      uint8_t *gd = crypto_buffer_source(ctx, ARG(1), &gl);
      BIGNUM *g = BN_bin2bn(gd, (int)gl, NULL);
      gen = (int)BN_get_word(g);
      BN_free(g);
    } else {
      gen = nb_int32(ctx, ARG(1), 2);
    }
    if (gen < 2) {
      free(w);
      JS_FreeValue(ctx, obj);
      ERR_raise(ERR_LIB_DH, DH_R_BAD_GENERATOR);
      return crypto_throw(ctx, ERR_get_error(), "Invalid generator");
    }
    w->dh = DH_new();
    if (DH_generate_parameters_ex(w->dh, bits, gen, NULL) != 1) {
      DH_free(w->dh);
      w->dh = NULL;
    }
  } else {
    size_t pl, gl = 0;
    uint8_t *pd = crypto_buffer_source(ctx, ARG(0), &pl), *gd = NULL;
    BIGNUM *p, *g;
    if (crypto_is_buffer_source(ctx, ARG(1))) {
      gd = crypto_buffer_source(ctx, ARG(1), &gl);
      g = BN_bin2bn(gd, (int)gl, NULL);
    } else {
      g = BN_new();
      BN_set_word(g, nb_int32(ctx, ARG(1), 2));
    }
    if (BN_is_zero(g) || BN_is_one(g)) {
      BN_free(g);
      free(w);
      JS_FreeValue(ctx, obj);
      ERR_raise(ERR_LIB_DH, DH_R_BAD_GENERATOR);
      return crypto_throw(ctx, ERR_get_error(), "Invalid generator");
    }
    p = BN_bin2bn(pd ? pd : (uint8_t *)"", (int)pl, NULL);
    w->dh = DH_new();
    if (DH_set0_pqg(w->dh, p, NULL, g) != 1) {
      BN_free(p);
      BN_free(g);
      DH_free(w->dh);
      w->dh = NULL;
    }
  }
  return dh_finish(ctx, obj, w);
}

static JSValue dh_group_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  const char *name = JS_ToCString(ctx, ARG(0));
  BIGNUM *p = NULL, *g;
  JSValue obj;
  DHWrap *w;
  if (!name)
    return JS_EXCEPTION;
#define G(n, fn) else if (!strcmp(name, n)) p = fn(NULL);
  if (0) {}
  G("modp1", BN_get_rfc2409_prime_768)
  G("modp2", BN_get_rfc2409_prime_1024)
  G("modp5", BN_get_rfc3526_prime_1536)
  G("modp14", BN_get_rfc3526_prime_2048)
  G("modp15", BN_get_rfc3526_prime_3072)
  G("modp16", BN_get_rfc3526_prime_4096)
  G("modp17", BN_get_rfc3526_prime_6144)
  G("modp18", BN_get_rfc3526_prime_8192)
#undef G
  JS_FreeCString(ctx, name);
  if (!p)
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNKNOWN_DH_GROUP", NULL);
  obj = nb_new_instance(ctx, nt, dh_class_id);
  if (JS_IsException(obj)) {
    BN_free(p);
    return obj;
  }
  w = calloc(1, sizeof(*w));
  g = BN_new();
  BN_set_word(g, 2);
  w->dh = DH_new();
  DH_set0_pqg(w->dh, p, NULL, g);
  return dh_finish(ctx, obj, w);
}

static DHWrap *dh_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, dh_class_id);
}

static JSValue bn_buffer(JSContext *ctx, const BIGNUM *b, int pad) {
  int n = BN_num_bytes(b);
  uint8_t *buf;
  JSValue r;
  if (pad < n)
    pad = n;
  buf = malloc(pad ? pad : 1);
  BN_bn2binpad(b, buf, pad);
  r = nb_new_buffer(ctx, buf, pad);
  free(buf);
  return r;
}

static JSValue dh_generate_keys(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  DHWrap *w = dh_of(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  if (DH_generate_key(w->dh) != 1)
    return crypto_throw(ctx, ERR_get_error(), "Key generation failed");
  return bn_buffer(ctx, DH_get0_pub_key(w->dh), DH_size(w->dh));
}

static JSValue dh_compute_secret(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  DHWrap *w = dh_of(ctx, this_val);
  size_t len;
  uint8_t *d, *out;
  BIGNUM *pub;
  int n, sz;
  JSValue r;
  if (!w)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  pub = BN_bin2bn(d ? d : (uint8_t *)"", (int)len, NULL);
  sz = DH_size(w->dh);
  out = malloc(sz ? sz : 1);
  n = DH_compute_key(out, pub, w->dh);
  if (n < 0) {
    int codes = 0, checked = DH_check_pub_key(w->dh, pub, &codes);
    BN_free(pub);
    free(out);
    if (checked == 1 && codes) {
      ERR_clear_error();
      if (codes & DH_CHECK_PUBKEY_TOO_SMALL)
        return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYLEN", "Supplied key is too small");
      if (codes & DH_CHECK_PUBKEY_TOO_LARGE)
        return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYLEN", "Supplied key is too large");
    }
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYTYPE", "Invalid key");
  }
  BN_free(pub);
  /* left-pad to the prime size */
  if (n < sz) {
    memmove(out + (sz - n), out, n);
    memset(out, 0, sz - n);
  }
  r = nb_new_buffer(ctx, out, sz);
  OPENSSL_cleanse(out, sz);
  free(out);
  return r;
}

static JSValue dh_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                      int magic) {
  DHWrap *w = dh_of(ctx, this_val);
  const BIGNUM *b;
  if (!w)
    return JS_EXCEPTION;
  switch (magic) {
  case 0: b = DH_get0_p(w->dh); break;
  case 1: b = DH_get0_g(w->dh); break;
  case 2: b = DH_get0_pub_key(w->dh); break;
  default: b = DH_get0_priv_key(w->dh);
  }
  if (!b)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_STATE",
                             magic == 2 ? "No public key - did you forget to generate one?"
                                        : "No private key - did you forget to generate one?");
  return bn_buffer(ctx, b, 0);
}

static JSValue dh_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                      int magic) {
  DHWrap *w = dh_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  BIGNUM *b;
  if (!w)
    return JS_EXCEPTION;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  b = BN_bin2bn(d ? d : (uint8_t *)"", (int)len, NULL);
  if (magic == 0)
    DH_set0_key(w->dh, b, NULL);
  else
    DH_set0_key(w->dh, NULL, b);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry dh_proto[] = {
  JS_CFUNC_DEF("generateKeys", 0, dh_generate_keys),
  JS_CFUNC_DEF("computeSecret", 1, dh_compute_secret),
  JS_CFUNC_MAGIC_DEF("getPrime", 0, dh_get, 0),
  JS_CFUNC_MAGIC_DEF("getGenerator", 0, dh_get, 1),
  JS_CFUNC_MAGIC_DEF("getPublicKey", 0, dh_get, 2),
  JS_CFUNC_MAGIC_DEF("getPrivateKey", 0, dh_get, 3),
  JS_CFUNC_MAGIC_DEF("setPublicKey", 1, dh_set, 0),
  JS_CFUNC_MAGIC_DEF("setPrivateKey", 1, dh_set, 1),
};

/* DHBitsJob(mode, public key x5, private key x5[, length]) */
typedef struct {
  KeyData *pub, *priv;
  uint8_t *out;
  size_t outlen;
} DHBitsParams;

static int dh_bits_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  DHBitsParams *p = job->params;
  int offset = 0;
  p->pub = crypto_get_public_or_private_key(ctx, argv, argc, &offset);
  if (!p->pub)
    return -1;
  p->priv = crypto_get_private_key(ctx, argv, argc, &offset, true);
  if (!p->priv)
    return -1;
  return 0;
}

static void dh_bits_work(CryptoJob *job) {
  DHBitsParams *p = job->params;
  EVP_PKEY_CTX *c = EVP_PKEY_CTX_new(p->priv->pkey, NULL);
  int ok = c && EVP_PKEY_derive_init(c) > 0 && EVP_PKEY_derive_set_peer(c, p->pub->pkey) > 0 &&
           EVP_PKEY_derive(c, NULL, &p->outlen) > 0;
  if (ok) {
    p->out = malloc(p->outlen ? p->outlen : 1);
    ok = EVP_PKEY_derive(c, p->out, &p->outlen) > 0;
  }
  EVP_PKEY_CTX_free(c);
  if (!ok)
    crypto_job_capture_errors(job, NULL);
}

static int dh_bits_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  DHBitsParams *p = job->params;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  *res = crypto_new_array_buffer(ctx, p->out, p->outlen);
  return 0;
}

static void dh_bits_cleanup(CryptoJob *job) {
  DHBitsParams *p = job->params;
  key_data_unref(p->pub);
  key_data_unref(p->priv);
  if (p->out) {
    OPENSSL_cleanse(p->out, p->outlen);
    free(p->out);
  }
}

static const CryptoJobTraits dh_bits_job = {
  "DHBitsJob", PROVIDER_DERIVEBITSREQUEST, sizeof(DHBitsParams),
  dh_bits_config, dh_bits_work, dh_bits_result, dh_bits_cleanup,
};

void crypto_init_sig(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef sdef = { .name = "Sign", .class_id = &sign_class_id, .ctor = sign_ctor,
                        .finalizer = sign_finalizer, .proto_funcs = sign_proto,
                        .proto_funcs_count = countof(sign_proto), .parent_ctor = JS_UNDEFINED };
  NodeClassDef vdef = { .name = "Verify", .class_id = &verify_class_id, .ctor = verify_ctor,
                        .finalizer = sign_finalizer, .proto_funcs = verify_proto,
                        .proto_funcs_count = countof(verify_proto), .parent_ctor = JS_UNDEFINED };
  NodeClassDef edef = { .name = "ECDH", .class_id = &ecdh_class_id, .ctor = ecdh_ctor,
                        .ctor_length = 1, .finalizer = ecdh_finalizer, .proto_funcs = ecdh_proto,
                        .proto_funcs_count = countof(ecdh_proto), .parent_ctor = JS_UNDEFINED };
  NodeClassDef ddef = { .name = "DiffieHellman", .class_id = &dh_class_id, .ctor = dh_ctor,
                        .ctor_length = 2, .finalizer = dh_finalizer, .proto_funcs = dh_proto,
                        .proto_funcs_count = countof(dh_proto), .parent_ctor = JS_UNDEFINED };
  JSValue dh, grp, proto;
  JS_FreeValue(ctx, nb_define_class(ctx, target, &sdef));
  JS_FreeValue(ctx, nb_define_class(ctx, target, &vdef));
  JS_FreeValue(ctx, nb_define_class(ctx, target, &edef));
  dh = nb_define_class(ctx, target, &ddef);
  /* DiffieHellmanGroup shares the class and its methods */
  grp = JS_NewCFunction2(ctx, dh_group_ctor, "DiffieHellmanGroup", 1,
                         JS_CFUNC_constructor_or_func, 0);
  proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, dh_proto, countof(dh_proto));
  JS_SetConstructor(ctx, grp, proto);
  JS_FreeValue(ctx, proto);
  JS_DefinePropertyValueStr(ctx, target, "DiffieHellmanGroup", grp,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, dh);
  nb_set_method(ctx, target, "ECDHConvertKey", ecdh_convert_key, 3);
  crypto_define_job(ctx, target, &sign_job);
  crypto_define_job(ctx, target, &rsa_kg_job);
  crypto_define_job(ctx, target, &dsa_kg_job);
  crypto_define_job(ctx, target, &ec_kg_job);
  crypto_define_job(ctx, target, &nid_kg_job);
  crypto_define_job(ctx, target, &dh_kg_job);
  crypto_define_job(ctx, target, &secret_kg_job);
  crypto_define_job(ctx, target, &dh_bits_job);
}
