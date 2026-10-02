/* internalBinding('crypto'), the core: OpenSSL errors, the CryptoJob
 * machinery (sync / threadpool / WebCrypto promise), hashes, HMAC, random,
 * PBKDF2 / HKDF / scrypt and the small helpers (crypto_util.cc,
 * crypto_hash.cc, crypto_hmac.cc, crypto_random.cc, ...).  Keys, ciphers,
 * signatures, X.509 and TLS live in the other crypto_*.c files. */
#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

/* ---------------------------------------------------------------------- */
/* errors (crypto_util.cc ThrowCryptoError / Decorate) */

static void decorate(JSContext *ctx, JSValueConst obj, unsigned long err) {
  const char *ls, *rs, *lib = "", *prefix = "OSSL_";
  char reason[128], code[192];
  size_t i;
  if (err == 0)
    return;
  ls = ERR_lib_error_string(err);
  rs = ERR_reason_error_string(err);
  if (ls)
    JS_SetPropertyStr(ctx, obj, "library", JS_NewString(ctx, ls));
  if (!rs)
    return;
  JS_SetPropertyStr(ctx, obj, "reason", JS_NewString(ctx, rs));
  snprintf(reason, sizeof(reason), "%s", rs);
  for (i = 0; reason[i]; i++)
    reason[i] = reason[i] == ' ' ? '_' : (char)toupper((unsigned char)reason[i]);
  switch (ERR_GET_LIB(err)) {
#define V(name) case ERR_LIB_##name: lib = #name "_"; break;
  V(SYS) V(BN) V(RSA) V(DH) V(EVP) V(BUF) V(OBJ) V(PEM) V(DSA) V(X509) V(ASN1)
  V(CONF) V(CRYPTO) V(EC) V(SSL) V(BIO) V(PKCS7) V(X509V3) V(RAND) V(ENGINE)
  V(OCSP) V(UI) V(COMP) V(ECDSA) V(ECDH) V(CMS) V(HMAC) V(USER) V(PKCS12) V(DSO)
  V(OSSL_STORE) V(FIPS) V(TS) V(CT) V(ASYNC) V(KDF) V(SM2)
#undef V
  }
  if (!strcmp(lib, "SSL_"))
    prefix = "";
  snprintf(code, sizeof(code), "ERR_%s%s%s", prefix, lib, reason);
  JS_SetPropertyStr(ctx, obj, "code", JS_NewString(ctx, code));
}

/* the rest of the queue (oldest first) as opensslErrorStack */
static void attach_error_stack(JSContext *ctx, JSValueConst obj, unsigned long *errs, int n) {
  JSValue arr;
  int i;
  if (n <= 0)
    return;
  arr = JS_NewArray(ctx);
  for (i = 0; i < n; i++) {
    char buf[256];
    ERR_error_string_n(errs[i], buf, sizeof(buf));
    JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, buf));
  }
  JS_SetPropertyStr(ctx, obj, "opensslErrorStack", arr);
}

JSValue crypto_error(JSContext *ctx, unsigned long err, const char *msg) {
  char buf[256];
  unsigned long errs[16];
  int n = 0;
  unsigned long e;
  JSValue obj;
  if (err != 0 || msg == NULL) {
    ERR_error_string_n(err, buf, sizeof(buf));
    msg = buf;
  }
  /* CryptoErrorStore::Capture: the queue, reversed */
  while ((e = ERR_get_error()) != 0)
    if (n < 16)
      errs[n++] = e;
  {
    int i;
    for (i = 0; i < n / 2; i++) {
      unsigned long t = errs[i];
      errs[i] = errs[n - 1 - i];
      errs[n - 1 - i] = t;
    }
  }
  obj = JS_NewError(ctx);
  JS_DefinePropertyValueStr(ctx, obj, "message", JS_NewString(ctx, msg),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  attach_error_stack(ctx, obj, errs, n);
  decorate(ctx, obj, err);
  return obj;
}

JSValue crypto_throw(JSContext *ctx, unsigned long err, const char *msg) {
  return JS_Throw(ctx, crypto_error(ctx, err, msg));
}

/* Node's error classes and default messages (node_errors.h) */
static const struct { const char *code; char cls; const char *msg; } node_crypto_errors[] = {
  { "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", 'E',
    "The selected key encoding is incompatible with the key type" },
  { "ERR_CRYPTO_INITIALIZATION_FAILED", 'E', "Initialization failed" },
  { "ERR_CRYPTO_INVALID_AUTH_TAG", 'T', "Invalid authentication tag" },
  { "ERR_CRYPTO_INVALID_COUNTER", 'T', "Invalid counter" },
  { "ERR_CRYPTO_INVALID_CURVE", 'T', "Invalid EC curve name" },
  { "ERR_CRYPTO_INVALID_DIGEST", 'T', "Invalid digest" },
  { "ERR_CRYPTO_INVALID_IV", 'T', "Invalid initialization vector" },
  { "ERR_CRYPTO_INVALID_JWK", 'T', "Invalid JWK format" },
  { "ERR_CRYPTO_INVALID_KEYLEN", 'R', "Invalid key length" },
  { "ERR_CRYPTO_INVALID_KEYPAIR", 'R', "Invalid key pair" },
  { "ERR_CRYPTO_INVALID_KEYTYPE", 'R', "Invalid key type" },
  { "ERR_CRYPTO_INVALID_MESSAGELEN", 'R', "Invalid message length" },
  { "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", 'R', "Invalid scrypt params" },
  { "ERR_CRYPTO_INVALID_STATE", 'E', "Invalid state" },
  { "ERR_CRYPTO_INVALID_TAG_LENGTH", 'R', "Invalid taglength" },
  { "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE", 'E', "Unsupported JWK EC curve" },
  { "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE", 'E', "Unsupported JWK Key Type." },
  { "ERR_CRYPTO_OPERATION_FAILED", 'E', "Operation failed" },
  { "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH", 'R', "Input buffers must have the same byte length" },
  { "ERR_CRYPTO_UNKNOWN_CIPHER", 'E', "Unknown cipher" },
  { "ERR_CRYPTO_UNKNOWN_DH_GROUP", 'E', "Unknown DH group" },
  { "ERR_CRYPTO_UNSUPPORTED_OPERATION", 'E', "Unsupported crypto operation" },
  { "ERR_CRYPTO_JOB_INIT_FAILED", 'E', "Failed to initialize crypto job config" },
  { "ERR_MISSING_PASSPHRASE", 'T', "Passphrase required for encrypted key" },
  { "ERR_INVALID_ARG_VALUE", 'T', "Invalid argument value" },
  { "ERR_INVALID_ARG_TYPE", 'T', "Invalid argument type" },
  { "ERR_OUT_OF_RANGE", 'R', "Out of range" },
  { "ERR_INVALID_THIS", 'T', "Value of \"this\" is the wrong type" },
  { "ERR_ILLEGAL_CONSTRUCTOR", 'T', "Illegal constructor" },
};

JSValue crypto_error_code(JSContext *ctx, const char *code, const char *msg) {
  char cls = 'E';
  size_t i;
  JSValue e;
  for (i = 0; i < countof(node_crypto_errors); i++)
    if (!strcmp(node_crypto_errors[i].code, code)) {
      cls = node_crypto_errors[i].cls;
      if (!msg)
        msg = node_crypto_errors[i].msg;
      break;
    }
  if (!msg)
    msg = code;
  if (cls == 'T')
    JS_ThrowTypeError(ctx, "%s", msg);
  else if (cls == 'R')
    JS_ThrowRangeError(ctx, "%s", msg);
  else
    JS_ThrowPlainError(ctx, "%s", msg);
  e = JS_GetException(ctx);
  JS_DefinePropertyValueStr(ctx, e, "code", JS_NewString(ctx, code),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  return e;
}

/* fmt NULL: the default message for the code */
JSValue crypto_throw_code(JSContext *ctx, const char *code, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  if (fmt) {
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  ERR_clear_error();
  return JS_Throw(ctx, crypto_error_code(ctx, code, fmt ? buf : NULL));
}

/* ---------------------------------------------------------------------- */
/* byte helpers */

bool crypto_is_buffer_source(JSContext *ctx, JSValueConst v) {
  size_t len;
  return nb_buffer_data(ctx, v, &len) != NULL;
}

/* any ArrayBuffer, SharedArrayBuffer or view; NULL otherwise */
uint8_t *crypto_buffer_source(JSContext *ctx, JSValueConst v, size_t *len) {
  return nb_buffer_data(ctx, v, len);
}

uint8_t *crypto_bytes_copy(JSContext *ctx, JSValueConst v, size_t *len) {
  uint8_t *out;
  if (JS_IsString(v)) {
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
      return NULL;
    out = malloc(n + 1);
    memcpy(out, s, n);
    out[n] = 0;
    JS_FreeCString(ctx, s);
    *len = n;
    return out;
  }
  {
    KeyData *k = crypto_key_handle_data(v);
    if (k && k->type == kKeyTypeSecret) {
      out = malloc(k->secret_len + 1);
      memcpy(out, k->secret, k->secret_len);
      *len = k->secret_len;
      return out;
    }
  }
  {
    size_t n;
    uint8_t *d = crypto_buffer_source(ctx, v, &n);
    if (!d) {
      JS_ThrowTypeError(ctx, "argument must be a string or a buffer");
      return NULL;
    }
    out = malloc(n + 1);
    memcpy(out, d, n);
    *len = n;
    return out;
  }
}

JSValue crypto_new_array_buffer(JSContext *ctx, const void *data, size_t len) {
  return JS_NewArrayBufferCopy(ctx, data, len);
}

/* data with an encoding (Decode<T> in crypto_util.h): pointer and maybe
   an owned copy to free */
static uint8_t *decode_data(JSContext *ctx, JSValueConst data, JSValueConst enc_v, size_t *len,
                            uint8_t **owned) {
  *owned = NULL;
  if (JS_IsString(data)) {
    int enc = node_parse_encoding(ctx, enc_v, ENC_UTF8);
    size_t n = node_string_bytes_size(ctx, data, enc);
    uint8_t *buf = malloc(n ? n : 1);
    *len = node_string_write(ctx, buf, n, data, enc, NULL);
    *owned = buf;
    return buf;
  }
  return crypto_buffer_source(ctx, data, len);
}

/* digests by name, OpenSSL 3 names and aliases; fetched ones are kept */
const EVP_MD *crypto_get_digest(const char *name) {
  static struct { char name[64]; EVP_MD *md; } cache[64];
  static int ncache;
  const EVP_MD *md;
  EVP_MD *f;
  int i;
  if (!name)
    return NULL;
  md = EVP_get_digestbyname(name);
  if (md)
    return md;
  ERR_set_mark();
  f = EVP_MD_fetch(NULL, name, NULL);
  ERR_pop_to_mark();
  if (!f)
    return NULL;
  for (i = 0; i < ncache; i++)
    if (!strcmp(cache[i].name, name)) {
      EVP_MD_free(f);
      return cache[i].md;
    }
  if (ncache < 64) {
    snprintf(cache[ncache].name, sizeof(cache[ncache].name), "%s", name);
    cache[ncache].md = f;
    ncache++;
  }
  return f;
}

int crypto_nid_from_curve(const char *name) {
  int nid = EC_curve_nist2nid(name);
  if (nid == NID_undef)
    nid = OBJ_sn2nid(name);
  if (nid == NID_undef)
    nid = OBJ_ln2nid(name);
  return nid;
}

/* ---------------------------------------------------------------------- */
/* jobs (CryptoJob in crypto_util.h) */

static JSClassID crypto_job_class_id;

void crypto_job_capture_errors(CryptoJob *job, const char *message) {
  unsigned long e;
  job->nerrors = 0;
  while ((e = ERR_get_error()) != 0)
    if (job->nerrors < 8)
      job->errors[job->nerrors++] = e;
  if (message)
    snprintf(job->message, sizeof(job->message), "%s", message);
  if (!job->nerrors && !job->message[0])
    snprintf(job->message, sizeof(job->message), "Ok");
}

bool crypto_job_failed(CryptoJob *job) {
  return job->nerrors > 0 || job->message[0];
}

int crypto_job_mode(CryptoJob *job) {
  return job->mode;
}

JSValue crypto_job_error(JSContext *ctx, CryptoJob *job) {
  /* CryptoErrorStore::ToException: the last error is the message, the
     others the stack */
  JSValue e;
  unsigned long errs[8];
  int n = 0, i;
  char buf[256];
  const char *msg;
  if (job->message_code && job->message[0])
    return crypto_error_code(ctx, job->message_code, job->message);
  /* stored oldest-first; Node reverses and pops the back (the oldest) */
  for (i = job->nerrors - 1; i >= 0; i--)
    errs[n++] = job->errors[i];
  if (job->message[0] && strcmp(job->message, "Ok")) {
    msg = job->message;
  } else if (n > 0) {
    ERR_error_string_n(errs[n - 1], buf, sizeof(buf));
    msg = buf;
    n--;
  } else {
    msg = "Ok";
  }
  e = JS_NewError(ctx);
  JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  attach_error_stack(ctx, e, errs, n);
  return e;
}

static void job_free(CryptoJob *job) {
  if (job->traits->cleanup)
    job->traits->cleanup(job);
  free(job->params);
  free(job);
}

static void job_finalizer(JSRuntime *rt, JSValueConst val) {
  CryptoJob *job = JS_GetOpaque(val, crypto_job_class_id);
  if (!job)
    return;
  JS_FreeValueRT(rt, job->resolving_funcs[0]);
  JS_FreeValueRT(rt, job->resolving_funcs[1]);
  async_wrap_destroy(&job->aw);
  job_free(job);
}

static void job_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  CryptoJob *job = JS_GetOpaque(val, crypto_job_class_id);
  if (job) {
    JS_MarkValue(rt, job->resolving_funcs[0], mark);
    JS_MarkValue(rt, job->resolving_funcs[1], mark);
  }
}

static JSValue job_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv,
                        int magic, JSValueConst *func_data) {
  const CryptoJobTraits *traits;
  CryptoJob *job;
  JSValue obj;
  int64_t p;
  JS_ToInt64(ctx, &p, func_data[0]);
  traits = (const CryptoJobTraits *)(uintptr_t)p;
  obj = nb_new_instance(ctx, nt, crypto_job_class_id);
  if (JS_IsException(obj))
    return obj;
  job = calloc(1, sizeof(*job));
  job->traits = traits;
  job->mode = nb_int32(ctx, ARG(0), kCryptoJobSync);
  job->resolving_funcs[0] = job->resolving_funcs[1] = JS_UNDEFINED;
  job->params = calloc(1, traits->params_size ? traits->params_size : 1);
  async_wrap_init(&job->aw, env_get(ctx), obj, traits->provider, -1);
  JS_SetOpaque(obj, job);
  if (traits->config(ctx, job, argc - 1, argv + 1) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

static void job_work_cb(uv_work_t *w) {
  CryptoJob *job = w->data;
  job->traits->work(job);
}

/* DOMException OperationError with the cause */
static JSValue webcrypto_error(JSContext *ctx, Env *env, JSValueConst cause) {
  JSValue ctor = JS_GetPropertyStr(ctx, env->per_context_exports, "DOMException");
  JSValue opts, args[2], e;
  if (!JS_IsFunction(ctx, ctor)) {
    JS_FreeValue(ctx, ctor);
    return JS_DupValue(ctx, cause);
  }
  opts = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, opts, "name", JS_NewString(ctx, "OperationError"));
  JS_SetPropertyStr(ctx, opts, "cause", JS_DupValue(ctx, cause));
  args[0] = JS_NewString(ctx, "The operation failed for an operation-specific reason");
  args[1] = opts;
  e = JS_CallConstructor(ctx, ctor, 2, (JSValueConst *)args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, ctor);
  if (JS_IsException(e))
    return JS_GetException(ctx);
  return e;
}

/* ToWebCryptoJobResult: Buffers become their ArrayBuffer */
static JSValue webcrypto_result(JSContext *ctx, JSValue v) {
  if (nb_is_array_buffer_view(ctx, v)) {
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    JS_FreeValue(ctx, v);
    return ab;
  }
  return v;
}

static void settle(JSContext *ctx, JSValueConst fn, JSValueConst v) {
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &v);
  JS_FreeValue(ctx, r);
}

static void job_after_work_cb(uv_work_t *w, int status) {
  CryptoJob *job = w->data;
  Env *env = job->aw.env;
  JSContext *ctx = env->ctx;
  JSValue obj = JS_DupValue(ctx, job->aw.object), err = JS_UNDEFINED, res = JS_UNDEFINED;
  job->scheduled = false;
  if (job->mode == kCryptoJobWebCrypto) {
    env_internal_callback_scope_enter(env);
    if (status == UV_ECANCELED) {
      JSValue e = crypto_error_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", "The operation was canceled");
      JSValue d = webcrypto_error(ctx, env, e);
      settle(ctx, job->resolving_funcs[1], d);
      JS_FreeValue(ctx, d);
      JS_FreeValue(ctx, e);
    } else if (job->traits->result(ctx, job, &err, &res) < 0) {
      JSValue e = JS_GetException(ctx);
      JSValue d = webcrypto_error(ctx, env, e);
      settle(ctx, job->resolving_funcs[1], d);
      JS_FreeValue(ctx, d);
      JS_FreeValue(ctx, e);
    } else if (!JS_IsUndefined(err)) {
      JSValue d = webcrypto_error(ctx, env, err);
      settle(ctx, job->resolving_funcs[1], d);
      JS_FreeValue(ctx, d);
    } else {
      res = webcrypto_result(ctx, res);
      /* shadow an inherited `then` while resolving (ResolveWebCrypto) */
      if (JS_IsObject(res)) {
        JSAtom then = JS_NewAtom(ctx, "then");
        int own = JS_GetOwnProperty(ctx, NULL, res, then);
        if (own == 0) {
          JS_DefinePropertyValue(ctx, res, then, JS_UNDEFINED, JS_PROP_CONFIGURABLE);
          settle(ctx, job->resolving_funcs[0], res);
          JS_DeleteProperty(ctx, res, then, 0);
        } else {
          settle(ctx, job->resolving_funcs[0], res);
        }
        JS_FreeAtom(ctx, then);
      } else {
        settle(ctx, job->resolving_funcs[0], res);
      }
    }
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, job->resolving_funcs[0]);
    JS_FreeValue(ctx, job->resolving_funcs[1]);
    job->resolving_funcs[0] = job->resolving_funcs[1] = JS_UNDEFINED;
    env_internal_callback_scope_exit(env, false);
  } else if (status != UV_ECANCELED) {
    JSValue args[2], r;
    if (job->traits->result(ctx, job, &err, &res) < 0) {
      JSValue e = JS_GetException(ctx);
      r = async_wrap_make_callback_name(&job->aw, "ondone", 1, (JSValueConst *)&e);
      JS_FreeValue(ctx, e);
    } else {
      args[0] = err;
      args[1] = res;
      r = async_wrap_make_callback_name(&job->aw, "ondone", 2, (JSValueConst *)args);
      JS_FreeValue(ctx, err);
      JS_FreeValue(ctx, res);
    }
    if (!JS_IsException(r))
      JS_FreeValue(ctx, r);
  }
  async_wrap_unref(&job->aw);
  JS_FreeValue(ctx, obj);
}

static void job_schedule(CryptoJob *job) {
  job->scheduled = true;
  async_wrap_ref(&job->aw);
  job->work.data = job;
  if (uv_queue_work(job->aw.env->loop, &job->work, job_work_cb, job_after_work_cb) != 0) {
    job->traits->work(job);
    job_after_work_cb(&job->work, 0);
  }
}

static JSValue job_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  CryptoJob *job = JS_GetOpaque2(ctx, this_val, crypto_job_class_id);
  JSValue err = JS_UNDEFINED, res = JS_UNDEFINED, arr;
  if (!job)
    return JS_EXCEPTION;
  if (job->mode == kCryptoJobWebCrypto) {
    JSValue p = JS_NewPromiseCapability(ctx, job->resolving_funcs);
    if (JS_IsException(p))
      return p;
    job_schedule(job);
    return p;
  }
  if (job->mode == kCryptoJobAsync) {
    job_schedule(job);
    return JS_UNDEFINED;
  }
  job->traits->work(job);
  if (job->traits->result(ctx, job, &err, &res) < 0)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, err);
  JS_SetPropertyUint32(ctx, arr, 1, res);
  return arr;
}

static JSValue job_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  CryptoJob *job = JS_GetOpaque(this_val, crypto_job_class_id);
  return JS_NewFloat64(ctx, job ? job->aw.async_id : -1);
}

static const JSCFunctionListEntry job_proto[] = {
  JS_CFUNC_DEF("run", 0, job_run),
  JS_CFUNC_DEF("getAsyncId", 0, job_get_async_id),
};

void crypto_define_job(JSContext *ctx, JSValueConst target, const CryptoJobTraits *traits) {
  JSValue data = JS_NewInt64(ctx, (int64_t)(uintptr_t)traits);
  JSValue ctor = JS_NewCFunctionData(ctx, job_ctor, 1, 0, 1, &data);
  JSValue proto = JS_NewObject(ctx);
  JS_SetConstructorBit(ctx, ctor, true);
  JS_SetPropertyFunctionList(ctx, proto, job_proto, countof(job_proto));
  JS_SetConstructor(ctx, ctor, proto);
  JS_DefinePropertyValueStr(ctx, ctor, "name", JS_NewString(ctx, traits->name),
                            JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, data);
  JS_DefinePropertyValueStr(ctx, target, traits->name, ctor,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* ---------------------------------------------------------------------- */
/* Hash */

static JSClassID hash_class_id, hmac_class_id;

typedef struct {
  EVP_MD_CTX *mdctx;
  unsigned md_len;
  uint8_t *digest;
  bool has_digest;
} Hash;

static void hash_finalizer(JSRuntime *rt, JSValueConst val) {
  Hash *h = JS_GetOpaque(val, hash_class_id);
  if (!h)
    return;
  EVP_MD_CTX_free(h->mdctx);
  free(h->digest);
  free(h);
}

static bool md_is_xof(const EVP_MD *md) {
  return (EVP_MD_get_flags(md) & EVP_MD_FLAG_XOF) != 0;
}

static unsigned default_xof_len(const EVP_MD *md) {
  const char *n = EVP_MD_get0_name(md);
  if (n && !strcasecmp(n, "SHAKE128"))
    return 16;
  if (n && !strcasecmp(n, "SHAKE256"))
    return 32;
  return 0;
}

/* new Hash(algorithm | hash, xofLen, algorithmId, cache) */
static JSValue hash_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, hash_class_id);
  Hash *h, *orig = NULL;
  const EVP_MD *md = NULL;
  if (JS_IsException(obj))
    return obj;
  h = calloc(1, sizeof(*h));
  JS_SetOpaque(obj, h);
  if (JS_IsObject(ARG(0))) {
    orig = JS_GetOpaque(ARG(0), hash_class_id);
    if (orig && orig->mdctx)
      md = EVP_MD_CTX_get0_md(orig->mdctx);
  } else {
    const char *name = JS_ToCString(ctx, ARG(0));
    md = crypto_get_digest(name);
    JS_FreeCString(ctx, name);
  }
  if (md) {
    h->mdctx = EVP_MD_CTX_new();
    if (!h->mdctx || EVP_DigestInit_ex(h->mdctx, md, NULL) <= 0) {
      EVP_MD_CTX_free(h->mdctx);
      h->mdctx = NULL;
      md = NULL;
    }
  }
  if (md) {
    int size = EVP_MD_get_size(md);
    h->md_len = size > 0 ? (unsigned)size : 0;
    if (md_is_xof(md) && h->md_len == 0)
      h->md_len = default_xof_len(md);
    if (!JS_IsUndefined(ARG(1))) {
      uint32_t xof = nb_uint32(ctx, ARG(1), 0);
      if (xof != h->md_len) {
        if (!md_is_xof(md)) {
          ERR_raise(ERR_LIB_EVP, EVP_R_NOT_XOF_OR_INVALID_LENGTH);
          EVP_MD_CTX_free(h->mdctx);
          h->mdctx = NULL;
          md = NULL;
        } else {
          h->md_len = xof;
        }
      }
    }
  }
  if (!md) {
    JS_FreeValue(ctx, obj);
    return crypto_throw(ctx, ERR_get_error(), "Digest method not supported");
  }
  if (orig && EVP_MD_CTX_copy_ex(h->mdctx, orig->mdctx) <= 0) {
    JS_FreeValue(ctx, obj);
    return crypto_throw(ctx, ERR_get_error(), "Digest copy error");
  }
  return obj;
}

static JSValue hash_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Hash *h = JS_GetOpaque2(ctx, this_val, hash_class_id);
  uint8_t *owned, *d;
  size_t len;
  bool ok;
  if (!h)
    return JS_EXCEPTION;
  d = decode_data(ctx, ARG(0), ARG(1), &len, &owned);
  if (!d)
    return JS_EXCEPTION;
  if (len > INT32_MAX) {
    free(owned);
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "data is too long");
  }
  ok = h->mdctx && EVP_DigestUpdate(h->mdctx, d, len) > 0;
  free(owned);
  return JS_NewBool(ctx, ok);
}

static JSValue hash_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Hash *h = JS_GetOpaque2(ctx, this_val, hash_class_id);
  int enc = ENC_BUFFER;
  if (!h)
    return JS_EXCEPTION;
  if (argc >= 1)
    enc = node_parse_encoding(ctx, ARG(0), ENC_BUFFER);
  if (!h->has_digest && h->md_len > 0) {
    int ok;
    h->digest = malloc(h->md_len);
    if (h->mdctx && md_is_xof(EVP_MD_CTX_get0_md(h->mdctx)))
      ok = EVP_DigestFinalXOF(h->mdctx, h->digest, h->md_len) > 0;
    else {
      unsigned n = h->md_len;
      ok = h->mdctx && EVP_DigestFinal_ex(h->mdctx, h->digest, &n) > 0;
    }
    if (!ok) {
      free(h->digest);
      h->digest = NULL;
      return crypto_throw(ctx, ERR_get_error(), NULL);
    }
    h->has_digest = true;
  }
  return node_string_encode(ctx, h->digest ? h->digest : (uint8_t *)"", h->md_len, enc);
}

static const JSCFunctionListEntry hash_proto[] = {
  JS_CFUNC_DEF("update", 2, hash_update),
  JS_CFUNC_DEF("digest", 1, hash_digest),
};

/* oneShotDigest(algorithm, id, cache, input, outputEncoding, encId, outputLength) */
static JSValue one_shot_digest(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  const char *name = JS_ToCString(ctx, ARG(0));
  const EVP_MD *md = crypto_get_digest(name);
  int enc, outlen;
  uint8_t *owned = NULL, *in, *out;
  size_t inlen;
  bool xof;
  JSValue ret;
  if (!md) {
    char msg[300];
    snprintf(msg, sizeof(msg), "Digest method %s is not supported", name ? name : "");
    JS_FreeCString(ctx, name);
    return crypto_throw(ctx, ERR_get_error(), msg);
  }
  enc = node_parse_encoding(ctx, ARG(4), ENC_HEX);
  xof = md_is_xof(md);
  outlen = EVP_MD_get_size(md);
  if (!xof && !JS_IsUndefined(ARG(6))) {
    outlen = (int)nb_uint32(ctx, ARG(6), 0);
    if (outlen != EVP_MD_get_size(md)) {
      char msg[300];
      snprintf(msg, sizeof(msg), "Output length %d is invalid for %s, which does not support XOF",
               outlen, name);
      JS_FreeCString(ctx, name);
      return crypto_throw(ctx, ERR_get_error(), msg);
    }
  } else if (xof) {
    if (!JS_IsUndefined(ARG(6)))
      outlen = (int)nb_uint32(ctx, ARG(6), 0);
    else if (outlen <= 0)
      outlen = (int)default_xof_len(md);
  }
  JS_FreeCString(ctx, name);
  if (outlen <= 0)
    return enc == ENC_BUFFER ? nb_new_buffer(ctx, "", 0) : JS_NewString(ctx, "");
  if (JS_IsString(ARG(3))) {
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, ARG(3));
    owned = malloc(n + 1);
    memcpy(owned, s, n);
    JS_FreeCString(ctx, s);
    in = owned;
    inlen = n;
  } else {
    in = crypto_buffer_source(ctx, ARG(3), &inlen);
  }
  out = malloc(outlen);
  {
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    int ok = EVP_DigestInit_ex(c, md, NULL) > 0 && EVP_DigestUpdate(c, in, inlen) > 0;
    if (ok) {
      if (xof)
        ok = EVP_DigestFinalXOF(c, out, outlen) > 0;
      else {
        unsigned n = outlen;
        ok = EVP_DigestFinal_ex(c, out, &n) > 0;
      }
    }
    EVP_MD_CTX_free(c);
    free(owned);
    if (!ok) {
      free(out);
      return crypto_throw(ctx, ERR_get_error(), NULL);
    }
  }
  ret = node_string_encode(ctx, out, outlen, enc);
  free(out);
  return ret;
}

static JSValue get_cached_aliases(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  return JS_NewObjectProto(ctx, JS_NULL);
}

/* HashJob(mode, algorithm, data[, outputLengthBits]) */
typedef struct {
  const EVP_MD *md;
  uint8_t *in;
  size_t inlen;
  unsigned outlen;
  uint8_t *out;
} HashParams;

static int hash_job_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  HashParams *p = job->params;
  const char *name = JS_ToCString(ctx, ARG(0));
  p->md = crypto_get_digest(name);
  if (!p->md) {
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s", name ? name : "");
    JS_FreeCString(ctx, name);
    return -1;
  }
  JS_FreeCString(ctx, name);
  p->in = crypto_bytes_copy(ctx, ARG(1), &p->inlen);
  if (!p->in)
    return -1;
  p->outlen = EVP_MD_get_size(p->md) > 0 ? EVP_MD_get_size(p->md) : default_xof_len(p->md);
  if (argc > 2 && !JS_IsUndefined(ARG(2))) {
    uint32_t bits = nb_uint32(ctx, ARG(2), 0);
    p->outlen = bits / 8;
  }
  return 0;
}

static void hash_job_work(CryptoJob *job) {
  HashParams *p = job->params;
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  int ok;
  p->out = malloc(p->outlen ? p->outlen : 1);
  ok = EVP_DigestInit_ex(c, p->md, NULL) > 0 && EVP_DigestUpdate(c, p->in, p->inlen) > 0;
  if (ok && p->outlen) {
    if (md_is_xof(p->md))
      ok = EVP_DigestFinalXOF(c, p->out, p->outlen) > 0;
    else {
      unsigned n = p->outlen;
      ok = EVP_DigestFinal_ex(c, p->out, &n) > 0;
    }
  }
  EVP_MD_CTX_free(c);
  if (!ok)
    crypto_job_capture_errors(job, "Digest operation failed");
}

static int bytes_result(JSContext *ctx, CryptoJob *job, const uint8_t *out, size_t len,
                        JSValue *err, JSValue *res) {
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  *res = crypto_new_array_buffer(ctx, out, len);
  return 0;
}

static int hash_job_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  HashParams *p = job->params;
  return bytes_result(ctx, job, p->out, p->outlen, err, res);
}

static void hash_job_cleanup(CryptoJob *job) {
  HashParams *p = job->params;
  free(p->in);
  free(p->out);
}

static const CryptoJobTraits hash_job = {
  "HashJob", PROVIDER_HASHREQUEST, sizeof(HashParams),
  hash_job_config, hash_job_work, hash_job_result, hash_job_cleanup,
};

/* ---------------------------------------------------------------------- */
/* Hmac */

typedef struct {
  EVP_MAC_CTX *ctx;
} Hmac;

static void hmac_finalizer(JSRuntime *rt, JSValueConst val) {
  Hmac *h = JS_GetOpaque(val, hmac_class_id);
  if (!h)
    return;
  EVP_MAC_CTX_free(h->ctx);
  free(h);
}

static JSValue hmac_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, hmac_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(Hmac)));
  return obj;
}

static EVP_MAC_CTX *hmac_new(const EVP_MD *md, const uint8_t *key, size_t keylen) {
  EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  EVP_MAC_CTX *c;
  OSSL_PARAM params[2];
  static const uint8_t empty = 0;
  if (!mac)
    return NULL;
  c = EVP_MAC_CTX_new(mac);
  EVP_MAC_free(mac);
  if (!c)
    return NULL;
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                               (char *)EVP_MD_get0_name(md), 0);
  params[1] = OSSL_PARAM_construct_end();
  if (EVP_MAC_init(c, keylen ? key : &empty, keylen, params) <= 0) {
    EVP_MAC_CTX_free(c);
    return NULL;
  }
  return c;
}

/* init(hmac, key) */
static JSValue hmac_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Hmac *h = JS_GetOpaque2(ctx, this_val, hmac_class_id);
  const char *name;
  const EVP_MD *md;
  uint8_t *key;
  size_t keylen;
  if (!h)
    return JS_EXCEPTION;
  name = JS_ToCString(ctx, ARG(0));
  md = crypto_get_digest(name);
  if (!md) {
    JSValue r = crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s",
                                  name ? name : "");
    JS_FreeCString(ctx, name);
    return r;
  }
  JS_FreeCString(ctx, name);
  key = crypto_bytes_copy(ctx, ARG(1), &keylen);
  if (!key)
    return JS_EXCEPTION;
  EVP_MAC_CTX_free(h->ctx);
  h->ctx = hmac_new(md, key, keylen);
  OPENSSL_cleanse(key, keylen);
  free(key);
  if (!h->ctx)
    return crypto_throw(ctx, ERR_get_error(), NULL);
  return JS_UNDEFINED;
}

static JSValue hmac_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Hmac *h = JS_GetOpaque2(ctx, this_val, hmac_class_id);
  uint8_t *owned, *d;
  size_t len;
  bool ok;
  if (!h)
    return JS_EXCEPTION;
  d = decode_data(ctx, ARG(0), ARG(1), &len, &owned);
  if (!d)
    return JS_EXCEPTION;
  ok = h->ctx && EVP_MAC_update(h->ctx, d, len) > 0;
  free(owned);
  return JS_NewBool(ctx, ok);
}

static JSValue hmac_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Hmac *h = JS_GetOpaque2(ctx, this_val, hmac_class_id);
  uint8_t md[EVP_MAX_MD_SIZE];
  size_t len = 0;
  int enc = ENC_BUFFER;
  if (!h)
    return JS_EXCEPTION;
  if (argc >= 1)
    enc = node_parse_encoding(ctx, ARG(0), ENC_BUFFER);
  if (h->ctx) {
    int ok = EVP_MAC_final(h->ctx, md, &len, sizeof(md)) > 0;
    EVP_MAC_CTX_free(h->ctx);
    h->ctx = NULL;
    if (!ok)
      return crypto_throw(ctx, ERR_get_error(), "Failed to finalize HMAC");
  }
  return node_string_encode(ctx, md, len, enc);
}

static const JSCFunctionListEntry hmac_proto[] = {
  JS_CFUNC_DEF("init", 2, hmac_init),
  JS_CFUNC_DEF("update", 2, hmac_update),
  JS_CFUNC_DEF("digest", 1, hmac_digest),
};

/* HmacJob(mode, signMode, hash, keyHandle, data[, signature]) */
typedef struct {
  int sign_mode;
  const EVP_MD *md;
  uint8_t *key, *data, *sig;
  size_t keylen, datalen, siglen;
  uint8_t out[EVP_MAX_MD_SIZE];
  size_t outlen;
} HmacParams;

static int hmac_job_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  HmacParams *p = job->params;
  const char *name;
  p->sign_mode = nb_int32(ctx, ARG(0), kSignJobModeSign);
  name = JS_ToCString(ctx, ARG(1));
  p->md = crypto_get_digest(name);
  if (!p->md) {
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s", name ? name : "");
    JS_FreeCString(ctx, name);
    return -1;
  }
  JS_FreeCString(ctx, name);
  if (!(p->key = crypto_bytes_copy(ctx, ARG(2), &p->keylen)))
    return -1;
  if (!(p->data = crypto_bytes_copy(ctx, ARG(3), &p->datalen)))
    return -1;
  if (p->sign_mode == kSignJobModeVerify &&
      !(p->sig = crypto_bytes_copy(ctx, ARG(4), &p->siglen)))
    return -1;
  return 0;
}

static void hmac_job_work(CryptoJob *job) {
  HmacParams *p = job->params;
  EVP_MAC_CTX *c = hmac_new(p->md, p->key, p->keylen);
  if (!c || EVP_MAC_update(c, p->data, p->datalen) <= 0 ||
      EVP_MAC_final(c, p->out, &p->outlen, sizeof(p->out)) <= 0)
    crypto_job_capture_errors(job, NULL);
  EVP_MAC_CTX_free(c);
}

static int hmac_job_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  HmacParams *p = job->params;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  if (p->sign_mode == kSignJobModeSign)
    *res = crypto_new_array_buffer(ctx, p->out, p->outlen);
  else
    *res = JS_NewBool(ctx, p->siglen > 0 && p->siglen == p->outlen &&
                               CRYPTO_memcmp(p->out, p->sig, p->outlen) == 0);
  return 0;
}

static void hmac_job_cleanup(CryptoJob *job) {
  HmacParams *p = job->params;
  if (p->key)
    OPENSSL_cleanse(p->key, p->keylen);
  free(p->key);
  free(p->data);
  free(p->sig);
}

static const CryptoJobTraits hmac_job = {
  "HmacJob", PROVIDER_SIGNREQUEST, sizeof(HmacParams),
  hmac_job_config, hmac_job_work, hmac_job_result, hmac_job_cleanup,
};

/* ---------------------------------------------------------------------- */
/* random */

/* RandomBytesJob(mode, buffer, offset, size) */
typedef struct {
  uint8_t *data;
  size_t size;
  JSValue keep;
} RandomBytesParams;

static int random_bytes_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  RandomBytesParams *p = job->params;
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, ARG(0), &len);
  uint32_t off = nb_uint32(ctx, ARG(1), 0), size = nb_uint32(ctx, ARG(2), 0);
  p->keep = JS_UNDEFINED;
  if (!d || (uint64_t)off + size > len)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "buffer is out of bounds"), -1;
  p->data = d + off;
  p->size = size;
  p->keep = JS_DupValue(ctx, ARG(0));
  return 0;
}

static void random_bytes_work(CryptoJob *job) {
  RandomBytesParams *p = job->params;
  if (p->size && RAND_bytes(p->data, (int)p->size) != 1)
    crypto_job_capture_errors(job, "Random bytes generation failed");
}

static int random_bytes_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  RandomBytesParams *p = job->params;
  *err = crypto_job_failed(job) ? crypto_job_error(ctx, job) : JS_UNDEFINED;
  *res = JS_UNDEFINED;
  JS_FreeValue(ctx, p->keep);
  p->keep = JS_UNDEFINED;
  return 0;
}

static void random_bytes_cleanup(CryptoJob *job) {
  RandomBytesParams *p = job->params;
  if (!JS_IsUndefined(p->keep))
    JS_FreeValueRT(job->aw.env->rt, p->keep);
}

static const CryptoJobTraits random_bytes_job = {
  "RandomBytesJob", PROVIDER_RANDOMBYTESREQUEST, sizeof(RandomBytesParams),
  random_bytes_config, random_bytes_work, random_bytes_result, random_bytes_cleanup,
};

/* RandomPrimeJob(mode, bits, safe, add, rem) */
typedef struct {
  int bits;
  bool safe;
  BIGNUM *add, *rem, *prime;
} PrimeParams;

static BIGNUM *bn_from(JSContext *ctx, JSValueConst v) {
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, v, &len);
  return d ? BN_bin2bn(d, (int)len, NULL) : NULL;
}

static int random_prime_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  PrimeParams *p = job->params;
  p->bits = nb_int32(ctx, ARG(0), 0);
  p->safe = JS_ToBool(ctx, ARG(1));
  if (!JS_IsUndefined(ARG(2))) {
    p->add = bn_from(ctx, ARG(2));
    if (!p->add)
      return crypto_throw(ctx, ERR_get_error(), "could not generate prime"), -1;
    if (BN_num_bits(p->add) > p->bits)
      return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "invalid options.add"), -1;
  }
  if (!JS_IsUndefined(ARG(3))) {
    p->rem = bn_from(ctx, ARG(3));
    if (!p->rem)
      return crypto_throw(ctx, ERR_get_error(), "could not generate prime"), -1;
    if (p->add && BN_cmp(p->add, p->rem) != 1)
      return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "invalid options.rem"), -1;
  }
  return 0;
}

static void random_prime_work(CryptoJob *job) {
  PrimeParams *p = job->params;
  BN_CTX *bn = BN_CTX_new();
  p->prime = BN_secure_new();
  if (!p->prime || !bn ||
      !BN_generate_prime_ex2(p->prime, p->bits, p->safe, p->add, p->rem, NULL, bn))
    crypto_job_capture_errors(job, "Random prime generation failed");
  BN_CTX_free(bn);
}

static int random_prime_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  PrimeParams *p = job->params;
  size_t n;
  uint8_t *buf;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  n = BN_num_bytes(p->prime);
  buf = malloc(n ? n : 1);
  BN_bn2binpad(p->prime, buf, (int)n);
  *err = JS_UNDEFINED;
  *res = crypto_new_array_buffer(ctx, buf, n);
  free(buf);
  return 0;
}

static void random_prime_cleanup(CryptoJob *job) {
  PrimeParams *p = job->params;
  BN_free(p->add);
  BN_free(p->rem);
  BN_clear_free(p->prime);
}

static const CryptoJobTraits random_prime_job = {
  "RandomPrimeJob", PROVIDER_RANDOMPRIMEREQUEST, sizeof(PrimeParams),
  random_prime_config, random_prime_work, random_prime_result, random_prime_cleanup,
};

/* CheckPrimeJob(mode, candidate, checks) */
typedef struct {
  BIGNUM *candidate;
  int checks;
  int result;
} CheckPrimeParams;

static int check_prime_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  CheckPrimeParams *p = job->params;
  p->candidate = bn_from(ctx, ARG(0));
  if (!p->candidate)
    return crypto_throw(ctx, ERR_get_error(), "could not read candidate"), -1;
  p->checks = nb_int32(ctx, ARG(1), 0);
  return 0;
}

static void check_prime_work(CryptoJob *job) {
  CheckPrimeParams *p = job->params;
  BN_CTX *bctx = BN_CTX_new();
  p->result = BN_check_prime(p->candidate, bctx, NULL);
  BN_CTX_free(bctx);
  if (p->result < 0)
    crypto_job_capture_errors(job, "check prime failed");
}

static int check_prime_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  CheckPrimeParams *p = job->params;
  if (crypto_job_failed(job)) {
    *err = crypto_job_error(ctx, job);
    *res = JS_UNDEFINED;
    return 0;
  }
  *err = JS_UNDEFINED;
  *res = JS_NewBool(ctx, p->result == 1);
  return 0;
}

static void check_prime_cleanup(CryptoJob *job) {
  CheckPrimeParams *p = job->params;
  BN_free(p->candidate);
}

static const CryptoJobTraits check_prime_job = {
  "CheckPrimeJob", PROVIDER_CHECKPRIMEREQUEST, sizeof(CheckPrimeParams),
  check_prime_config, check_prime_work, check_prime_result, check_prime_cleanup,
};

/* ---------------------------------------------------------------------- */
/* key derivation */

typedef struct {
  uint8_t *pass, *salt, *info, *out;
  size_t passlen, saltlen, infolen, outlen;
  const EVP_MD *md;
  uint32_t iterations;
  uint64_t N, maxmem;
  uint32_t r, p;
} KdfParams;

static void kdf_cleanup(CryptoJob *job) {
  KdfParams *p = job->params;
  if (p->pass)
    OPENSSL_cleanse(p->pass, p->passlen);
  free(p->pass);
  free(p->salt);
  free(p->info);
  if (p->out)
    OPENSSL_cleanse(p->out, p->outlen);
  free(p->out);
}

static int kdf_result(JSContext *ctx, CryptoJob *job, JSValue *err, JSValue *res) {
  KdfParams *p = job->params;
  return bytes_result(ctx, job, p->out, p->outlen, err, res);
}

static const EVP_MD *digest_arg(JSContext *ctx, JSValueConst v) {
  const char *name = JS_ToCString(ctx, v);
  const EVP_MD *md = crypto_get_digest(name);
  if (!md)
    crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s", name ? name : "");
  JS_FreeCString(ctx, name);
  return md;
}

/* PBKDF2Job(mode, password, salt, iterations, keylen, digest) */
static int pbkdf2_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KdfParams *p = job->params;
  if (!(p->pass = crypto_bytes_copy(ctx, ARG(0), &p->passlen)))
    return -1;
  if (!(p->salt = crypto_bytes_copy(ctx, ARG(1), &p->saltlen)))
    return -1;
  p->iterations = nb_uint32(ctx, ARG(2), 1);
  p->outlen = nb_uint32(ctx, ARG(3), 0);
  if (!(p->md = digest_arg(ctx, ARG(4))))
    return -1;
  return 0;
}

static void pbkdf2_work(CryptoJob *job) {
  KdfParams *p = job->params;
  p->out = malloc(p->outlen ? p->outlen : 1);
  if (p->outlen && PKCS5_PBKDF2_HMAC((const char *)p->pass, (int)p->passlen, p->salt,
                                     (int)p->saltlen, (int)p->iterations, p->md,
                                     (int)p->outlen, p->out) != 1)
    crypto_job_capture_errors(job, "PBKDF2 failed");
}

static const CryptoJobTraits pbkdf2_job = {
  "PBKDF2Job", PROVIDER_PBKDF2REQUEST, sizeof(KdfParams),
  pbkdf2_config, pbkdf2_work, kdf_result, kdf_cleanup,
};

/* HKDFJob(mode, hash, key, salt, info, length) */
static int hkdf_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KdfParams *p = job->params;
  if (!(p->md = digest_arg(ctx, ARG(0))))
    return -1;
  if (!(p->pass = crypto_bytes_copy(ctx, ARG(1), &p->passlen)))
    return -1;
  if (!(p->salt = crypto_bytes_copy(ctx, ARG(2), &p->saltlen)))
    return -1;
  if (!(p->info = crypto_bytes_copy(ctx, ARG(3), &p->infolen)))
    return -1;
  p->outlen = nb_uint32(ctx, ARG(4), 0);
  {
    int max = 255 * EVP_MD_get_size(p->md);
    if (EVP_MD_get_size(p->md) > 0 && p->outlen > (size_t)max)
      return crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length"), -1;
  }
  return 0;
}

static void hkdf_work(CryptoJob *job) {
  KdfParams *p = job->params;
  EVP_KDF *kdf;
  EVP_KDF_CTX *kctx;
  OSSL_PARAM params[5], *q = params;
  static uint8_t zero_salt[EVP_MAX_MD_SIZE];
  p->out = malloc(p->outlen ? p->outlen : 1);
  if (!p->outlen)
    return;
  kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
  kctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
  EVP_KDF_free(kdf);
  *q++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                          (char *)EVP_MD_get0_name(p->md), 0);
  *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, p->passlen ? p->pass : zero_salt,
                                           p->passlen);
  /* an empty salt means HashLen zeros (RFC 5869) */
  if (p->saltlen)
    *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, p->salt, p->saltlen);
  else
    *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, zero_salt,
                                             EVP_MD_get_size(p->md));
  *q++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, p->info, p->infolen);
  *q = OSSL_PARAM_construct_end();
  if (!kctx || EVP_KDF_derive(kctx, p->out, p->outlen, params) <= 0)
    crypto_job_capture_errors(job, "HKDF failed");
  EVP_KDF_CTX_free(kctx);
}

static const CryptoJobTraits hkdf_job = {
  "HKDFJob", PROVIDER_DERIVEBITSREQUEST, sizeof(KdfParams),
  hkdf_config, hkdf_work, kdf_result, kdf_cleanup,
};

/* ScryptJob(mode, password, salt, N, r, p, maxmem, keylen) */
static int scrypt_config(JSContext *ctx, CryptoJob *job, int argc, JSValueConst *argv) {
  KdfParams *p = job->params;
  int64_t maxmem;
  if (!(p->pass = crypto_bytes_copy(ctx, ARG(0), &p->passlen)))
    return -1;
  if (!(p->salt = crypto_bytes_copy(ctx, ARG(1), &p->saltlen)))
    return -1;
  p->N = nb_uint32(ctx, ARG(2), 0);
  p->r = nb_uint32(ctx, ARG(3), 0);
  p->p = nb_uint32(ctx, ARG(4), 0);
  maxmem = nb_int64(ctx, ARG(5), 0);
  p->maxmem = maxmem < 0 ? 0 : (uint64_t)maxmem;
  p->outlen = nb_uint32(ctx, ARG(6), 0);
  if (EVP_PBE_scrypt(NULL, 0, NULL, 0, p->N, p->r, p->p, p->maxmem, NULL, 0) != 1) {
    unsigned long e = ERR_peek_last_error();
    if (e) {
      char buf[256];
      ERR_error_string_n(e, buf, sizeof(buf));
      crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "Invalid scrypt params: %s", buf);
    } else {
      crypto_throw_code(ctx, "ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "Invalid scrypt params");
    }
    return -1;
  }
  return 0;
}

static void scrypt_work(CryptoJob *job) {
  KdfParams *p = job->params;
  p->out = malloc(p->outlen ? p->outlen : 1);
  if (p->outlen && EVP_PBE_scrypt((const char *)p->pass, p->passlen, p->salt, p->saltlen, p->N,
                                  p->r, p->p, p->maxmem, p->out, p->outlen) != 1)
    crypto_job_capture_errors(job, "Scrypt failed");
}

static const CryptoJobTraits scrypt_job = {
  "ScryptJob", PROVIDER_SCRYPTREQUEST, sizeof(KdfParams),
  scrypt_config, scrypt_work, kdf_result, kdf_cleanup,
};

/* ---------------------------------------------------------------------- */
/* misc functions */

typedef struct {
  JSContext *ctx;
  JSValue arr;
  uint32_t n;
} NameList;

static void collect_md(EVP_MD *md, void *arg) {
  NameList *l = arg;
  const char *n = EVP_MD_get0_name(md);
  if (n) {
    char low[128];
    size_t i;
    snprintf(low, sizeof(low), "%s", n);
    for (i = 0; low[i]; i++)
      low[i] = (char)tolower((unsigned char)low[i]);
    JS_SetPropertyUint32(l->ctx, l->arr, l->n++, JS_NewString(l->ctx, low));
  }
}

static void collect_obj_name(const OBJ_NAME *o, void *arg) {
  NameList *l = arg;
  char low[128];
  size_t i;
  snprintf(low, sizeof(low), "%s", o->name);
  for (i = 0; low[i]; i++)
    low[i] = (char)tolower((unsigned char)low[i]);
  JS_SetPropertyUint32(l->ctx, l->arr, l->n++, JS_NewString(l->ctx, low));
}

static JSValue get_hashes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  NameList l = { ctx, JS_NewArray(ctx), 0 };
  OBJ_NAME_do_all_sorted(OBJ_NAME_TYPE_MD_METH, collect_obj_name, &l);
  EVP_MD_do_all_provided(NULL, collect_md, &l);
  return l.arr;
}

static void collect_cipher(EVP_CIPHER *c, void *arg) {
  NameList *l = arg;
  const char *n = EVP_CIPHER_get0_name(c);
  if (n) {
    char low[128];
    size_t i;
    snprintf(low, sizeof(low), "%s", n);
    for (i = 0; low[i]; i++)
      low[i] = (char)tolower((unsigned char)low[i]);
    JS_SetPropertyUint32(l->ctx, l->arr, l->n++, JS_NewString(l->ctx, low));
  }
}

static JSValue get_ciphers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  NameList l = { ctx, JS_NewArray(ctx), 0 };
  OBJ_NAME_do_all_sorted(OBJ_NAME_TYPE_CIPHER_METH, collect_obj_name, &l);
  EVP_CIPHER_do_all_provided(NULL, collect_cipher, &l);
  return l.arr;
}

static JSValue get_ssl_ciphers(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  SSL_CTX *sctx = SSL_CTX_new(TLS_method());
  SSL *ssl = sctx ? SSL_new(sctx) : NULL;
  JSValue arr = JS_NewArray(ctx);
  if (ssl) {
    STACK_OF(SSL_CIPHER) *ciphers = SSL_get_ciphers(ssl);
    int i, n = sk_SSL_CIPHER_num(ciphers);
    for (i = 0; i < n; i++) {
      const char *name = SSL_CIPHER_get_name(sk_SSL_CIPHER_value(ciphers, i));
      char low[128];
      size_t j;
      snprintf(low, sizeof(low), "%s", name);
      for (j = 0; low[j]; j++)
        low[j] = (char)tolower((unsigned char)low[j]);
      JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, low));
    }
  }
  SSL_free(ssl);
  SSL_CTX_free(sctx);
  return arr;
}

static JSValue get_curves(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t n = EC_get_builtin_curves(NULL, 0), i;
  EC_builtin_curve *curves = malloc(sizeof(*curves) * (n ? n : 1));
  JSValue arr = JS_NewArray(ctx);
  n = EC_get_builtin_curves(curves, n);
  for (i = 0; i < n; i++)
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, OBJ_nid2sn(curves[i].nid)));
  free(curves);
  return arr;
}

static JSValue timing_safe_equal(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  size_t la, lb;
  uint8_t *a = crypto_buffer_source(ctx, ARG(0), &la), *b = crypto_buffer_source(ctx, ARG(1), &lb);
  if (!a || !b)
    return JS_ThrowTypeError(ctx, "arguments must be buffers");
  return JS_NewBool(ctx, la == lb && CRYPTO_memcmp(a, b, la) == 0);
}

static JSValue secure_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  uint32_t n = nb_uint32(ctx, ARG(0), 0);
  JSValue ab = JS_NewArrayBufferCopy(ctx, NULL, n), u8;
  if (JS_IsException(ab))
    return ab;
  {
    JSValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Uint8Array");
    u8 = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&ab);
    JS_FreeValue(ctx, ctor);
  }
  JS_FreeValue(ctx, ab);
  return u8;
}

static JSValue secure_heap_used(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue get_fips_crypto(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  return JS_NewInt32(ctx, EVP_default_properties_is_fips_enabled(NULL) ? 1 : 0);
}

static JSValue set_fips_crypto(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  if (JS_ToBool(ctx, ARG(0)))
    return crypto_throw(ctx, 0, "Cannot set FIPS mode in a non-FIPS build.");
  return JS_UNDEFINED;
}

static JSValue test_fips_crypto(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_FALSE;
}

static JSValue get_sec_level(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  SSL_CTX *c = SSL_CTX_new(TLS_method());
  int lvl = c ? SSL_CTX_get_security_level(c) : 1;
  SSL_CTX_free(c);
  return JS_NewInt32(ctx, lvl);
}

static JSValue set_engine(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return crypto_throw_code(ctx, "ERR_CRYPTO_ENGINE_UNKNOWN", "Engine \"%s\" was not found",
                           "engine");
}

static JSValue cert_verify_spkac(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, ARG(0), &len);
  NETSCAPE_SPKI *spki;
  EVP_PKEY *pk;
  bool ok = false;
  if (!d || !len)
    return JS_FALSE;
  {
    char *s = malloc(len + 1);
    size_t n = len;
    memcpy(s, d, len);
    s[len] = 0;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
      s[--n] = 0;
    spki = NETSCAPE_SPKI_b64_decode(s, (int)n);
    free(s);
  }
  if (spki) {
    pk = NETSCAPE_SPKI_get_pubkey(spki);
    ok = pk && NETSCAPE_SPKI_verify(spki, pk) > 0;
    EVP_PKEY_free(pk);
    NETSCAPE_SPKI_free(spki);
  }
  ERR_clear_error();
  return JS_NewBool(ctx, ok);
}

static NETSCAPE_SPKI *spkac_from(JSContext *ctx, JSValueConst v) {
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, v, &len);
  NETSCAPE_SPKI *spki;
  char *s;
  size_t n = len;
  if (!d || !len)
    return NULL;
  s = malloc(len + 1);
  memcpy(s, d, len);
  s[len] = 0;
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    s[--n] = 0;
  spki = NETSCAPE_SPKI_b64_decode(s, (int)n);
  free(s);
  return spki;
}

static JSValue cert_export_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  NETSCAPE_SPKI *spki = spkac_from(ctx, ARG(0));
  EVP_PKEY *pk;
  BIO *bio;
  BUF_MEM *mem;
  JSValue r = JS_UNDEFINED;
  if (!spki)
    return nb_new_buffer(ctx, "", 0);
  pk = NETSCAPE_SPKI_get_pubkey(spki);
  bio = BIO_new(BIO_s_mem());
  if (pk && PEM_write_bio_PUBKEY(bio, pk) > 0) {
    BIO_get_mem_ptr(bio, &mem);
    r = nb_new_buffer(ctx, mem->data, mem->length);
  } else {
    r = nb_new_buffer(ctx, "", 0);
  }
  BIO_free(bio);
  EVP_PKEY_free(pk);
  NETSCAPE_SPKI_free(spki);
  ERR_clear_error();
  return r;
}

static JSValue cert_export_challenge(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  NETSCAPE_SPKI *spki = spkac_from(ctx, ARG(0));
  JSValue r;
  if (!spki)
    return nb_new_buffer(ctx, "", 0);
  r = nb_new_buffer(ctx, ASN1_STRING_get0_data(spki->spkac->challenge),
                    ASN1_STRING_length(spki->spkac->challenge));
  NETSCAPE_SPKI_free(spki);
  return r;
}

/* ---------------------------------------------------------------------- */

static void define_classes(JSContext *ctx, JSValueConst t) {
  NodeClassDef hdef = { .name = "Hash", .class_id = &hash_class_id, .ctor = hash_ctor,
                        .ctor_length = 4, .finalizer = hash_finalizer, .proto_funcs = hash_proto,
                        .proto_funcs_count = countof(hash_proto), .parent_ctor = JS_UNDEFINED };
  NodeClassDef mdef = { .name = "Hmac", .class_id = &hmac_class_id, .ctor = hmac_ctor,
                        .finalizer = hmac_finalizer, .proto_funcs = hmac_proto,
                        .proto_funcs_count = countof(hmac_proto), .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &hdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &mdef));
}

JSValue binding_init_crypto(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  JSRuntime *rt = JS_GetRuntime(ctx);
  static const struct { const char *name; int value; } consts[] = {
#define C(n) { #n, n }
    C(kCryptoJobAsync), C(kCryptoJobSync), C(kCryptoJobWebCrypto),
    C(kKeyTypeSecret), C(kKeyTypePublic), C(kKeyTypePrivate),
    C(kKeyFormatDER), C(kKeyFormatPEM), C(kKeyFormatJWK), C(kKeyFormatRawPublic),
    C(kKeyFormatRawPrivate), C(kKeyFormatRawSeed), C(kKeyFormatStore),
    C(kKeyEncodingPKCS1), C(kKeyEncodingPKCS8), C(kKeyEncodingSPKI), C(kKeyEncodingSEC1),
    C(kSigEncDER), C(kSigEncP1363), C(kSignJobModeSign), C(kSignJobModeVerify),
    C(kWebCryptoKeyFormatRaw), C(kWebCryptoKeyFormatPKCS8), C(kWebCryptoKeyFormatSPKI),
    C(kWebCryptoKeyFormatJWK), C(kWebCryptoCipherEncrypt), C(kWebCryptoCipherDecrypt),
#undef C
    { "kKeyVariantAES_CTR_128", 0 }, { "kKeyVariantAES_CTR_192", 1 },
    { "kKeyVariantAES_CTR_256", 2 }, { "kKeyVariantAES_CBC_128", 3 },
    { "kKeyVariantAES_CBC_192", 4 }, { "kKeyVariantAES_CBC_256", 5 },
    { "kKeyVariantAES_GCM_128", 6 }, { "kKeyVariantAES_GCM_192", 7 },
    { "kKeyVariantAES_GCM_256", 8 }, { "kKeyVariantAES_KW_128", 9 },
    { "kKeyVariantAES_KW_192", 10 }, { "kKeyVariantAES_KW_256", 11 },
    { "kKeyVariantAES_OCB_128", 12 }, { "kKeyVariantAES_OCB_192", 13 },
    { "kKeyVariantAES_OCB_256", 14 },
    { "kKeyVariantRSA_SSA_PKCS1_v1_5", 0 }, { "kKeyVariantRSA_PSS", 1 },
    { "kKeyVariantRSA_OAEP", 2 },
    { "OPENSSL_EC_NAMED_CURVE", OPENSSL_EC_NAMED_CURVE },
    { "OPENSSL_EC_EXPLICIT_CURVE", OPENSSL_EC_EXPLICIT_CURVE },
    { "EVP_PKEY_ED25519", EVP_PKEY_ED25519 }, { "EVP_PKEY_ED448", EVP_PKEY_ED448 },
    { "EVP_PKEY_X25519", EVP_PKEY_X25519 }, { "EVP_PKEY_X448", EVP_PKEY_X448 },
    { "RSA_PKCS1_PSS_PADDING", RSA_PKCS1_PSS_PADDING },
    { "X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT", X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT },
    { "X509_CHECK_FLAG_NEVER_CHECK_SUBJECT", X509_CHECK_FLAG_NEVER_CHECK_SUBJECT },
    { "X509_CHECK_FLAG_NO_WILDCARDS", X509_CHECK_FLAG_NO_WILDCARDS },
    { "X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS", X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS },
    { "X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS", X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS },
    { "X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS", X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS },
  };
  size_t i;
  JS_NewClassID(rt, &crypto_job_class_id);
  if (!JS_IsRegisteredClass(rt, crypto_job_class_id)) {
    JSClassDef cd = { .class_name = "CryptoJob", .finalizer = job_finalizer,
                      .gc_mark = job_gc_mark };
    JS_NewClass(rt, crypto_job_class_id, &cd);
  }
  for (i = 0; i < countof(consts); i++)
    nb_set_int(ctx, t, consts[i].name, consts[i].value);

  define_classes(ctx, t);
  crypto_define_job(ctx, t, &hash_job);
  crypto_define_job(ctx, t, &hmac_job);
  crypto_define_job(ctx, t, &random_bytes_job);
  crypto_define_job(ctx, t, &random_prime_job);
  crypto_define_job(ctx, t, &check_prime_job);
  crypto_define_job(ctx, t, &pbkdf2_job);
  crypto_define_job(ctx, t, &hkdf_job);
  crypto_define_job(ctx, t, &scrypt_job);

  nb_set_method(ctx, t, "oneShotDigest", one_shot_digest, 7);
  nb_set_method(ctx, t, "getCachedAliases", get_cached_aliases, 0);
  nb_set_method(ctx, t, "getHashes", get_hashes, 0);
  nb_set_method(ctx, t, "getCiphers", get_ciphers, 0);
  nb_set_method(ctx, t, "getSSLCiphers", get_ssl_ciphers, 0);
  nb_set_method(ctx, t, "getCurves", get_curves, 0);
  nb_set_method(ctx, t, "timingSafeEqual", timing_safe_equal, 2);
  nb_set_method(ctx, t, "secureBuffer", secure_buffer, 1);
  nb_set_method(ctx, t, "secureHeapUsed", secure_heap_used, 0);
  nb_set_method(ctx, t, "getFipsCrypto", get_fips_crypto, 0);
  nb_set_method(ctx, t, "setFipsCrypto", set_fips_crypto, 1);
  nb_set_method(ctx, t, "testFipsCrypto", test_fips_crypto, 0);
  nb_set_method(ctx, t, "getOpenSSLSecLevelCrypto", get_sec_level, 0);
  nb_set_method(ctx, t, "setEngine", set_engine, 2);
  nb_set_method(ctx, t, "certVerifySpkac", cert_verify_spkac, 1);
  nb_set_method(ctx, t, "certExportPublicKey", cert_export_public_key, 1);
  nb_set_method(ctx, t, "certExportChallenge", cert_export_challenge, 1);

  crypto_init_keys(env, t);
  crypto_init_cipher(env, t);
  crypto_init_sig(env, t);
  crypto_init_x509(env, t);
  crypto_init_tls(env, t);
  return t;
}
