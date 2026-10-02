/* internalBinding('url') (node_url.cc) over ada's C API, as Node uses ada. */
#include <stdio.h>
#include <stdlib.h>

#include "ada_c.h"
#include "node.h"

enum { kProtocol = 0, kHost = 1, kHostname = 2, kPort = 3, kUsername = 4,
       kPassword = 5, kPathname = 6, kSearch = 7, kHash = 8, kHref = 9 };

/* the components array JS shares with the binding (one per environment) */
static uint32_t *url_components_of(Env *env) {
  return env_scratch(env, "url_components", 9 * sizeof(uint32_t));
}

static void update_components(JSContext *ctx, ada_url u) {
  const ada_url_components *c = ada_get_components(u);
  uint32_t *url_components = url_components_of(env_get(ctx));
  url_components[0] = c->protocol_end;
  url_components[1] = c->username_end;
  url_components[2] = c->host_start;
  url_components[3] = c->host_end;
  url_components[4] = c->port;
  url_components[5] = c->pathname_start;
  url_components[6] = c->search_start;
  url_components[7] = c->hash_start;
  url_components[8] = ada_get_scheme_type(u);
}

static JSValue throw_invalid_url(JSContext *ctx, JSValueConst input, JSValueConst base) {
  JSValue e = JS_NewTypeError(ctx, "Invalid URL");
  JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "ERR_INVALID_URL"));
  JS_SetPropertyStr(ctx, e, "input", JS_DupValue(ctx, input));
  if (!JS_IsUndefined(base))
    JS_SetPropertyStr(ctx, e, "base", JS_DupValue(ctx, base));
  return JS_Throw(ctx, e);
}

static JSValue str(JSContext *ctx, ada_string s) {
  return node_new_utf8_string(ctx, (const uint8_t *)s.data, s.length);
}

static JSValue url_parse(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  bool raise = argc > 2 && JS_ToBool(ctx, argv[2]);
  size_t ilen, blen = 0;
  char *input = node_string_to_utf8(ctx, ARG(0), &ilen), *base = NULL;
  ada_url u;
  JSValue r;
  if (!input)
    return JS_EXCEPTION;
  if (JS_IsString(ARG(1))) {
    base = node_string_to_utf8(ctx, ARG(1), &blen);
    u = ada_parse_with_base(input, ilen, base, blen);
  } else {
    u = ada_parse(input, ilen);
  }
  free(input);
  free(base);
  if (!ada_is_valid(u)) {
    ada_free(u);
    if (raise)
      return throw_invalid_url(ctx, ARG(0), JS_IsString(ARG(1)) ? ARG(1) : JS_UNDEFINED);
    return JS_UNDEFINED;
  }
  update_components(ctx, u);
  r = str(ctx, ada_get_href(u));
  ada_free(u);
  return r;
}

static JSValue url_can_parse(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  size_t ilen, blen;
  char *input, *base;
  bool ok;
  JSValue sv = JS_ToString(ctx, ARG(0));
  input = node_string_to_utf8(ctx, sv, &ilen);
  JS_FreeValue(ctx, sv);
  if (!input)
    return JS_EXCEPTION;
  if (argc > 1 && !JS_IsUndefined(argv[1])) {
    JSValue bv = JS_ToString(ctx, argv[1]);
    base = node_string_to_utf8(ctx, bv, &blen);
    JS_FreeValue(ctx, bv);
    ok = base && ada_can_parse_with_base(input, ilen, base, blen);
    free(base);
  } else {
    ok = ada_can_parse(input, ilen);
  }
  free(input);
  return JS_NewBool(ctx, ok);
}

static JSValue url_update(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  size_t hlen, vlen;
  char *href = node_string_to_utf8(ctx, ARG(0), &hlen);
  uint32_t action = nb_uint32(ctx, ARG(1), 0);
  char *val = node_string_to_utf8(ctx, ARG(2), &vlen);
  ada_url u;
  bool ok = true;
  JSValue r;
  if (!href || !val) {
    free(href);
    free(val);
    return JS_EXCEPTION;
  }
  u = ada_parse(href, hlen);
  free(href);
  if (!ada_is_valid(u)) {
    ada_free(u);
    free(val);
    return JS_FALSE;
  }
  switch (action) {
  case kPathname: ok = ada_set_pathname(u, val, vlen); break;
  case kHash: ada_set_hash(u, val, vlen); break;
  case kHost: ok = ada_set_host(u, val, vlen); break;
  case kHostname: ok = ada_set_hostname(u, val, vlen); break;
  case kHref: ok = ada_set_href(u, val, vlen); break;
  case kPassword: ok = ada_set_password(u, val, vlen); break;
  case kPort: ok = ada_set_port(u, val, vlen); break;
  case kProtocol: ok = ada_set_protocol(u, val, vlen); break;
  case kSearch: ada_set_search(u, val, vlen); break;
  case kUsername: ok = ada_set_username(u, val, vlen); break;
  default: ok = false; break;
  }
  free(val);
  if (!ok) {
    ada_free(u);
    return JS_FALSE;
  }
  update_components(ctx, u);
  r = str(ctx, ada_get_href(u));
  ada_free(u);
  return r;
}

static JSValue url_get_origin(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  size_t len;
  char *input = node_string_to_utf8(ctx, ARG(0), &len);
  ada_url u = ada_parse(input, len);
  ada_owned_string o;
  JSValue r;
  free(input);
  if (!ada_is_valid(u)) {
    ada_free(u);
    return node_throw_type_error(ctx, "ERR_INVALID_URL", "Invalid URL");
  }
  o = ada_get_origin(u);
  r = node_new_utf8_string(ctx, (const uint8_t *)o.data, o.length);
  ada_free_owned_string(o);
  ada_free(u);
  return r;
}

static JSValue hostname_of(JSContext *ctx, JSValueConst input, bool unicode) {
  size_t len;
  char *s = node_string_to_utf8(ctx, input, &len);
  ada_url u;
  JSValue r;
  if (!s)
    return JS_EXCEPTION;
  if (len == 0) {
    free(s);
    return JS_NewString(ctx, "");
  }
  u = ada_parse("ws://x", 6);
  if (!ada_set_hostname(u, s, len)) {
    free(s);
    ada_free(u);
    return JS_NewString(ctx, "");
  }
  free(s);
  if (unicode) {
    ada_string h = ada_get_hostname(u);
    ada_owned_string o = ada_idna_to_unicode(h.data, h.length);
    r = node_new_utf8_string(ctx, (const uint8_t *)o.data, o.length);
    ada_free_owned_string(o);
  } else {
    r = str(ctx, ada_get_hostname(u));
  }
  ada_free(u);
  return r;
}

JSValue node_url_domain_to_ascii(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  return hostname_of(ctx, ARG(0), false);
}

JSValue node_url_domain_to_unicode(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  return hostname_of(ctx, ARG(0), true);
}

/* url.format(URL, { fragment, unicode, search, auth }) */
static JSValue url_format(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  size_t len;
  char *href = node_string_to_utf8(ctx, ARG(0), &len);
  bool hash = JS_ToBool(ctx, ARG(1)), unicode = JS_ToBool(ctx, ARG(2));
  bool search = JS_ToBool(ctx, ARG(3)), auth = JS_ToBool(ctx, ARG(4));
  ada_url u;
  JSValue r;
  if (!href)
    return JS_EXCEPTION;
  u = ada_parse(href, len);
  free(href);
  if (!ada_is_valid(u)) {
    ada_free(u);
    return JS_DupValue(ctx, ARG(0));
  }
  if (!hash)
    ada_clear_hash(u);
  if (!search)
    ada_clear_search(u);
  if (!auth) {
    ada_set_username(u, "", 0);
    ada_set_password(u, "", 0);
  }
  if (unicode && ada_has_hostname(u)) {
    /* href with the host in Unicode: splice it in by the components */
    ada_string h = ada_get_href(u);
    const ada_url_components *c = ada_get_components(u);
    ada_string hn = ada_get_hostname(u);
    ada_owned_string uni = ada_idna_to_unicode(hn.data, hn.length);
    size_t host_start = c->host_start, host_end = c->host_end;
    char *buf;
    size_t n = 0;
    /* host_start points at '@' or the second '/' */
    if (host_start < h.length && (h.data[host_start] == '@' || h.data[host_start] == '/'))
      host_start++;
    buf = malloc(h.length + uni.length + 1);
    memcpy(buf, h.data, host_start);
    n = host_start;
    memcpy(buf + n, uni.data, uni.length);
    n += uni.length;
    memcpy(buf + n, h.data + host_end, h.length - host_end);
    n += h.length - host_end;
    r = node_new_utf8_string(ctx, (const uint8_t *)buf, n);
    free(buf);
    ada_free_owned_string(uni);
  } else {
    r = str(ctx, ada_get_href(u));
  }
  ada_free(u);
  return r;
}

static const char *encode_path_char(unsigned char c) {
  switch (c) {
  case '\0': return "%00";
  case '\t': return "%09";
  case '\n': return "%0A";
  case '\r': return "%0D";
  case ' ': return "%20";
  case '"': return "%22";
  case '#': return "%23";
  case '%': return "%25";
  case '?': return "%3F";
  case '[': return "%5B";
  case '\\': return "%5C";
  case ']': return "%5D";
  case '^': return "%5E";
  case '|': return "%7C";
  case '~': return "%7E";
  default: return NULL;
  }
}

static JSValue url_path_to_file_url(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  size_t len, i, n = 0;
  char *path = node_string_to_utf8(ctx, ARG(0), &len), *enc;
  bool windows = JS_ToBool(ctx, ARG(1));
  ada_url u;
  JSValue r;
  if (!path)
    return JS_EXCEPTION;
  enc = malloc(len * 3 + 8);
  memcpy(enc, "file://", 7);
  n = 7;
  for (i = 0; i < len; i++) {
    unsigned char c = path[i];
    const char *e = c <= '~' ? encode_path_char(c) : NULL;
    if (windows && c == '\\') {
      enc[n++] = '/';
    } else if (e) {
      memcpy(enc + n, e, 3);
      n += 3;
    } else {
      enc[n++] = c;
    }
  }
  u = ada_parse(enc, n);
  free(enc);
  if (!ada_is_valid(u)) {
    ada_free(u);
    free(path);
    return throw_invalid_url(ctx, ARG(0), JS_UNDEFINED);
  }
  if (windows && argc > 2 && JS_IsString(argv[2])) {
    size_t hlen;
    char *host = node_string_to_utf8(ctx, argv[2], &hlen);
    if (!ada_set_hostname(u, host, hlen)) {
      free(host);
      ada_free(u);
      free(path);
      return throw_invalid_url(ctx, ARG(0), argv[2]);
    }
    free(host);
  }
  free(path);
  update_components(ctx, u);
  r = str(ctx, ada_get_href(u));
  ada_free(u);
  return r;
}

JSValue binding_init_url(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "parse", url_parse, 3);
  nb_set_method(ctx, t, "canParse", url_can_parse, 2);
  nb_set_method(ctx, t, "update", url_update, 3);
  nb_set_method(ctx, t, "getOrigin", url_get_origin, 1);
  nb_set_method(ctx, t, "domainToASCII", node_url_domain_to_ascii, 1);
  nb_set_method(ctx, t, "domainToUnicode", node_url_domain_to_unicode, 1);
  nb_set_method(ctx, t, "format", url_format, 5);
  nb_set_method(ctx, t, "pathToFileURL", url_path_to_file_url, 3);
  nb_set(ctx, t, "urlComponents",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, url_components_of(env), 9, 4));
  return t;
}
