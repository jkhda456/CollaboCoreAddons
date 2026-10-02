/* X.509 for internalBinding('crypto') (crypto_x509.cc, the X509 parts of
 * ncrypto.cc) and the root certificate store (crypto_context.cc):
 * parseX509 / X509Certificate handles, the legacy certificate dictionary,
 * bundled / system / extra CA certificates. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#include "crypto.h"

static JSClassID x509_class_id;

/* Node's bundled Mozilla root store */
static const char *const root_certs[] = {
#define NODE_WANT_INTERNALS 1
#include "node_root_certs.h"
};

static const int kX509NameFlagsMultiline =
    ASN1_STRFLGS_ESC_2253 | ASN1_STRFLGS_ESC_CTRL | ASN1_STRFLGS_UTF8_CONVERT |
    XN_FLAG_SEP_MULTILINE | XN_FLAG_FN_SN;
static const int kX509NameFlagsRFC2253WithinUtf8JSON =
    XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB & ~ASN1_STRFLGS_ESC_CTRL;

/* ---------------------------------------------------------------------- */
/* printing (ncrypto PrintGeneralName & co) */

static JSValue bio_string(JSContext *ctx, BIO *bio) {
  char *data;
  long n = BIO_get_mem_data(bio, &data);
  JSValue r = JS_NewStringLen(ctx, data, n > 0 ? n : 0);
  BIO_free(bio);
  return r;
}

static bool is_safe_alt_name(const char *name, size_t len, bool utf8) {
  size_t i;
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c == '"' || c == '\\' || c == ',' || c == '\'')
      return false;
    if (utf8) {
      if (c < ' ' || c == 0x7f)
        return false;
    } else if (c < ' ' || c > '~') {
      return false;
    }
  }
  return true;
}

static void print_alt_name(BIO *out, const char *name, size_t len, bool utf8,
                           const char *prefix) {
  size_t j;
  if (is_safe_alt_name(name, len, utf8)) {
    if (prefix)
      BIO_printf(out, "%s:", prefix);
    BIO_write(out, name, (int)len);
    return;
  }
  BIO_write(out, "\"", 1);
  if (prefix)
    BIO_printf(out, "%s:", prefix);
  for (j = 0; j < len; j++) {
    unsigned char c = (unsigned char)name[j];
    if (c == '\\')
      BIO_write(out, "\\\\", 2);
    else if (c == '"')
      BIO_write(out, "\\\"", 2);
    else if ((c >= ' ' && c != ',' && c <= '~') || (utf8 && (c & 0x80)))
      BIO_write(out, &c, 1);
    else {
      static const char hex[] = "0123456789abcdef";
      char u[] = { '\\', 'u', '0', '0', hex[(c & 0xf0) >> 4], hex[c & 0x0f] };
      BIO_write(out, u, sizeof(u));
    }
  }
  BIO_write(out, "\"", 1);
}

static bool print_general_name(BIO *out, const GENERAL_NAME *gen) {
  switch (gen->type) {
  case GEN_DNS:
    BIO_write(out, "DNS:", 4);
    print_alt_name(out, (const char *)ASN1_STRING_get0_data(gen->d.dNSName),
                   ASN1_STRING_length(gen->d.dNSName), false, NULL);
    break;
  case GEN_EMAIL:
    BIO_write(out, "email:", 6);
    print_alt_name(out, (const char *)ASN1_STRING_get0_data(gen->d.rfc822Name),
                   ASN1_STRING_length(gen->d.rfc822Name), false, NULL);
    break;
  case GEN_URI:
    BIO_write(out, "URI:", 4);
    print_alt_name(out, (const char *)ASN1_STRING_get0_data(gen->d.uniformResourceIdentifier),
                   ASN1_STRING_length(gen->d.uniformResourceIdentifier), false, NULL);
    break;
  case GEN_DIRNAME: {
    BIO *tmp = BIO_new(BIO_s_mem());
    char *oline;
    long n;
    BIO_printf(out, "DirName:");
    if (X509_NAME_print_ex(tmp, gen->d.dirn, 0, kX509NameFlagsRFC2253WithinUtf8JSON) < 0) {
      BIO_free(tmp);
      return false;
    }
    n = BIO_get_mem_data(tmp, &oline);
    print_alt_name(out, oline, n > 0 ? n : 0, true, NULL);
    BIO_free(tmp);
    break;
  }
  case GEN_IPADD: {
    const unsigned char *b = ASN1_STRING_get0_data(gen->d.ip);
    int l = ASN1_STRING_length(gen->d.ip), j;
    BIO_printf(out, "IP Address:");
    if (l == 4) {
      BIO_printf(out, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    } else if (l == 16) {
      for (j = 0; j < 8; j++)
        BIO_printf(out, j == 0 ? "%X" : ":%X", (b[2 * j] << 8) | b[2 * j + 1]);
    } else {
      BIO_printf(out, "<invalid length=%d>", l);
    }
    break;
  }
  case GEN_RID: {
    char oline[256];
    OBJ_obj2txt(oline, sizeof(oline), gen->d.rid, 1);
    BIO_printf(out, "Registered ID:%s", oline);
    break;
  }
  case GEN_OTHERNAME: {
    const char *prefix = NULL;
    bool unicode = true;
    int vt;
    switch (OBJ_obj2nid(gen->d.otherName->type_id)) {
    case NID_id_on_SmtpUTF8Mailbox: prefix = "SmtpUTF8Mailbox"; break;
    case NID_XmppAddr: prefix = "XmppAddr"; break;
    case NID_SRVName: prefix = "SRVName"; unicode = false; break;
    case NID_ms_upn: prefix = "UPN"; break;
    case NID_NAIRealm: prefix = "NAIRealm"; break;
    }
    vt = gen->d.otherName->value->type;
    if (!prefix || (unicode && vt != V_ASN1_UTF8STRING) || (!unicode && vt != V_ASN1_IA5STRING)) {
      BIO_printf(out, "othername:<unsupported>");
    } else {
      ASN1_STRING *s = unicode ? gen->d.otherName->value->value.utf8string
                               : gen->d.otherName->value->value.ia5string;
      BIO_printf(out, "othername:");
      print_alt_name(out, (const char *)ASN1_STRING_get0_data(s), ASN1_STRING_length(s), unicode,
                     prefix);
    }
    break;
  }
  case GEN_X400:
    BIO_printf(out, "X400Name:<unsupported>");
    break;
  case GEN_EDIPARTY:
    BIO_printf(out, "EdiPartyName:<unsupported>");
    break;
  default:
    return false;
  }
  return true;
}

static JSValue subject_alt_name(JSContext *ctx, X509 *cert) {
  int idx = X509_get_ext_by_NID(cert, NID_subject_alt_name, -1);
  GENERAL_NAMES *names;
  BIO *bio;
  int i;
  if (idx < 0)
    return JS_UNDEFINED;
  names = X509V3_EXT_d2i(X509_get_ext(cert, idx));
  if (!names)
    return JS_UNDEFINED;
  bio = BIO_new(BIO_s_mem());
  for (i = 0; i < sk_GENERAL_NAME_num(names); i++) {
    if (i)
      BIO_write(bio, ", ", 2);
    if (!print_general_name(bio, sk_GENERAL_NAME_value(names, i)))
      break;
  }
  sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
  return bio_string(ctx, bio);
}

static JSValue info_access(JSContext *ctx, X509 *cert) {
  int idx = X509_get_ext_by_NID(cert, NID_info_access, -1);
  AUTHORITY_INFO_ACCESS *descs;
  BIO *bio;
  int i;
  if (idx < 0)
    return JS_UNDEFINED;
  descs = X509V3_EXT_d2i(X509_get_ext(cert, idx));
  if (!descs)
    return JS_UNDEFINED;
  bio = BIO_new(BIO_s_mem());
  for (i = 0; i < sk_ACCESS_DESCRIPTION_num(descs); i++) {
    ACCESS_DESCRIPTION *d = sk_ACCESS_DESCRIPTION_value(descs, i);
    char obj[80];
    if (i)
      BIO_write(bio, "\n", 1);
    i2t_ASN1_OBJECT(obj, sizeof(obj), d->method);
    BIO_printf(bio, "%s - ", obj);
    if (!print_general_name(bio, d->location))
      break;
  }
  sk_ACCESS_DESCRIPTION_pop_free(descs, ACCESS_DESCRIPTION_free);
  return bio_string(ctx, bio);
}

static JSValue name_string(JSContext *ctx, X509_NAME *name) {
  BIO *bio = BIO_new(BIO_s_mem());
  if (X509_NAME_print_ex(bio, name, 0, kX509NameFlagsMultiline) <= 0) {
    BIO_free(bio);
    return JS_UNDEFINED;
  }
  return bio_string(ctx, bio);
}

static JSValue name_object(JSContext *ctx, X509_NAME *name) {
  JSValue o = JS_NewObjectProto(ctx, JS_NULL);
  int i, n = X509_NAME_entry_count(name);
  for (i = 0; i < n; i++) {
    X509_NAME_ENTRY *e = X509_NAME_get_entry(name, i);
    ASN1_OBJECT *obj = X509_NAME_ENTRY_get_object(e);
    ASN1_STRING *val = X509_NAME_ENTRY_get_data(e);
    char key[80];
    unsigned char *u8;
    int nid = OBJ_obj2nid(obj), l;
    JSValue v, prev;
    if (nid != NID_undef)
      snprintf(key, sizeof(key), "%s", OBJ_nid2sn(nid));
    else
      OBJ_obj2txt(key, sizeof(key), obj, 0);
    l = ASN1_STRING_to_UTF8(&u8, val);
    if (l < 0)
      continue;
    v = JS_NewStringLen(ctx, (const char *)u8, l);
    OPENSSL_free(u8);
    prev = JS_GetPropertyStr(ctx, o, key);
    if (JS_IsUndefined(prev)) {
      JS_SetPropertyStr(ctx, o, key, v);
    } else if (JS_IsArray(prev)) {
      JSValue lenv = JS_GetPropertyStr(ctx, prev, "length");
      uint32_t len;
      JS_ToUint32(ctx, &len, lenv);
      JS_SetPropertyUint32(ctx, prev, len, v);
    } else {
      JSValue arr = JS_NewArray(ctx);
      JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, prev));
      JS_SetPropertyUint32(ctx, arr, 1, v);
      JS_SetPropertyStr(ctx, o, key, arr);
    }
    JS_FreeValue(ctx, prev);
  }
  return o;
}

static JSValue time_string(JSContext *ctx, const ASN1_TIME *t) {
  BIO *bio = BIO_new(BIO_s_mem());
  ASN1_TIME_print(bio, t);
  return bio_string(ctx, bio);
}

static double time_ms(const ASN1_TIME *t) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  ASN1_TIME_to_tm(t, &tm);
  return (double)timegm(&tm) * 1000.0;
}

static JSValue fingerprint(JSContext *ctx, X509 *cert, const EVP_MD *md) {
  unsigned char d[EVP_MAX_MD_SIZE];
  unsigned n, i;
  char out[EVP_MAX_MD_SIZE * 3];
  static const char hex[] = "0123456789ABCDEF";
  if (!X509_digest(cert, md, d, &n) || n == 0)
    return JS_UNDEFINED;
  for (i = 0; i < n; i++) {
    out[3 * i] = hex[d[i] >> 4];
    out[3 * i + 1] = hex[d[i] & 15];
    out[3 * i + 2] = ':';
  }
  out[3 * n - 1] = 0;
  return JS_NewString(ctx, out);
}

static JSValue key_usage(JSContext *ctx, X509 *cert) {
  STACK_OF(ASN1_OBJECT) *eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
  JSValue arr;
  int i;
  if (!eku)
    return JS_UNDEFINED;
  arr = JS_NewArray(ctx);
  for (i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
    char buf[256];
    if (OBJ_obj2txt(buf, sizeof(buf), sk_ASN1_OBJECT_value(eku, i), 1) >= 0)
      JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, buf));
  }
  sk_ASN1_OBJECT_pop_free(eku, ASN1_OBJECT_free);
  return arr;
}

static JSValue serial_number(JSContext *ctx, X509 *cert) {
  ASN1_INTEGER *s = X509_get_serialNumber(cert);
  BIGNUM *bn = s ? ASN1_INTEGER_to_BN(s, NULL) : NULL;
  char *hex;
  JSValue r;
  if (!bn)
    return JS_UNDEFINED;
  hex = BN_bn2hex(bn);
  BN_free(bn);
  r = JS_NewString(ctx, hex);
  OPENSSL_free(hex);
  return r;
}

static JSValue der_buffer(JSContext *ctx, X509 *cert) {
  int n = i2d_X509(cert, NULL);
  uint8_t *buf, *p;
  JSValue r;
  if (n <= 0)
    return JS_UNDEFINED;
  buf = malloc(n);
  p = buf;
  i2d_X509(cert, &p);
  r = nb_new_buffer(ctx, buf, n);
  free(buf);
  return r;
}

/* X509ToObject: the dictionary of tls getPeerCertificate() */
JSValue crypto_x509_to_object(JSContext *ctx, X509 *cert) {
  JSValue o = JS_NewObject(ctx), v;
  EVP_PKEY *pkey = X509_get0_pubkey(cert);
  ERR_set_mark();
#define SET(name, val)                                    \
  do {                                                    \
    v = (val);                                            \
    if (!JS_IsUndefined(v))                               \
      JS_SetPropertyStr(ctx, o, name, v);                 \
  } while (0)
  SET("subject", name_object(ctx, X509_get_subject_name(cert)));
  SET("issuer", name_object(ctx, X509_get_issuer_name(cert)));
  SET("subjectaltname", subject_alt_name(ctx, cert));
  SET("infoAccess", info_access(ctx, cert));
  SET("ca", JS_NewBool(ctx, X509_check_ca(cert) == 1));
  if (pkey && (EVP_PKEY_get_base_id(pkey) == EVP_PKEY_RSA ||
               EVP_PKEY_get_base_id(pkey) == EVP_PKEY_RSA_PSS)) {
    BIGNUM *n = NULL, *e = NULL;
    BIO *bio;
    int len;
    uint8_t *der, *p;
    EVP_PKEY_get_bn_param(pkey, "n", &n);
    EVP_PKEY_get_bn_param(pkey, "e", &e);
    bio = BIO_new(BIO_s_mem());
    BN_print(bio, n);
    SET("modulus", bio_string(ctx, bio));
    bio = BIO_new(BIO_s_mem());
    BIO_puts(bio, "0x");
    BN_print(bio, e);
    SET("exponent", bio_string(ctx, bio));
    {
      RSA *rsa = EVP_PKEY_get1_RSA(pkey);
      len = rsa ? i2d_RSA_PUBKEY(rsa, NULL) : 0;
      if (len > 0) {
        der = malloc(len);
        p = der;
        i2d_RSA_PUBKEY(rsa, &p);
        SET("pubkey", nb_new_buffer(ctx, der, len));
        free(der);
      }
      RSA_free(rsa);
    }
    SET("bits", JS_NewInt32(ctx, BN_num_bits(n)));
    BN_free(n);
    BN_free(e);
  } else if (pkey && EVP_PKEY_get_base_id(pkey) == EVP_PKEY_EC) {
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
    const EC_GROUP *g = EC_KEY_get0_group(ec);
    int nid = EC_GROUP_get_curve_name(g), bits = EC_GROUP_order_bits(g);
    if (EC_KEY_get0_public_key(ec))
      SET("pubkey", crypto_ec_point_to_buffer(ctx, g, EC_KEY_get0_public_key(ec),
                                              EC_KEY_get_conv_form(ec)));
    if (bits > 0)
      SET("bits", JS_NewInt32(ctx, bits));
    if (nid != NID_undef) {
      const char *sn = OBJ_nid2sn(nid), *nist = EC_curve_nid2nist(nid);
      if (sn)
        SET("asn1Curve", JS_NewString(ctx, sn));
      if (nist)
        SET("nistCurve", JS_NewString(ctx, nist));
    }
  }
  SET("valid_from", time_string(ctx, X509_get0_notBefore(cert)));
  SET("valid_to", time_string(ctx, X509_get0_notAfter(cert)));
  SET("fingerprint", fingerprint(ctx, cert, EVP_sha1()));
  SET("fingerprint256", fingerprint(ctx, cert, EVP_sha256()));
  SET("fingerprint512", fingerprint(ctx, cert, EVP_sha512()));
  SET("ext_key_usage", key_usage(ctx, cert));
  SET("serialNumber", serial_number(ctx, cert));
  SET("raw", der_buffer(ctx, cert));
#undef SET
  ERR_pop_to_mark();
  return o;
}

/* ---------------------------------------------------------------------- */
/* X509Certificate handles */

typedef struct {
  X509 *cert;
  JSValue issuer; /* X509Certificate of the issuer chain, or undefined */
} X509Handle;

static void x509_finalizer(JSRuntime *rt, JSValueConst val) {
  X509Handle *h = JS_GetOpaque(val, x509_class_id);
  if (!h)
    return;
  X509_free(h->cert);
  JS_FreeValueRT(rt, h->issuer);
  free(h);
}

static void x509_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  X509Handle *h = JS_GetOpaque(val, x509_class_id);
  if (h)
    JS_MarkValue(rt, h->issuer, mark);
}

/* takes cert; the chain (if any) is consumed as the issuer chain */
JSValue crypto_x509_new_handle(JSContext *ctx, X509 *cert, STACK_OF(X509) *chain) {
  JSValue obj = JS_NewObjectClass(ctx, x509_class_id);
  X509Handle *h;
  if (JS_IsException(obj)) {
    X509_free(cert);
    return obj;
  }
  h = calloc(1, sizeof(*h));
  h->cert = cert;
  h->issuer = JS_UNDEFINED;
  if (chain && sk_X509_num(chain)) {
    X509 *first = X509_dup(sk_X509_value(chain, 0));
    sk_X509_delete(chain, 0);
    h->issuer = crypto_x509_new_handle(ctx, first, sk_X509_num(chain) ? chain : NULL);
  }
  JS_SetOpaque(obj, h);
  return obj;
}

static X509Handle *x509_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, x509_class_id);
}

#define X509_GETTER(fname, expr)                                                    \
  static JSValue fname(JSContext *ctx, JSValueConst this_val, int argc,             \
                       JSValueConst *argv) {                                        \
    X509Handle *h = x509_of(ctx, this_val);                                         \
    JSValue r;                                                                      \
    if (!h)                                                                         \
      return JS_EXCEPTION;                                                          \
    ERR_set_mark();                                                                 \
    r = (expr);                                                                     \
    ERR_pop_to_mark();                                                              \
    return r;                                                                       \
  }

X509_GETTER(x_subject, name_string(ctx, X509_get_subject_name(h->cert)))
X509_GETTER(x_issuer, name_string(ctx, X509_get_issuer_name(h->cert)))
X509_GETTER(x_subject_alt_name, subject_alt_name(ctx, h->cert))
X509_GETTER(x_info_access, info_access(ctx, h->cert))
X509_GETTER(x_valid_from, time_string(ctx, X509_get0_notBefore(h->cert)))
X509_GETTER(x_valid_to, time_string(ctx, X509_get0_notAfter(h->cert)))
X509_GETTER(x_valid_from_date, JS_NewDate(ctx, time_ms(X509_get0_notBefore(h->cert))))
X509_GETTER(x_valid_to_date, JS_NewDate(ctx, time_ms(X509_get0_notAfter(h->cert))))
X509_GETTER(x_fingerprint, fingerprint(ctx, h->cert, EVP_sha1()))
X509_GETTER(x_fingerprint256, fingerprint(ctx, h->cert, EVP_sha256()))
X509_GETTER(x_fingerprint512, fingerprint(ctx, h->cert, EVP_sha512()))
X509_GETTER(x_key_usage, key_usage(ctx, h->cert))
X509_GETTER(x_serial_number, serial_number(ctx, h->cert))
X509_GETTER(x_raw, der_buffer(ctx, h->cert))
X509_GETTER(x_check_ca, JS_NewBool(ctx, X509_check_ca(h->cert) == 1))
X509_GETTER(x_to_legacy, crypto_x509_to_object(ctx, h->cert))

static JSValue x_pem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  BIO *bio;
  if (!h)
    return JS_EXCEPTION;
  bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, h->cert);
  return bio_string(ctx, bio);
}

static JSValue x_signature_algorithm(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  int nid;
  const char *ln;
  if (!h)
    return JS_EXCEPTION;
  nid = X509_get_signature_nid(h->cert);
  ln = nid != NID_undef ? OBJ_nid2ln(nid) : NULL;
  return ln ? JS_NewString(ctx, ln) : JS_UNDEFINED;
}

static JSValue x_signature_algorithm_oid(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  const X509_ALGOR *alg = NULL;
  const ASN1_OBJECT *obj = NULL;
  char buf[128];
  int len;
  if (!h)
    return JS_EXCEPTION;
  X509_get0_signature(NULL, &alg, h->cert);
  if (!alg)
    return JS_UNDEFINED;
  X509_ALGOR_get0(&obj, NULL, NULL, alg);
  if (!obj)
    return JS_UNDEFINED;
  len = OBJ_obj2txt(buf, sizeof(buf), obj, 1);
  if (len < 0 || len >= (int)sizeof(buf))
    return JS_UNDEFINED;
  return JS_NewStringLen(ctx, buf, len);
}

static JSValue x_public_key(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  EVP_PKEY *pk;
  KeyData *k;
  JSValue r;
  if (!h)
    return JS_EXCEPTION;
  pk = X509_get_pubkey(h->cert);
  if (!pk)
    return crypto_throw(ctx, ERR_get_error(), NULL);
  k = key_data_new_pkey(kKeyTypePublic, pk);
  r = crypto_key_handle_new(ctx, k);
  key_data_unref(k);
  return r;
}

static JSValue check_result(JSContext *ctx, int r, JSValueConst subject, char *peername) {
  JSValue ret;
  switch (r) {
  case 1:
    ret = peername ? JS_NewString(ctx, peername) : JS_DupValue(ctx, subject);
    OPENSSL_free(peername);
    return ret;
  case 0:
    return JS_UNDEFINED;
  case -2:
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE", "Invalid name");
  default:
    return crypto_throw_code(ctx, "ERR_CRYPTO_OPERATION_FAILED", NULL);
  }
}

static JSValue x_check(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                       int magic) {
  X509Handle *h = x509_of(ctx, this_val);
  size_t len;
  const char *s;
  uint32_t flags = nb_uint32(ctx, ARG(1), 0);
  char *peer = NULL;
  int r;
  if (!h)
    return JS_EXCEPTION;
  s = JS_ToCStringLen(ctx, &len, ARG(0));
  if (!s)
    return JS_EXCEPTION;
  ERR_set_mark();
  if (magic == 0)
    r = X509_check_host(h->cert, s, len, flags, &peer);
  else if (magic == 1)
    r = X509_check_email(h->cert, s, len, flags);
  else
    r = X509_check_ip_asc(h->cert, s, flags);
  ERR_pop_to_mark();
  JS_FreeCString(ctx, s);
  return check_result(ctx, r, ARG(0), peer);
}

static JSValue x_check_issued(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val), *o;
  bool r;
  if (!h)
    return JS_EXCEPTION;
  o = x509_of(ctx, ARG(0));
  if (!o)
    return JS_EXCEPTION;
  ERR_set_mark();
  r = X509_check_issued(o->cert, h->cert) == X509_V_OK;
  ERR_pop_to_mark();
  return JS_NewBool(ctx, r);
}

static JSValue x_check_private_key(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  KeyData *k;
  bool r;
  if (!h)
    return JS_EXCEPTION;
  k = crypto_key_handle_data(ARG(0));
  if (!k || !k->pkey)
    return JS_ThrowTypeError(ctx, "invalid key");
  ERR_set_mark();
  r = X509_check_private_key(h->cert, k->pkey) == 1;
  ERR_pop_to_mark();
  return JS_NewBool(ctx, r);
}

static JSValue x_verify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  KeyData *k;
  bool r;
  if (!h)
    return JS_EXCEPTION;
  k = crypto_key_handle_data(ARG(0));
  if (!k || !k->pkey)
    return JS_ThrowTypeError(ctx, "invalid key");
  ERR_set_mark();
  r = X509_verify(h->cert, k->pkey) == 1;
  ERR_pop_to_mark();
  return JS_NewBool(ctx, r);
}

static JSValue x_get_issuer_cert(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  X509Handle *h = x509_of(ctx, this_val);
  if (!h)
    return JS_EXCEPTION;
  return JS_DupValue(ctx, h->issuer);
}

static const JSCFunctionListEntry x509_proto[] = {
  JS_CFUNC_DEF("subject", 0, x_subject),
  JS_CFUNC_DEF("subjectAltName", 0, x_subject_alt_name),
  JS_CFUNC_DEF("infoAccess", 0, x_info_access),
  JS_CFUNC_DEF("issuer", 0, x_issuer),
  JS_CFUNC_DEF("validTo", 0, x_valid_to),
  JS_CFUNC_DEF("validFrom", 0, x_valid_from),
  JS_CFUNC_DEF("validToDate", 0, x_valid_to_date),
  JS_CFUNC_DEF("validFromDate", 0, x_valid_from_date),
  JS_CFUNC_DEF("signatureAlgorithm", 0, x_signature_algorithm),
  JS_CFUNC_DEF("signatureAlgorithmOid", 0, x_signature_algorithm_oid),
  JS_CFUNC_DEF("fingerprint", 0, x_fingerprint),
  JS_CFUNC_DEF("fingerprint256", 0, x_fingerprint256),
  JS_CFUNC_DEF("fingerprint512", 0, x_fingerprint512),
  JS_CFUNC_DEF("keyUsage", 0, x_key_usage),
  JS_CFUNC_DEF("serialNumber", 0, x_serial_number),
  JS_CFUNC_DEF("pem", 0, x_pem),
  JS_CFUNC_DEF("raw", 0, x_raw),
  JS_CFUNC_DEF("publicKey", 0, x_public_key),
  JS_CFUNC_DEF("checkCA", 0, x_check_ca),
  JS_CFUNC_MAGIC_DEF("checkHost", 2, x_check, 0),
  JS_CFUNC_MAGIC_DEF("checkEmail", 2, x_check, 1),
  JS_CFUNC_MAGIC_DEF("checkIP", 2, x_check, 2),
  JS_CFUNC_DEF("checkIssued", 1, x_check_issued),
  JS_CFUNC_DEF("checkPrivateKey", 1, x_check_private_key),
  JS_CFUNC_DEF("verify", 1, x_verify),
  JS_CFUNC_DEF("toLegacy", 0, x_to_legacy),
  JS_CFUNC_DEF("getIssuerCert", 0, x_get_issuer_cert),
};

static JSValue parse_x509(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t len;
  uint8_t *d = crypto_buffer_source(ctx, ARG(0), &len);
  BIO *bio;
  X509 *cert;
  if (!d)
    return JS_ThrowTypeError(ctx, "argument must be a buffer");
  ERR_set_mark();
  bio = BIO_new_mem_buf(d, (int)len);
  cert = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
  if (!cert) {
    BIO_reset(bio);
    cert = d2i_X509_bio(bio, NULL);
  }
  BIO_free(bio);
  if (!cert) {
    unsigned long e = ERR_peek_last_error();
    ERR_pop_to_mark();
    return crypto_throw(ctx, e, NULL);
  }
  ERR_pop_to_mark();
  return crypto_x509_new_handle(ctx, cert, NULL);
}

/* ---------------------------------------------------------------------- */
/* root certificates */

static uv_mutex_t roots_lock;
static uv_once_t roots_once = UV_ONCE_INIT;
static STACK_OF(X509) *bundled, *system_certs, *extra_certs, *user_roots;
static bool extra_loaded, system_loaded;

static void roots_init(void) {
  uv_mutex_init(&roots_lock);
}

static void load_certs_bio(STACK_OF(X509) *out, BIO *bio) {
  X509 *x;
  while ((x = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL)
    sk_X509_push(out, x);
  ERR_clear_error();
}

static unsigned long load_certs_file(STACK_OF(X509) *out, const char *path) {
  BIO *bio = BIO_new_file(path, "r");
  unsigned long e;
  int before = sk_X509_num(out);
  if (!bio) {
    e = ERR_get_error();
    return e ? e : 1;
  }
  load_certs_bio(out, bio);
  BIO_free(bio);
  return sk_X509_num(out) > before ? 0 : ERR_PACK(ERR_LIB_PEM, 0, PEM_R_NO_START_LINE);
}

static STACK_OF(X509) *get_bundled(void) {
  uv_once(&roots_once, roots_init);
  uv_mutex_lock(&roots_lock);
  if (!bundled) {
    size_t i;
    bundled = sk_X509_new_null();
    for (i = 0; i < countof(root_certs); i++) {
      BIO *bio = BIO_new_mem_buf(root_certs[i], (int)strlen(root_certs[i]));
      X509 *x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
      BIO_free(bio);
      if (x)
        sk_X509_push(bundled, x);
    }
  }
  uv_mutex_unlock(&roots_lock);
  return bundled;
}

static STACK_OF(X509) *get_system(void) {
  uv_once(&roots_once, roots_init);
  uv_mutex_lock(&roots_lock);
  if (!system_loaded) {
    const char *file = getenv(X509_get_default_cert_file_env());
    const char *dir = getenv(X509_get_default_cert_dir_env());
    system_loaded = true;
    system_certs = sk_X509_new_null();
    if (!file)
      file = X509_get_default_cert_file();
    if (!dir)
      dir = X509_get_default_cert_dir();
    if (file && *file)
      load_certs_file(system_certs, file);
    /* the usual Linux bundle locations when OpenSSL's defaults are empty */
    if (sk_X509_num(system_certs) == 0) {
      static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem",
      };
      size_t i;
      for (i = 0; i < countof(bundles) && sk_X509_num(system_certs) == 0; i++)
        load_certs_file(system_certs, bundles[i]);
    }
    if (dir && *dir) {
      DIR *d = opendir(dir);
      struct dirent *ent;
      while (d && (ent = readdir(d)) != NULL) {
        char path[4096];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
          load_certs_file(system_certs, path);
      }
      if (d)
        closedir(d);
    }
    ERR_clear_error();
  }
  uv_mutex_unlock(&roots_lock);
  return system_certs;
}

static STACK_OF(X509) *get_extra(void) {
  const char *file = getenv("NODE_EXTRA_CA_CERTS");
  uv_once(&roots_once, roots_init);
  uv_mutex_lock(&roots_lock);
  if (!extra_loaded) {
    extra_loaded = true;
    extra_certs = sk_X509_new_null();
    if (file && *file) {
      unsigned long e = load_certs_file(extra_certs, file);
      if (e) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        fprintf(stderr, "Warning: Ignoring extra certs from `%s`, load failed: %s\n", file, buf);
      }
    }
  }
  uv_mutex_unlock(&roots_lock);
  return extra_certs;
}

/* NODE_USE_SYSTEM_CA=1 / --use-system-ca.  In the collaboCore guest the
   system store is on by default (NODE_USE_SYSTEM_CA=0 turns it off): the
   engine terminates TLS to hosts it adds API keys for with a per-session CA
   that only /etc/ssl/cert.pem has. */
static bool use_system_ca(void) {
  const char *e = getenv("NODE_USE_SYSTEM_CA");
#ifdef __wasm__
  return !(e && !strcmp(e, "0"));
#else
  return e && !strcmp(e, "1");
#endif
}

static bool option_use_system_ca(JSContext *ctx) {
  Env *env = ctx ? env_get(ctx) : NULL;
  return use_system_ca() ||
         (env && env->options && node_option_bool(env->options, "--use-system-ca"));
}

static bool option_openssl_ca(JSContext *ctx) {
  Env *env = ctx ? env_get(ctx) : NULL;
  return env && env->options && node_option_bool(env->options, "--use-openssl-ca");
}

/* --use-openssl-ca and --use-system-ca, read from the main thread's options
   when it loads crypto (the stores are process-wide, as in Node) */
static bool root_opt_openssl_ca, root_opt_system_ca;

X509_STORE *crypto_new_root_store(void) {
  X509_STORE *store = X509_STORE_new();
  int i;
  STACK_OF(X509) *s;
  if (user_roots) {
    for (i = 0; i < sk_X509_num(user_roots); i++)
      X509_STORE_add_cert(store, sk_X509_value(user_roots, i));
    return store;
  }
  if (root_opt_openssl_ca) {
    X509_STORE_set_default_paths(store);
  } else {
    s = get_bundled();
    for (i = 0; i < sk_X509_num(s); i++)
      X509_STORE_add_cert(store, sk_X509_value(s, i));
    if (root_opt_system_ca || use_system_ca()) {
      s = get_system();
      for (i = 0; i < sk_X509_num(s); i++)
        X509_STORE_add_cert(store, sk_X509_value(s, i));
    }
  }
  s = get_extra();
  for (i = 0; i < sk_X509_num(s); i++)
    X509_STORE_add_cert(store, sk_X509_value(s, i));
  ERR_clear_error();
  return store;
}

static JSValue certs_to_pem_array(JSContext *ctx, STACK_OF(X509) *s) {
  JSValue arr = JS_NewArray(ctx);
  int i;
  for (i = 0; s && i < sk_X509_num(s); i++) {
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, sk_X509_value(s, i));
    JS_SetPropertyUint32(ctx, arr, i, bio_string(ctx, bio));
  }
  return arr;
}

JSValue crypto_root_certificates(JSContext *ctx) {
  JSValue arr = JS_NewArray(ctx);
  size_t i;
  for (i = 0; i < countof(root_certs); i++)
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, root_certs[i]));
  return arr;
}

static JSValue get_bundled_root_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  return crypto_root_certificates(ctx);
}

static JSValue get_extra_ca_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  return certs_to_pem_array(ctx, get_extra());
}

static JSValue get_system_ca_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  return certs_to_pem_array(ctx, get_system());
}

static JSValue get_user_root_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  X509_STORE *store;
  STACK_OF(X509_OBJECT) *objs;
  STACK_OF(X509) *certs;
  JSValue r;
  int i;
  if (user_roots)
    return certs_to_pem_array(ctx, user_roots);
  store = crypto_new_root_store();
  objs = X509_STORE_get0_objects(store);
  certs = sk_X509_new_null();
  for (i = 0; i < sk_X509_OBJECT_num(objs); i++) {
    X509 *x = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(objs, i));
    if (x)
      sk_X509_push(certs, x);
  }
  r = certs_to_pem_array(ctx, certs);
  sk_X509_free(certs);
  X509_STORE_free(store);
  return r;
}

/* resetRootCertStore(arrayOfPEMStrings) */
static JSValue reset_root_cert_store(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  JSValue lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  uint32_t n = 0, i;
  STACK_OF(X509) *s = sk_X509_new_null();
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue v = JS_GetPropertyUint32(ctx, ARG(0), i);
    size_t len;
    uint8_t *d = crypto_bytes_copy(ctx, v, &len);
    JS_FreeValue(ctx, v);
    if (d) {
      BIO *bio = BIO_new_mem_buf(d, (int)len);
      int before = sk_X509_num(s);
      load_certs_bio(s, bio);
      BIO_free(bio);
      free(d);
      if (sk_X509_num(s) == before) {
        sk_X509_pop_free(s, X509_free);
        return crypto_throw(ctx, ERR_get_error(), "Failed to load certificate data");
      }
    }
  }
  uv_once(&roots_once, roots_init);
  uv_mutex_lock(&roots_lock);
  if (user_roots)
    sk_X509_pop_free(user_roots, X509_free);
  user_roots = s;
  uv_mutex_unlock(&roots_lock);
  return JS_UNDEFINED;
}

static void load_ca_thread(void *arg) {
  get_bundled();
  if (getenv("NODE_EXTRA_CA_CERTS"))
    get_extra();
}

static JSValue start_loading_certificates(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  static bool started;
  uv_thread_t t;
  if (!started) {
    started = true;
    if (uv_thread_create(&t, load_ca_thread, NULL) == 0)
      uv_thread_detach(&t);
  }
  return JS_UNDEFINED;
}

static JSValue get_cert_compression_algorithms(JSContext *ctx, JSValueConst this_val, int argc,
                                               JSValueConst *argv) {
  JSValue arr = JS_NewArray(ctx);
  uint32_t n = 0;
#ifndef OPENSSL_NO_ZLIB
  JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, "zlib"));
#endif
#ifndef OPENSSL_NO_BROTLI
  JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, "brotli"));
#endif
#ifndef OPENSSL_NO_ZSTD
  JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, "zstd"));
#endif
  (void)n;
  return arr;
}

void crypto_init_x509(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  if (env->is_main_thread) {
    root_opt_openssl_ca = option_openssl_ca(ctx);
    root_opt_system_ca = option_use_system_ca(ctx);
  }
  if (x509_class_id == 0 || !JS_IsRegisteredClass(env->rt, x509_class_id)) {
    JSClassDef cd = { .class_name = "X509Certificate", .finalizer = x509_finalizer,
                      .gc_mark = x509_mark };
    JSValue proto;
    if (x509_class_id == 0)
      JS_NewClassID(env->rt, &x509_class_id);
    JS_NewClass(env->rt, x509_class_id, &cd);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, x509_proto, countof(x509_proto));
    JS_SetClassProto(ctx, x509_class_id, proto);
  }
  nb_set_method(ctx, target, "parseX509", parse_x509, 1);
  nb_set_method(ctx, target, "getBundledRootCertificates", get_bundled_root_certificates, 0);
  nb_set_method(ctx, target, "getExtraCACertificates", get_extra_ca_certificates, 0);
  nb_set_method(ctx, target, "getSystemCACertificates", get_system_ca_certificates, 0);
  nb_set_method(ctx, target, "getUserRootCertificates", get_user_root_certificates, 0);
  nb_set_method(ctx, target, "resetRootCertStore", reset_root_cert_store, 1);
  nb_set_method(ctx, target, "startLoadingCertificatesOffThread", start_loading_certificates, 0);
  nb_set_method(ctx, target, "getCertificateCompressionAlgorithms",
                get_cert_compression_algorithms, 0);
}
