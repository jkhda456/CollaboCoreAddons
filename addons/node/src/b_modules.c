/* internalBinding('modules') (node_modules.cc): package.json reading for the
 * module loaders, with the same cached, field-by-field result Node gives. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ada_c.h"
#include "node.h"

static char *read_file(const char *path, size_t *plen, int *perr) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  char *buf;
  size_t n = 0;
  if (fd < 0) {
    *perr = -errno;
    return NULL;
  }
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    close(fd);
    *perr = UV_EISDIR;
    return NULL;
  }
  buf = malloc(st.st_size + 1);
  for (;;) {
    ssize_t r = read(fd, buf + n, st.st_size - n);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      *perr = -errno;
      free(buf);
      close(fd);
      return NULL;
    }
    if (r == 0)
      break;
    n += r;
    if (n == (size_t)st.st_size)
      break;
  }
  close(fd);
  buf[n] = 0;
  *plen = n;
  return buf;
}

static JSValue throw_invalid_config(JSContext *ctx, const char *path, const char *base,
                                    const char *specifier) {
  if (base && specifier) {
    const char *b = base;
    if (!strncmp(b, "file://", 7))
      b += 7;
    return node_throw_error(ctx, "ERR_INVALID_PACKAGE_CONFIG",
                            "Invalid package config %s while importing \"%s\" from %s.",
                            path, specifier, b);
  }
  return node_throw_error(ctx, "ERR_INVALID_PACKAGE_CONFIG", "Invalid package config %s.", path);
}

/* JSON text of a parsed value */
static JSValue to_json(JSContext *ctx, JSValueConst v) {
  return JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
}

/* [name, main, type, imports, exports, path] or undefined (no file);
   JS_EXCEPTION for a broken one */
static JSValue get_package_json(Env *env, const char *path, const char *base,
                                const char *specifier) {
  JSContext *ctx = env->ctx;
  JSValue cache = env_binding_data(env, "modules"), cached, obj, arr, v;
  size_t len;
  int err = 0;
  char *src;
  const char *json;

  cached = JS_GetPropertyStr(ctx, cache, path);
  if (!JS_IsUndefined(cached)) {
    JS_FreeValue(ctx, cache);
    if (JS_IsNull(cached))
      return JS_UNDEFINED;
    return cached;
  }
  src = read_file(path, &len, &err);
  if (!src) {
    if (err != UV_ENOENT && err != UV_ENOTDIR && err != UV_EISDIR) {
      JS_FreeValue(ctx, cache);
      return node_throw_error(ctx, "ERR_INVALID_PACKAGE_CONFIG",
                              "Cannot read package config %s: %s.", path, uv_strerror(err));
    }
    JS_SetPropertyStr(ctx, cache, path, JS_NULL);
    JS_FreeValue(ctx, cache);
    return JS_UNDEFINED;
  }
  json = src;
  if (len >= 3 && (uint8_t)json[0] == 0xef && (uint8_t)json[1] == 0xbb && (uint8_t)json[2] == 0xbf) {
    json += 3;
    len -= 3;
  }
  obj = JS_ParseJSON(ctx, json, len, path);
  free(src);
  if (JS_IsException(obj) || !JS_IsObject(obj) || JS_IsArray(obj)) {
    if (JS_IsException(obj))
      JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, cache);
    return throw_invalid_config(ctx, path, base, specifier);
  }
  arr = JS_NewArray(ctx);
  v = JS_GetPropertyStr(ctx, obj, "name");
  if (!JS_IsUndefined(v) && !JS_IsString(v)) {
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, cache);
    return throw_invalid_config(ctx, path, base, specifier);
  }
  JS_SetPropertyUint32(ctx, arr, 0, v);
  v = JS_GetPropertyStr(ctx, obj, "main");
  JS_SetPropertyUint32(ctx, arr, 1, JS_IsString(v) ? v : (JS_FreeValue(ctx, v), JS_UNDEFINED));
  v = JS_GetPropertyStr(ctx, obj, "type");
  {
    const char *t = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    const char *type = "none";
    if (t && (!strcmp(t, "commonjs") || !strcmp(t, "module")))
      type = t;
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewString(ctx, type));
    JS_FreeCString(ctx, t);
    JS_FreeValue(ctx, v);
  }
  {
    static const char *const fields[] = { "imports", "exports" };
    int i;
    for (i = 0; i < 2; i++) {
      JSValue f = JS_GetPropertyStr(ctx, obj, fields[i]), out = JS_UNDEFINED;
      if (JS_IsString(f))
        out = JS_DupValue(ctx, f);
      else if (JS_IsObject(f))
        out = to_json(ctx, f);
      JS_FreeValue(ctx, f);
      JS_SetPropertyUint32(ctx, arr, 3 + i, out);
    }
  }
  JS_SetPropertyUint32(ctx, arr, 5, JS_NewString(ctx, path));
  JS_FreeValue(ctx, obj);
  JS_SetPropertyStr(ctx, cache, path, JS_DupValue(ctx, arr));
  JS_FreeValue(ctx, cache);
  return arr;
}

static JSValue m_read_package_json(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  char *path = node_string_to_utf8(ctx, ARG(0), NULL), *base = NULL, *spec = NULL;
  bool is_esm = JS_ToBool(ctx, ARG(1));
  JSValue r;
  if (!path)
    return JS_EXCEPTION;
  if (is_esm) {
    if (JS_IsString(ARG(2)))
      base = node_string_to_utf8(ctx, ARG(2), NULL);
    if (JS_IsString(ARG(3)))
      spec = node_string_to_utf8(ctx, ARG(3), NULL);
  }
  r = get_package_json(env, path, base, spec);
  free(path);
  free(base);
  free(spec);
  return r;
}

/* the nearest package.json above check_path, stopping at node_modules */
static JSValue traverse_parent(Env *env, const char *check_path) {
  char *cur = strdup(check_path);
  JSValue r = JS_UNDEFINED;
  for (;;) {
    char *slash = strrchr(cur, '/'), pj[4096];
    const char *last;
    if (!slash)
      break;
    if (slash == cur) {
      /* reached the root: Node stops when parent_path() == path */
      break;
    }
    *slash = 0;
    last = strrchr(cur, '/');
    last = last ? last + 1 : cur;
    if (!strcmp(last, "node_modules"))
      break;
    snprintf(pj, sizeof(pj), "%s/package.json", cur);
    r = get_package_json(env, pj, NULL, NULL);
    if (!JS_IsUndefined(r))
      break;
  }
  free(cur);
  return r;
}

static JSValue m_get_nearest_parent_package_json(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  char *p = node_string_to_utf8(ctx, ARG(0), NULL);
  JSValue r;
  size_t n;
  if (!p)
    return JS_EXCEPTION;
  /* a trailing slash means the directory itself */
  n = strlen(p);
  if (n > 1 && p[n - 1] == '/') {
    p = realloc(p, n + 2);
    strcat(p, "x");
  }
  r = traverse_parent(env_get(ctx), p);
  free(p);
  return r;
}

static JSValue m_get_nearest_parent_package_json_type(JSContext *ctx, JSValueConst this_val,
                                                      int argc, JSValueConst *argv) {
  JSValue r = m_get_nearest_parent_package_json(ctx, this_val, argc, argv), t;
  if (!JS_IsArray(r))
    return r;
  t = JS_GetPropertyUint32(ctx, r, 2);
  JS_FreeValue(ctx, r);
  return t;
}

static char *file_url_to_path(const char *href, size_t len) {
  ada_url u = ada_parse(href, len);
  ada_string p;
  char *out, *o;
  size_t i;
  if (!ada_is_valid(u)) {
    ada_free(u);
    return NULL;
  }
  p = ada_get_pathname(u);
  out = malloc(p.length + 1);
  o = out;
  for (i = 0; i < p.length; i++) {
    if (p.data[i] == '%' && i + 2 < p.length) {
      char hex[3] = { p.data[i + 1], p.data[i + 2], 0 };
      *o++ = (char)strtol(hex, NULL, 16);
      i += 2;
    } else {
      *o++ = p.data[i];
    }
  }
  *o = 0;
  ada_free(u);
  return out;
}

static JSValue package_scope(JSContext *ctx, JSValueConst resolved_v, bool only_type) {
  Env *env = env_get(ctx);
  size_t len;
  char *resolved = node_string_to_utf8(ctx, resolved_v, &len);
  ada_url base, pj;
  JSValue r = JS_UNDEFINED;
  if (!resolved)
    return JS_EXCEPTION;
  base = ada_parse(resolved, len);
  if (!ada_is_valid(base)) {
    ada_free(base);
    free(resolved);
    return node_throw_type_error(ctx, "ERR_INVALID_URL", "Invalid URL");
  }
  {
    ada_string h = ada_get_href(base);
    pj = ada_parse_with_base("./package.json", 14, h.data, h.length);
  }
  ada_free(base);
  for (;;) {
    ada_string pn = ada_get_pathname(pj), h;
    char *path, *last_pathname;
    ada_url next;
    if (pn.length >= 26 && !memcmp(pn.data + pn.length - 26, "/node_modules/package.json", 26))
      break;
    h = ada_get_href(pj);
    path = file_url_to_path(h.data, h.length);
    if (!path)
      break;
    r = get_package_json(env, path, NULL, resolved);
    free(path);
    if (JS_IsException(r)) {
      ada_free(pj);
      free(resolved);
      return r;
    }
    if (!JS_IsUndefined(r)) {
      ada_free(pj);
      free(resolved);
      if (only_type) {
        JSValue t = JS_GetPropertyUint32(ctx, r, 2);
        JS_FreeValue(ctx, r);
        return t;
      }
      return r;
    }
    last_pathname = strndup(pn.data, pn.length);
    next = ada_parse_with_base("../package.json", 15, h.data, h.length);
    ada_free(pj);
    pj = next;
    pn = ada_get_pathname(pj);
    if (pn.length == strlen(last_pathname) && !memcmp(pn.data, last_pathname, pn.length)) {
      free(last_pathname);
      break;
    }
    free(last_pathname);
  }
  free(resolved);
  if (only_type) {
    ada_free(pj);
    return JS_UNDEFINED;
  }
  {
    ada_string h = ada_get_href(pj);
    char *path = file_url_to_path(h.data, h.length);
    r = path ? JS_NewString(ctx, path) : JS_UNDEFINED;
    free(path);
  }
  ada_free(pj);
  return r;
}

static JSValue m_get_package_scope_config(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  return package_scope(ctx, ARG(0), false);
}

static JSValue m_get_package_type(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  return package_scope(ctx, ARG(0), true);
}

/* enableCompileCache(directory, portable): [status, message, directory] */
static JSValue m_enable_compile_cache(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  JSValue arr = JS_NewArray(ctx);
  char *dir = node_string_to_utf8(ctx, ARG(0), NULL), *message, *directory;
  int status;
  if (!dir) {
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
  }
  status = node_compile_cache_enable(dir, &message, &directory);
  free(dir);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, status));
  JS_SetPropertyUint32(ctx, arr, 1, message ? JS_NewString(ctx, message) : JS_UNDEFINED);
  JS_SetPropertyUint32(ctx, arr, 2, directory ? JS_NewString(ctx, directory) : JS_UNDEFINED);
  free(message);
  free(directory);
  return arr;
}

static JSValue m_get_compile_cache_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  const char *d = node_compile_cache_dir();
  return JS_NewString(ctx, d ? d : "");
}

/* setLazyPathHelpers(meta, url): import.meta.dirname / filename */
static JSValue m_set_lazy_path_helpers(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  size_t len;
  const char *href = JS_ToCStringLen(ctx, &len, ARG(1));
  char *path, *slash;
  if (!href)
    return JS_EXCEPTION;
  path = file_url_to_path(href, len);
  JS_FreeCString(ctx, href);
  if (!path)
    return JS_UNDEFINED;
  JS_DefinePropertyValueStr(ctx, ARG(0), "filename", JS_NewString(ctx, path), JS_PROP_C_W_E);
  slash = strrchr(path, '/');
  if (slash)
    *slash = 0;
  JS_DefinePropertyValueStr(ctx, ARG(0), "dirname", JS_NewString(ctx, path), JS_PROP_C_W_E);
  free(path);
  return JS_UNDEFINED;
}

static JSValue m_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

JSValue binding_init_modules(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), st = JS_NewArray(ctx), cct, mf;
  static const char *const statuses[] = { "FAILED", "ENABLED", "ALREADY_ENABLED", "DISABLED" };
  static const char *const code_types[] = { "kCommonJS", "kESM", "kStrippedTypeScript",
                                            "kTransformedTypeScript",
                                            "kTransformedTypeScriptWithSourceMaps" };
  int i;
  nb_set_method(ctx, t, "readPackageJSON", m_read_package_json, 4);
  nb_set_method(ctx, t, "getNearestParentPackageJSON", m_get_nearest_parent_package_json, 1);
  nb_set_method(ctx, t, "getNearestParentPackageJSONType",
                m_get_nearest_parent_package_json_type, 1);
  nb_set_method(ctx, t, "getPackageScopeConfig", m_get_package_scope_config, 1);
  nb_set_method(ctx, t, "getPackageType", m_get_package_type, 1);
  nb_set_method(ctx, t, "enableCompileCache", m_enable_compile_cache, 2);
  nb_set_method(ctx, t, "getCompileCacheDir", m_get_compile_cache_dir, 0);
  nb_set_method(ctx, t, "flushCompileCache", m_noop, 1);
  nb_set_method(ctx, t, "getCompileCacheEntry", m_noop, 3);
  nb_set_method(ctx, t, "saveCompileCacheEntry", m_noop, 2);
  nb_set_method(ctx, t, "setLazyPathHelpers", m_set_lazy_path_helpers, 2);
  for (i = 0; i < 4; i++)
    JS_SetPropertyUint32(ctx, st, i, JS_NewString(ctx, statuses[i]));
  nb_set(ctx, t, "compileCacheStatus", st);
  cct = JS_NewObjectProto(ctx, JS_NULL);
  for (i = 0; i < 5; i++)
    nb_set_int(ctx, cct, code_types[i], i);
  nb_set(ctx, t, "cachedCodeTypes", cct);
  mf = JS_NewObjectProto(ctx, JS_NULL);
  nb_set_int(ctx, mf, "kCommonJS", 0);
  nb_set_int(ctx, mf, "kModule", 1);
  nb_set(ctx, t, "moduleFormats", mf);
  return t;
}
