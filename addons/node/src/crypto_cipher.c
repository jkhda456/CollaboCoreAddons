/* Ciphers for internalBinding('crypto') (crypto_cipher.cc, crypto_aes.cc,
 * crypto_rsa.cc's RSACipherJob, crypto_chacha20_poly1305.cc):
 * CipherBase, publicEncrypt & co, getCipherInfo and the WebCrypto cipher
 * jobs. */
#include <openssl/rsa.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

#define kNoAuthTagLength ((unsigned)-1)
enum { kAuthTagUnknown, kAuthTagComputed, kAuthTagSetByUser };

static JSClassID cipher_class_id;

typedef struct {
  EVP_CIPHER_CTX *ctx;
  bool encrypt;
  unsigned auth_tag_len;
  int auth_tag_state;
  uint8_t auth_tag[16];
  bool pending_auth_failed;
  int max_message_size;
} CipherBase;

static const EVP_CIPHER *cipher_by_name(const char *name) {
  const EVP_CIPHER *c = EVP_get_cipherbyname(name);
  return c;
}

static int cipher_mode(const EVP_CIPHER *c) {
  return EVP_CIPHER_get_mode(c);
}

static bool is_chacha_poly(const EVP_CIPHER *c) {
  return EVP_CIPHER_get_nid(c) == NID_chacha20_poly1305;
}

/* GCM, CCM, OCB and chacha20-poly1305 (ncrypto isSupportedAuthenticatedMode) */
static bool is_auth_mode(const EVP_CIPHER *c) {
  int m = cipher_mode(c);
  return m == EVP_CIPH_GCM_MODE || m == EVP_CIPH_CCM_MODE || m == EVP_CIPH_OCB_MODE ||
         is_chacha_poly(c);
}

static bool valid_gcm_tag_len(unsigned n) {
  return n == 4 || n == 8 || (n >= 12 && n <= 16);
}

static void cipher_finalizer(JSRuntime *rt, JSValueConst val) {
  CipherBase *c = JS_GetOpaque(val, cipher_class_id);
  if (!c)
    return;
  EVP_CIPHER_CTX_free(c->ctx);
  free(c);
}

static JSValue throw_invalid_iv(JSContext *ctx) {
  return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_IV", NULL);
}

/* new CipherBase(isEncrypt, cipher, key, iv, authTagLength) */
static JSValue cipher_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, cipher_class_id);
  CipherBase *cb;
  const char *name;
  const EVP_CIPHER *c;
  uint8_t *key = NULL, *iv = NULL;
  size_t keylen = 0, ivlen = 0;
  int expected_iv;
  unsigned tag_len;
  if (JS_IsException(obj))
    return obj;
  cb = calloc(1, sizeof(*cb));
  cb->auth_tag_len = kNoAuthTagLength;
  cb->max_message_size = INT32_MAX;
  JS_SetOpaque(obj, cb);
  cb->encrypt = JS_ToBool(ctx, ARG(0));
  name = JS_ToCString(ctx, ARG(1));
  c = cipher_by_name(name ? name : "");
  if (!c) {
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, obj);
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNKNOWN_CIPHER", NULL);
  }
  key = crypto_bytes_copy(ctx, ARG(2), &keylen);
  if (!key)
    goto fail;
  if (!JS_IsNull(ARG(3)) && !JS_IsUndefined(ARG(3)))
    iv = crypto_buffer_source(ctx, ARG(3), &ivlen);
  tag_len = JS_IsNumber(ARG(4)) && nb_int32(ctx, ARG(4), -1) >= 0 ? nb_uint32(ctx, ARG(4), 0)
                                                                 : kNoAuthTagLength;
  expected_iv = EVP_CIPHER_get_iv_length(c);
  if (ivlen == 0 && expected_iv != 0) {
    throw_invalid_iv(ctx);
    goto fail;
  }
  if (!is_auth_mode(c) && ivlen > 0 && (int)ivlen != expected_iv) {
    throw_invalid_iv(ctx);
    goto fail;
  }
  if (is_chacha_poly(c) && ivlen > 12) {
    throw_invalid_iv(ctx);
    goto fail;
  }

  /* CommonInit */
  ERR_set_mark();
  cb->ctx = EVP_CIPHER_CTX_new();
  if (cipher_mode(c) == EVP_CIPH_WRAP_MODE)
    EVP_CIPHER_CTX_set_flags(cb->ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  if (EVP_CipherInit_ex(cb->ctx, c, NULL, NULL, NULL, cb->encrypt) != 1) {
    crypto_throw(ctx, ERR_peek_last_error(), "Failed to initialize cipher");
    ERR_pop_to_mark();
    goto fail;
  }
  if (is_auth_mode(c)) {
    if (EVP_CIPHER_CTX_ctrl(cb->ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)ivlen, NULL) != 1) {
      ERR_pop_to_mark();
      throw_invalid_iv(ctx);
      goto fail;
    }
    if (cipher_mode(c) == EVP_CIPH_CCM_MODE) {
      if (ivlen < 7 || ivlen > 13) {
        ERR_pop_to_mark();
        throw_invalid_iv(ctx);
        goto fail;
      }
      cb->max_message_size = ivlen == 12 ? 16777215 : ivlen == 13 ? 65535 : INT32_MAX;
    }
    if (tag_len == kNoAuthTagLength) {
      if (cipher_mode(c) == EVP_CIPH_GCM_MODE) {
        /* any valid length, decided at setAuthTag / final */
      } else if (is_chacha_poly(c)) {
        tag_len = 16;
      } else {
        ERR_pop_to_mark();
        crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_AUTH_TAG", "authTagLength required for %s",
                          name);
        goto fail;
      }
    } else if ((cipher_mode(c) == EVP_CIPH_GCM_MODE && !valid_gcm_tag_len(tag_len)) ||
               (cipher_mode(c) != EVP_CIPH_GCM_MODE &&
                EVP_CIPHER_CTX_ctrl(cb->ctx, EVP_CTRL_AEAD_SET_TAG, (int)tag_len, NULL) != 1)) {
      ERR_pop_to_mark();
      crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_AUTH_TAG", "Invalid authentication tag length: %u",
                        tag_len);
      goto fail;
    }
    cb->auth_tag_len = tag_len;
  }
  if (EVP_CIPHER_CTX_set_key_length(cb->ctx, (int)keylen) != 1) {
    ERR_pop_to_mark();
    EVP_CIPHER_CTX_free(cb->ctx);
    cb->ctx = NULL;
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYLEN", NULL);
    goto fail;
  }
  if (EVP_CipherInit_ex(cb->ctx, NULL, NULL, key, iv, cb->encrypt) != 1) {
    crypto_throw(ctx, ERR_peek_last_error(), "Failed to initialize cipher");
    ERR_pop_to_mark();
    goto fail;
  }
  ERR_pop_to_mark();
  JS_FreeCString(ctx, name);
  OPENSSL_cleanse(key, keylen);
  free(key);
  return obj;
fail:
  JS_FreeCString(ctx, name);
  if (key) {
    OPENSSL_cleanse(key, keylen);
    free(key);
  }
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static CipherBase *cipher_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, cipher_class_id);
}

static JSValue cipher_update(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  uint8_t *owned = NULL, *d, *out;
  size_t len;
  int outlen, bs, r;
  JSValue ret;
  if (!cb)
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
  if (len > INT32_MAX) {
    free(owned);
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "data is too long");
  }
  if (!cb->ctx) {
    free(owned);
    return crypto_throw(ctx, ERR_get_error(), "Trying to add data in unsupported state");
  }
  if (EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_CCM_MODE && (int)len > cb->max_message_size) {
    free(owned);
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_MESSAGELEN", NULL);
  }
  bs = EVP_CIPHER_CTX_get_block_size(cb->ctx);
  if (len + (size_t)bs > INT32_MAX) {
    free(owned);
    return crypto_throw(ctx, 0, "Trying to add data in unsupported state");
  }
  outlen = (int)len + bs;
  ERR_set_mark();
  if (cb->encrypt && EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_WRAP_MODE &&
      EVP_CipherUpdate(cb->ctx, NULL, &outlen, d, (int)len) != 1) {
    free(owned);
    ret = crypto_throw(ctx, ERR_peek_last_error(), "Trying to add data in unsupported state");
    ERR_pop_to_mark();
    return ret;
  }
  out = malloc(outlen > 0 ? outlen : 1);
  r = EVP_CipherUpdate(cb->ctx, out, &outlen, d, (int)len);
  free(owned);
  if (r != 1 && !cb->encrypt && EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_CCM_MODE) {
    cb->pending_auth_failed = true;
    r = 1;
  }
  if (r != 1) {
    free(out);
    ret = crypto_throw(ctx, ERR_peek_last_error(), "Trying to add data in unsupported state");
    ERR_pop_to_mark();
    return ret;
  }
  ERR_pop_to_mark();
  ret = nb_new_buffer(ctx, out, outlen > 0 ? outlen : 0);
  free(out);
  return ret;
}

static JSValue cipher_final(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  uint8_t out[64];
  int outlen = 0;
  bool ok, auth;
  JSValue ret;
  if (!cb)
    return JS_EXCEPTION;
  if (!cb->ctx)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_STATE", NULL);
  auth = is_auth_mode(EVP_CIPHER_CTX_get0_cipher(cb->ctx));
  ERR_set_mark();
  if (!cb->encrypt && EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_CCM_MODE) {
    ok = !cb->pending_auth_failed;
  } else {
    ok = EVP_CipherFinal_ex(cb->ctx, out, &outlen) == 1;
    if (ok && cb->encrypt && auth) {
      if (cb->auth_tag_len == kNoAuthTagLength)
        cb->auth_tag_len = 16;
      ok = EVP_CIPHER_CTX_ctrl(cb->ctx, EVP_CTRL_AEAD_GET_TAG, (int)cb->auth_tag_len,
                               cb->auth_tag) == 1;
      if (ok)
        cb->auth_tag_state = kAuthTagComputed;
    }
  }
  EVP_CIPHER_CTX_free(cb->ctx);
  cb->ctx = NULL;
  if (!ok) {
    ret = crypto_throw(ctx, ERR_peek_last_error(),
                       auth ? "Unsupported state or unable to authenticate data"
                            : "Unsupported state");
    ERR_pop_to_mark();
    return ret;
  }
  ERR_pop_to_mark();
  return nb_new_buffer(ctx, out, outlen);
}

static JSValue cipher_set_auto_padding(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  if (!cb)
    return JS_EXCEPTION;
  if (!cb->ctx)
    return JS_FALSE;
  return JS_NewBool(ctx, EVP_CIPHER_CTX_set_padding(cb->ctx, JS_ToBool(ctx, ARG(0))) == 1);
}

static JSValue cipher_get_auth_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  if (!cb)
    return JS_EXCEPTION;
  if (cb->ctx || !cb->encrypt || cb->auth_tag_len == kNoAuthTagLength ||
      cb->auth_tag_state != kAuthTagComputed)
    return JS_UNDEFINED;
  return nb_new_buffer(ctx, cb->auth_tag, cb->auth_tag_len);
}

static JSValue cipher_set_auth_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  bool valid, gcm;
  if (!cb)
    return JS_EXCEPTION;
  if (!cb->ctx || !is_auth_mode(EVP_CIPHER_CTX_get0_cipher(cb->ctx)) || cb->encrypt ||
      cb->auth_tag_state != kAuthTagUnknown)
    return JS_FALSE;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  gcm = EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_GCM_MODE;
  if (gcm)
    valid = (cb->auth_tag_len == kNoAuthTagLength || cb->auth_tag_len == len) &&
            valid_gcm_tag_len((unsigned)len);
  else
    valid = cb->auth_tag_len == len;
  if (!valid)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_AUTH_TAG",
                             "Invalid authentication tag length: %u", (unsigned)len);
  if (gcm && cb->auth_tag_len == kNoAuthTagLength && len != 16)
    node_emit_process_warning(env_get(ctx),
                              "Using AES-GCM authentication tags of less than 128 bits without "
                              "specifying the authTagLength option when initializing decryption "
                              "is deprecated.",
                              "DeprecationWarning", "DEP0182");
  cb->auth_tag_len = (unsigned)len;
  if (EVP_CIPHER_CTX_ctrl(cb->ctx, EVP_CTRL_AEAD_SET_TAG, (int)len, d) != 1)
    return JS_FALSE;
  cb->auth_tag_state = kAuthTagSetByUser;
  return JS_TRUE;
}

static JSValue cipher_set_aad(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  CipherBase *cb = cipher_of(ctx, this_val);
  int plaintext_len = nb_int32(ctx, ARG(1), -1), outlen;
  size_t len;
  uint8_t *d;
  bool ok;
  if (!cb)
    return JS_EXCEPTION;
  if (!cb->ctx || !is_auth_mode(EVP_CIPHER_CTX_get0_cipher(cb->ctx)))
    return JS_FALSE;
  d = crypto_buffer_source(ctx, ARG(0), &len);
  ERR_set_mark();
  if (EVP_CIPHER_CTX_get_mode(cb->ctx) == EVP_CIPH_CCM_MODE) {
    if (plaintext_len < 0) {
      ERR_pop_to_mark();
      return node_throw_type_error(ctx, "ERR_MISSING_ARGS",
                                   "options.plaintextLength required for CCM mode with AAD");
    }
    if (plaintext_len > cb->max_message_size) {
      ERR_pop_to_mark();
      return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_MESSAGELEN", NULL);
    }
    if (EVP_CipherUpdate(cb->ctx, NULL, &outlen, NULL, plaintext_len) != 1) {
      ERR_pop_to_mark();
      return JS_FALSE;
    }
  }
  ok = EVP_CipherUpdate(cb->ctx, NULL, &outlen, d, (int)len) == 1;
  ERR_pop_to_mark();
  return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry cipher_proto[] = {
  JS_CFUNC_DEF("update", 2, cipher_update),
  JS_CFUNC_DEF("final", 0, cipher_final),
  JS_CFUNC_DEF("setAutoPadding", 1, cipher_set_auto_padding),
  JS_CFUNC_DEF("getAuthTag", 0, cipher_get_auth_tag),
  JS_CFUNC_DEF("setAuthTag", 1, cipher_set_auth_tag),
  JS_CFUNC_DEF("setAAD", 2, cipher_set_aad),
};

/* ---------------------------------------------------------------------- */
/* publicEncrypt / privateDecrypt / privateEncrypt / publicDecrypt:
   (data, format, type, passphrase, namedCurve, buffer, padding, oaepHash,
    oaepLabel) */

enum { OP_PUBLIC_ENCRYPT, OP_PRIVATE_DECRYPT, OP_PRIVATE_ENCRYPT, OP_PUBLIC_DECRYPT };

static JSValue rsa_cipher(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int op) {
  int offset = 0;
  KeyData *k = (op == OP_PRIVATE_DECRYPT || op == OP_PRIVATE_ENCRYPT)
                   ? crypto_get_private_key(ctx, argv, argc, &offset, true)
                   : crypto_get_public_or_private_key(ctx, argv, argc, &offset);
  size_t inlen, outlen = 0;
  uint8_t *in, *out = NULL;
  int padding;
  EVP_PKEY_CTX *pctx = NULL;
  const EVP_MD *md = NULL;
  JSValue ret = JS_EXCEPTION;
  int ok = 0;
  if (!k)
    return JS_EXCEPTION;
  in = crypto_buffer_source(ctx, ARG(offset), &inlen);
  padding = nb_int32(ctx, ARG(offset + 1), RSA_PKCS1_OAEP_PADDING);
  if (JS_IsString(ARG(offset + 2))) {
    const char *h = JS_ToCString(ctx, ARG(offset + 2));
    md = crypto_get_digest(h);
    if (!md) {
      crypto_throw_code(ctx, "ERR_OSSL_EVP_INVALID_DIGEST", "Invalid digest used: %s", h);
      JS_FreeCString(ctx, h);
      key_data_unref(k);
      return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, h);
  }
  pctx = EVP_PKEY_CTX_new(k->pkey, NULL);
  if (!pctx)
    goto fail;
  switch (op) {
  case OP_PUBLIC_ENCRYPT:
  case OP_PRIVATE_ENCRYPT:
    ok = op == OP_PUBLIC_ENCRYPT ? EVP_PKEY_encrypt_init(pctx) : EVP_PKEY_sign_init(pctx);
    break;
  default:
    ok = op == OP_PRIVATE_DECRYPT ? EVP_PKEY_decrypt_init(pctx) : EVP_PKEY_verify_recover_init(pctx);
  }
  if (ok != 1 || EVP_PKEY_CTX_set_rsa_padding(pctx, padding) != 1)
    goto fail;
  if (md && EVP_PKEY_CTX_set_rsa_oaep_md(pctx, md) != 1)
    goto fail;
  if (crypto_is_buffer_source(ctx, ARG(offset + 3))) {
    size_t ll;
    uint8_t *l = crypto_buffer_source(ctx, ARG(offset + 3), &ll);
    if (ll) {
      uint8_t *copy = OPENSSL_memdup(l, ll);
      if (EVP_PKEY_CTX_set0_rsa_oaep_label(pctx, copy, (int)ll) != 1) {
        OPENSSL_free(copy);
        goto fail;
      }
    }
  }
  switch (op) {
  case OP_PUBLIC_ENCRYPT: ok = EVP_PKEY_encrypt(pctx, NULL, &outlen, in, inlen); break;
  case OP_PRIVATE_DECRYPT: ok = EVP_PKEY_decrypt(pctx, NULL, &outlen, in, inlen); break;
  case OP_PRIVATE_ENCRYPT: ok = EVP_PKEY_sign(pctx, NULL, &outlen, in, inlen); break;
  default: ok = EVP_PKEY_verify_recover(pctx, NULL, &outlen, in, inlen);
  }
  if (ok != 1)
    goto fail;
  out = malloc(outlen ? outlen : 1);
  switch (op) {
  case OP_PUBLIC_ENCRYPT: ok = EVP_PKEY_encrypt(pctx, out, &outlen, in, inlen); break;
  case OP_PRIVATE_DECRYPT: ok = EVP_PKEY_decrypt(pctx, out, &outlen, in, inlen); break;
  case OP_PRIVATE_ENCRYPT: ok = EVP_PKEY_sign(pctx, out, &outlen, in, inlen); break;
  default: ok = EVP_PKEY_verify_recover(pctx, out, &outlen, in, inlen);
  }
  if (ok != 1)
    goto fail;
  ret = nb_new_buffer(ctx, out, outlen);
  goto done;
fail:
  ret = crypto_throw(ctx, ERR_get_error(), NULL);
done:
  free(out);
  EVP_PKEY_CTX_free(pctx);
  key_data_unref(k);
  return ret;
}

static JSValue public_encrypt(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return rsa_cipher(ctx, t, argc, argv, OP_PUBLIC_ENCRYPT);
}
static JSValue private_decrypt(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return rsa_cipher(ctx, t, argc, argv, OP_PRIVATE_DECRYPT);
}
static JSValue private_encrypt(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return rsa_cipher(ctx, t, argc, argv, OP_PRIVATE_ENCRYPT);
}
static JSValue public_decrypt(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return rsa_cipher(ctx, t, argc, argv, OP_PUBLIC_DECRYPT);
}

/* ---------------------------------------------------------------------- */
/* getCipherInfo(info, nameOrNid, keyLength, ivLength) */

static const char *mode_label(const EVP_CIPHER *c) {
  switch (cipher_mode(c)) {
  case EVP_CIPH_CCM_MODE: return "ccm";
  case EVP_CIPH_CFB_MODE: return "cfb";
  case EVP_CIPH_CBC_MODE: return "cbc";
  case EVP_CIPH_CTR_MODE: return "ctr";
  case EVP_CIPH_ECB_MODE: return "ecb";
  case EVP_CIPH_GCM_MODE: return "gcm";
  case EVP_CIPH_OCB_MODE: return "ocb";
  case EVP_CIPH_OFB_MODE: return "ofb";
  case EVP_CIPH_STREAM_CIPHER: return "stream";
  case EVP_CIPH_WRAP_MODE: return "wrap";
  case EVP_CIPH_XTS_MODE: return "xts";
  }
  return "";
}

static JSValue get_cipher_info(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  JSValueConst info = ARG(0);
  const EVP_CIPHER *c;
  int ivlen, keylen, blk;
  const char *ml;
  if (JS_IsString(ARG(1))) {
    const char *n = JS_ToCString(ctx, ARG(1));
    c = cipher_by_name(n);
    JS_FreeCString(ctx, n);
  } else {
    c = EVP_get_cipherbynid(nb_int32(ctx, ARG(1), 0));
  }
  if (!c)
    return JS_UNDEFINED;
  ivlen = EVP_CIPHER_get_iv_length(c);
  keylen = EVP_CIPHER_get_key_length(c);
  blk = EVP_CIPHER_get_block_size(c);
  if (JS_IsNumber(ARG(2)) || JS_IsNumber(ARG(3))) {
    EVP_CIPHER_CTX *cc = EVP_CIPHER_CTX_new();
    bool ok = EVP_CipherInit_ex(cc, c, NULL, NULL, NULL, 1) == 1;
    ERR_set_mark();
    if (ok && JS_IsNumber(ARG(2))) {
      int l = nb_int32(ctx, ARG(2), 0);
      ok = EVP_CIPHER_CTX_set_key_length(cc, l) == 1;
      keylen = l;
    }
    if (ok && JS_IsNumber(ARG(3))) {
      int l = nb_int32(ctx, ARG(3), 0);
      int m = cipher_mode(c);
      if (m == EVP_CIPH_CCM_MODE)
        ok = l >= 7 && l <= 13;
      else if (m == EVP_CIPH_GCM_MODE)
        ok = true;
      else if (m == EVP_CIPH_OCB_MODE)
        ok = EVP_CIPHER_CTX_ctrl(cc, EVP_CTRL_AEAD_SET_IVLEN, l, NULL) == 1;
      else
        ok = l == ivlen;
      ivlen = l;
    }
    ERR_pop_to_mark();
    EVP_CIPHER_CTX_free(cc);
    if (!ok)
      return JS_UNDEFINED;
  }
  ml = mode_label(c);
  if (ml[0])
    JS_SetPropertyStr(ctx, info, "mode", JS_NewString(ctx, ml));
  JS_SetPropertyStr(ctx, info, "name", JS_NewString(ctx, OBJ_nid2sn(EVP_CIPHER_get_nid(c))));
  JS_SetPropertyStr(ctx, info, "nid", JS_NewInt32(ctx, EVP_CIPHER_get_nid(c)));
  if (cipher_mode(c) != EVP_CIPH_STREAM_CIPHER)
    JS_SetPropertyStr(ctx, info, "blockSize", JS_NewInt32(ctx, blk));
  if (ivlen != 0)
    JS_SetPropertyStr(ctx, info, "ivLength", JS_NewInt32(ctx, ivlen));
  JS_SetPropertyStr(ctx, info, "keyLength", JS_NewInt32(ctx, keylen));
  return JS_DupValue(ctx, info);
}

/* ---------------------------------------------------------------------- */
/* WebCrypto cipher jobs */

typedef struct {
  int cipher_mode; /* kWebCryptoCipherEncrypt / Decrypt */
  KeyData *key;
  uint8_t *data, *iv, *aad, *label;
  size_t datalen, ivlen, aadlen, labellen;
  int variant;
  const EVP_CIPHER *cipher;
  const EVP_MD *md;
  unsigned tag_len, ctr_length;
  uint8_t *out;
  size_t outlen;
} WCParams;

static void wc_cleanup(CryptoJob *job) {
  WCParams *p = job->params;
  key_data_unref(p->key);
  free(p->data);
  free(p->iv);
  free(p->aad);
  free(p->label);
  free(p->out);
}

static int wc_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  WCParams *p = job->params;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  *res = crypto_new_array_buffer(ctx, p->out ? p->out : (uint8_t *)"", p->outlen);
  return 0;
}

static int wc_common(JSContext *ctx, WCParams *p, int argc, JSValueConst *argv) {
  p->cipher_mode = nb_int32(ctx, ARG(0), kWebCryptoCipherEncrypt);
  p->key = crypto_key_handle_data(ARG(1));
  if (!p->key)
    return JS_ThrowTypeError(ctx, "invalid key handle"), -1;
  key_data_ref(p->key);
  p->data = crypto_bytes_copy(ctx, ARG(2), &p->datalen);
  return p->data ? 0 : -1;
}

static const char *aes_names[] = {
  "aes-128-ctr", "aes-192-ctr", "aes-256-ctr", "aes-128-cbc", "aes-192-cbc", "aes-256-cbc",
  "aes-128-gcm", "aes-192-gcm", "aes-256-gcm", "id-aes128-wrap", "id-aes192-wrap",
  "id-aes256-wrap", "aes-128-ocb", "aes-192-ocb", "aes-256-ocb",
};

/* AESCipherJob(mode, cipherMode, key, data, variant, iv|counter, length|tagLength, aad) */
static int aes_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  WCParams *p = job->params;
  int m;
  if (wc_common(ctx, p, argc, argv) < 0)
    return -1;
  p->variant = nb_int32(ctx, ARG(3), 0);
  if (p->variant < 0 || p->variant >= (int)countof(aes_names) ||
      !(p->cipher = EVP_get_cipherbyname(aes_names[p->variant])))
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNKNOWN_CIPHER", NULL), -1;
  m = cipher_mode(p->cipher);
  if (m == EVP_CIPH_WRAP_MODE) {
    static const uint8_t default_iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
    p->iv = malloc(8);
    memcpy(p->iv, default_iv, 8);
    p->ivlen = 8;
    return 0;
  }
  if (!(p->iv = crypto_bytes_copy(ctx, ARG(4), &p->ivlen)))
    return -1;
  if (m == EVP_CIPH_CTR_MODE) {
    p->ctr_length = nb_uint32(ctx, ARG(5), 0);
    if (p->ctr_length == 0 || p->ctr_length > 128)
      return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_COUNTER", NULL), -1;
  } else if (m == EVP_CIPH_GCM_MODE || m == EVP_CIPH_OCB_MODE) {
    p->tag_len = nb_uint32(ctx, ARG(5), 16);
    if (!JS_IsUndefined(ARG(6)) && !(p->aad = crypto_bytes_copy(ctx, ARG(6), &p->aadlen)))
      return -1;
  }
  if (m == EVP_CIPH_OCB_MODE) {
    if (p->ivlen == 0 || p->ivlen > 15)
      return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_IV", NULL), -1;
  } else if ((int)p->ivlen < EVP_CIPHER_get_iv_length(p->cipher)) {
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_IV", NULL), -1;
  }
  return 0;
}

static bool evp_run(const EVP_CIPHER *c, bool enc, const uint8_t *key, const uint8_t *iv,
                    size_t ivlen, const uint8_t *aad, size_t aadlen, const uint8_t *in,
                    size_t inlen, uint8_t *out, size_t *outlen, int tag_len, const uint8_t *tag,
                    uint8_t *tag_out, bool padding) {
  EVP_CIPHER_CTX *cc = EVP_CIPHER_CTX_new();
  int l = 0, f = 0;
  bool aead = is_auth_mode(c);
  bool ok = EVP_CipherInit_ex(cc, c, NULL, NULL, NULL, enc) == 1;
  if (ok && cipher_mode(c) == EVP_CIPH_WRAP_MODE)
    EVP_CIPHER_CTX_set_flags(cc, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  if (ok && aead)
    ok = EVP_CIPHER_CTX_ctrl(cc, EVP_CTRL_AEAD_SET_IVLEN, (int)ivlen, NULL) == 1;
  if (ok && aead && cipher_mode(c) == EVP_CIPH_OCB_MODE)
    ok = EVP_CIPHER_CTX_ctrl(cc, EVP_CTRL_AEAD_SET_TAG, tag_len, enc ? NULL : (void *)tag) == 1;
  if (ok)
    ok = EVP_CipherInit_ex(cc, NULL, NULL, key, iv, enc) == 1;
  if (ok && !padding)
    EVP_CIPHER_CTX_set_padding(cc, 0);
  if (ok && aead && !enc && cipher_mode(c) != EVP_CIPH_OCB_MODE)
    ok = EVP_CIPHER_CTX_ctrl(cc, EVP_CTRL_AEAD_SET_TAG, tag_len, (void *)tag) == 1;
  if (ok && aad && aadlen)
    ok = EVP_CipherUpdate(cc, NULL, &l, aad, (int)aadlen) == 1;
  if (ok)
    ok = EVP_CipherUpdate(cc, out, &l, in, (int)inlen) == 1;
  if (ok)
    ok = EVP_CipherFinal_ex(cc, out + l, &f) == 1;
  if (ok && aead && enc)
    ok = EVP_CIPHER_CTX_ctrl(cc, EVP_CTRL_AEAD_GET_TAG, tag_len, tag_out) == 1;
  *outlen = (size_t)l + f;
  EVP_CIPHER_CTX_free(cc);
  return ok;
}

static void aes_work(CryptoJob *job) {
  WCParams *p = job->params;
  bool enc = p->cipher_mode == kWebCryptoCipherEncrypt;
  int m = cipher_mode(p->cipher);
  const uint8_t *key = p->key->secret;
  if (p->key->secret_len != (size_t)EVP_CIPHER_get_key_length(p->cipher)) {
    crypto_job_capture_errors(job, "Invalid key length");
    return;
  }
  if (m == EVP_CIPH_GCM_MODE || m == EVP_CIPH_OCB_MODE) {
    size_t n;
    if (enc) {
      p->out = malloc(p->datalen + p->tag_len + 16);
      if (!evp_run(p->cipher, true, key, p->iv, p->ivlen, p->aad, p->aadlen, p->data,
                   p->datalen, p->out, &n, (int)p->tag_len, NULL, NULL, true)) {
        crypto_job_capture_errors(job, "Cipher job failed");
        return;
      }
      /* the tag goes after the ciphertext */
      {
        EVP_CIPHER_CTX *dummy = NULL;
        (void)dummy;
      }
      p->outlen = n;
      /* rerun to get the tag is wasteful: evp_run wrote it into tag_out */
      return;
    }
    if (p->datalen < p->tag_len) {
      crypto_job_capture_errors(job, "The provided data is too small.");
      return;
    }
    p->out = malloc(p->datalen + 16);
    if (!evp_run(p->cipher, false, key, p->iv, p->ivlen, p->aad, p->aadlen, p->data,
                 p->datalen - p->tag_len, p->out, &n, (int)p->tag_len,
                 p->data + p->datalen - p->tag_len, NULL, true)) {
      crypto_job_capture_errors(job, "Cipher job failed");
      return;
    }
    p->outlen = n;
    return;
  }
  if (m == EVP_CIPH_CTR_MODE) {
    /* counter blocks wrap at 2^length (the low bits of the counter block) */
    size_t nblocks = (p->datalen + 15) / 16, done = 0;
    uint8_t ctr[16];
    p->out = malloc(p->datalen ? p->datalen : 1);
    memcpy(ctr, p->iv, 16);
    while (done < p->datalen) {
      /* blocks left before the counter part wraps */
      uint64_t left = UINT64_MAX;
      size_t chunk, n;
      if (p->ctr_length < 64) {
        uint64_t low = 0, mask = (p->ctr_length == 64) ? UINT64_MAX : ((1ULL << p->ctr_length) - 1);
        int i;
        for (i = 8; i < 16; i++)
          low = (low << 8) | ctr[i];
        left = mask - (low & mask) + 1;
      }
      chunk = p->datalen - done;
      if (left != UINT64_MAX && chunk > left * 16)
        chunk = (size_t)left * 16;
      if (!evp_run(p->cipher, enc, key, ctr, 16, NULL, 0, p->data + done, chunk, p->out + done,
                   &n, 0, NULL, NULL, true)) {
        crypto_job_capture_errors(job, "Cipher job failed");
        return;
      }
      done += chunk;
      if (done < p->datalen) {
        /* wrapped: zero the counter bits */
        uint64_t low = 0, mask = (1ULL << p->ctr_length) - 1;
        int i;
        for (i = 8; i < 16; i++)
          low = (low << 8) | ctr[i];
        low &= ~mask;
        for (i = 15; i >= 8; i--) {
          ctr[i] = (uint8_t)low;
          low >>= 8;
        }
      }
    }
    (void)nblocks;
    p->outlen = p->datalen;
    return;
  }
  {
    size_t n;
    p->out = malloc(p->datalen + 32);
    if (!evp_run(p->cipher, enc, key, p->iv, p->ivlen, NULL, 0, p->data, p->datalen, p->out, &n,
                 0, NULL, NULL, true)) {
      crypto_job_capture_errors(job, "Cipher job failed");
      return;
    }
    p->outlen = n;
  }
}

/* AES-GCM/OCB encryption appends the tag: done here instead of in evp_run */
static void aead_encrypt_work(CryptoJob *job) {
  WCParams *p = job->params;
  size_t n;
  uint8_t tag[16];
  p->out = malloc(p->datalen + p->tag_len + 16);
  if (!evp_run(p->cipher, true, p->key->secret, p->iv, p->ivlen, p->aad, p->aadlen, p->data,
               p->datalen, p->out, &n, (int)p->tag_len, NULL, tag, true)) {
    crypto_job_capture_errors(job, "Cipher job failed");
    return;
  }
  memcpy(p->out + n, tag, p->tag_len);
  p->outlen = n + p->tag_len;
}

static void aes_job_work(CryptoJob *job) {
  WCParams *p = job->params;
  int m = cipher_mode(p->cipher);
  if ((m == EVP_CIPH_GCM_MODE || m == EVP_CIPH_OCB_MODE) &&
      p->cipher_mode == kWebCryptoCipherEncrypt) {
    if (p->key->secret_len != (size_t)EVP_CIPHER_get_key_length(p->cipher)) {
      crypto_job_capture_errors(job, "Invalid key length");
      return;
    }
    aead_encrypt_work(job);
    return;
  }
  aes_work(job);
}

static const CryptoJobTraits aes_job = {
  "AESCipherJob", PROVIDER_CIPHERREQUEST, sizeof(WCParams),
  aes_config, aes_job_work, wc_result, wc_cleanup,
};

/* ChaCha20Poly1305CipherJob(mode, cipherMode, key, data, iv, aad) */
static int c20p_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  WCParams *p = job->params;
  if (wc_common(ctx, p, argc, argv) < 0)
    return -1;
  p->cipher = EVP_get_cipherbyname("chacha20-poly1305");
  if (!p->cipher)
    return crypto_throw_code(ctx, "ERR_CRYPTO_UNKNOWN_CIPHER", NULL), -1;
  if (!(p->iv = crypto_bytes_copy(ctx, ARG(3), &p->ivlen)))
    return -1;
  if (p->ivlen != 12)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_IV", NULL), -1;
  if (!JS_IsUndefined(ARG(4)) && !(p->aad = crypto_bytes_copy(ctx, ARG(4), &p->aadlen)))
    return -1;
  p->tag_len = 16;
  return 0;
}

static void c20p_work(CryptoJob *job) {
  WCParams *p = job->params;
  size_t n;
  if (p->cipher_mode == kWebCryptoCipherEncrypt) {
    aead_encrypt_work(job);
    return;
  }
  if (p->datalen < 16) {
    crypto_job_capture_errors(job, "The provided data is too small.");
    return;
  }
  p->out = malloc(p->datalen);
  if (!evp_run(p->cipher, false, p->key->secret, p->iv, p->ivlen, p->aad, p->aadlen, p->data,
               p->datalen - 16, p->out, &n, 16, p->data + p->datalen - 16, NULL, true)) {
    crypto_job_capture_errors(job, "Cipher job failed");
    return;
  }
  p->outlen = n;
}

static const CryptoJobTraits c20p_job = {
  "ChaCha20Poly1305CipherJob", PROVIDER_CIPHERREQUEST, sizeof(WCParams),
  c20p_config, c20p_work, wc_result, wc_cleanup,
};

/* RSACipherJob(mode, cipherMode, key, data, variant, hash, label) */
static int rsa_job_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  WCParams *p = job->params;
  const char *h;
  if (wc_common(ctx, p, argc, argv) < 0)
    return -1;
  p->variant = nb_int32(ctx, ARG(3), 2);
  h = JS_ToCString(ctx, ARG(4));
  p->md = crypto_get_digest(h);
  JS_FreeCString(ctx, h);
  if (!p->md)
    return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", NULL), -1;
  if (crypto_is_buffer_source(ctx, ARG(5)) && !(p->label = crypto_bytes_copy(ctx, ARG(5), &p->labellen)))
    return -1;
  return 0;
}

static void rsa_job_work(CryptoJob *job) {
  WCParams *p = job->params;
  EVP_PKEY_CTX *pc = EVP_PKEY_CTX_new(p->key->pkey, NULL);
  bool enc = p->cipher_mode == kWebCryptoCipherEncrypt;
  int ok = pc && (enc ? EVP_PKEY_encrypt_init(pc) : EVP_PKEY_decrypt_init(pc)) == 1 &&
           EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_OAEP_PADDING) == 1 &&
           EVP_PKEY_CTX_set_rsa_oaep_md(pc, p->md) == 1;
  if (ok && p->labellen) {
    uint8_t *copy = OPENSSL_memdup(p->label, p->labellen);
    ok = EVP_PKEY_CTX_set0_rsa_oaep_label(pc, copy, (int)p->labellen) == 1;
    if (!ok)
      OPENSSL_free(copy);
  }
  if (ok)
    ok = (enc ? EVP_PKEY_encrypt(pc, NULL, &p->outlen, p->data, p->datalen)
              : EVP_PKEY_decrypt(pc, NULL, &p->outlen, p->data, p->datalen)) == 1;
  if (ok) {
    p->out = malloc(p->outlen ? p->outlen : 1);
    ok = (enc ? EVP_PKEY_encrypt(pc, p->out, &p->outlen, p->data, p->datalen)
              : EVP_PKEY_decrypt(pc, p->out, &p->outlen, p->data, p->datalen)) == 1;
  }
  if (!ok)
    crypto_job_capture_errors(job, "Cipher job failed");
  EVP_PKEY_CTX_free(pc);
}

static const CryptoJobTraits rsa_cipher_job = {
  "RSACipherJob", PROVIDER_CIPHERREQUEST, sizeof(WCParams),
  rsa_job_config, rsa_job_work, wc_result, wc_cleanup,
};

void crypto_init_cipher(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef def = { .name = "CipherBase", .class_id = &cipher_class_id, .ctor = cipher_ctor,
                       .ctor_length = 5, .finalizer = cipher_finalizer,
                       .proto_funcs = cipher_proto, .proto_funcs_count = countof(cipher_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, target, &def));
  nb_set_method(ctx, target, "publicEncrypt", public_encrypt, 9);
  nb_set_method(ctx, target, "privateDecrypt", private_decrypt, 9);
  nb_set_method(ctx, target, "privateEncrypt", private_encrypt, 9);
  nb_set_method(ctx, target, "publicDecrypt", public_decrypt, 9);
  nb_set_method(ctx, target, "getCipherInfo", get_cipher_info, 4);
  crypto_define_job(ctx, target, &aes_job);
  crypto_define_job(ctx, target, &c20p_job);
  crypto_define_job(ctx, target, &rsa_cipher_job);
}
