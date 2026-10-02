/* internalBinding('fs') (node_file.cc) and ('fs_dir') (node_dir.cc).
 *
 * Each call is synchronous (throws a UVException), asynchronous with an
 * FSReqCallback (req.oncomplete(err, result)) or with the kUsePromises
 * symbol (returns a promise), as in Node. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "node.h"

#define kFsStatsFieldsNumber 18
#define kFsStatFsFieldsNumber 8  /* type bsize frsize blocks bfree bavail files ffree */

typedef struct FsBindingData {
  double stat_values[2 * kFsStatsFieldsNumber];
  int64_t bigint_stat_values[2 * kFsStatsFieldsNumber];
  double statfs_values[kFsStatFsFieldsNumber];
  int64_t bigint_statfs_values[kFsStatFsFieldsNumber];
  JSValue stat_values_v, bigint_stat_values_v, statfs_values_v, bigint_statfs_values_v;
} FsBindingData;

/* per environment (each worker has its own); the arrays' JS values are owned by
   the environment's binding data, so these are borrowed */
static FsBindingData *fsd_of(Env *env) {
  FsBindingData *d = env->fsd;
  JSContext *ctx = env->ctx;
  JSValue holder;
  if (d)
    return d;
  d = env->fsd = calloc(1, sizeof(*d));
  holder = env_binding_data(env, "fs");
  d->stat_values_v = nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, d->stat_values,
                                        2 * kFsStatsFieldsNumber, 8);
  d->bigint_stat_values_v = nb_new_typed_array(ctx, JS_TYPED_ARRAY_BIG_INT64,
                                               d->bigint_stat_values,
                                               2 * kFsStatsFieldsNumber, 8);
  d->statfs_values_v = nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, d->statfs_values,
                                          kFsStatFsFieldsNumber, 8);
  d->bigint_statfs_values_v = nb_new_typed_array(ctx, JS_TYPED_ARRAY_BIG_INT64,
                                                 d->bigint_statfs_values,
                                                 kFsStatFsFieldsNumber, 8);
  JS_SetPropertyStr(ctx, holder, "statValues", d->stat_values_v);
  JS_SetPropertyStr(ctx, holder, "bigintStatValues", d->bigint_stat_values_v);
  JS_SetPropertyStr(ctx, holder, "statFsValues", d->statfs_values_v);
  JS_SetPropertyStr(ctx, holder, "bigintStatFsValues", d->bigint_statfs_values_v);
  JS_FreeValue(ctx, holder);
  return d;
}

static JSClassID req_callback_class_id, file_handle_class_id, fs_dir_class_id;
static JSClassID stat_watcher_class_id;

/* ---------------------------------------------------------------------- */
/* paths and values */

/* a path argument: string, Buffer or Uint8Array (NUL-terminated, malloc'ed) */
static char *path_arg(JSContext *ctx, JSValueConst v) {
  size_t len;
  uint8_t *b;
  char *r;
  if (JS_IsString(v))
    return node_string_to_utf8(ctx, v, NULL);
  b = nb_buffer_data(ctx, v, &len);
  if (b) {
    r = malloc(len + 1);
    memcpy(r, b, len);
    r[len] = 0;
    return r;
  }
  {
    JSValue s = JS_ToString(ctx, v);
    if (JS_IsException(s))
      return NULL;
    r = node_string_to_utf8(ctx, s, NULL);
    JS_FreeValue(ctx, s);
    return r;
  }
}

static int64_t offset_arg(JSContext *ctx, JSValueConst v) {
  int64_t n;
  if (JS_IsBigInt(v)) {
    if (JS_ToBigInt64(ctx, &n, v) < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return -1;
    }
    return n;
  }
  if (JS_IsNumber(v)) {
    double d;
    JS_ToFloat64(ctx, &d, v);
    if (d != d || d < -1 || d > 9007199254740991.0)
      return -1;
    return (int64_t)d;
  }
  return -1;
}

static int fill_stats(double *d, int64_t *b, const uv_stat_t *s, bool bigint) {
  int64_t v[kFsStatsFieldsNumber] = {
    (int64_t)s->st_dev, (int64_t)s->st_mode, (int64_t)s->st_nlink, (int64_t)s->st_uid,
    (int64_t)s->st_gid, (int64_t)s->st_rdev, (int64_t)s->st_blksize, (int64_t)s->st_ino,
    (int64_t)s->st_size, (int64_t)s->st_blocks,
    (int64_t)s->st_atim.tv_sec, (int64_t)s->st_atim.tv_nsec,
    (int64_t)s->st_mtim.tv_sec, (int64_t)s->st_mtim.tv_nsec,
    (int64_t)s->st_ctim.tv_sec, (int64_t)s->st_ctim.tv_nsec,
    (int64_t)s->st_birthtim.tv_sec, (int64_t)s->st_birthtim.tv_nsec,
  };
  int i;
  for (i = 0; i < kFsStatsFieldsNumber; i++) {
    if (bigint)
      b[i] = v[i];
    else
      d[i] = (double)v[i];
  }
  return 0;
}

/* the binding's shared stats array, filled (FillGlobalStatsArray) */
static JSValue global_stats(JSContext *ctx, const uv_stat_t *s, bool bigint, int second) {
  FsBindingData *fsd = fsd_of(env_get(ctx));
  int off = second ? kFsStatsFieldsNumber : 0;
  fill_stats(fsd->stat_values + off, fsd->bigint_stat_values + off, s, bigint);
  return JS_DupValue(ctx, bigint ? fsd->bigint_stat_values_v : fsd->stat_values_v);
}

int node_fs_fill_stats(Env *env, const uv_stat_t *s, bool bigint, int offset) {
  FsBindingData *fsd = fsd_of(env);
  fill_stats(fsd->stat_values + offset, fsd->bigint_stat_values + offset, s, bigint);
  return 0;
}

JSValue node_fs_stats_array(Env *env, bool bigint) {
  FsBindingData *fsd = fsd_of(env);
  return JS_DupValue(env->ctx, bigint ? fsd->bigint_stat_values_v : fsd->stat_values_v);
}

/* a fresh stats array (promises get their own) */
static JSValue new_stats(JSContext *ctx, const uv_stat_t *s, bool bigint) {
  JSValue arr;
  if (bigint) {
    int64_t *b = nb_alloc_shared_array(ctx, JS_TYPED_ARRAY_BIG_INT64, kFsStatsFieldsNumber, 8, &arr);
    fill_stats(NULL, b, s, true);
  } else {
    double *d;
    JSValue len = JS_NewInt32(ctx, kFsStatsFieldsNumber);
    arr = JS_NewTypedArray(ctx, 1, (JSValueConst *)&len, JS_TYPED_ARRAY_FLOAT64);
    {
      size_t n;
      d = (double *)nb_buffer_data(ctx, arr, &n);
      fill_stats(d, NULL, s, false);
    }
  }
  return arr;
}

static JSValue statfs_values(JSContext *ctx, const uv_statfs_t *s, bool bigint, bool fresh) {
  int64_t v[kFsStatFsFieldsNumber] = {
    (int64_t)s->f_type, (int64_t)s->f_bsize, (int64_t)s->f_frsize, (int64_t)s->f_blocks,
    (int64_t)s->f_bfree, (int64_t)s->f_bavail, (int64_t)s->f_files, (int64_t)s->f_ffree,
  };
  int i;
  FsBindingData *fsd = fsd_of(env_get(ctx));
  if (fresh) {
    JSValue arr = JS_NewArray(ctx);
    for (i = 0; i < kFsStatFsFieldsNumber; i++)
      JS_SetPropertyUint32(ctx, arr, i, bigint ? JS_NewBigInt64(ctx, v[i])
                                               : JS_NewFloat64(ctx, (double)v[i]));
    return arr;
  }
  for (i = 0; i < kFsStatFsFieldsNumber; i++) {
    if (bigint)
      fsd->bigint_statfs_values[i] = v[i];
    else
      fsd->statfs_values[i] = (double)v[i];
  }
  return JS_DupValue(ctx, bigint ? fsd->bigint_statfs_values_v : fsd->statfs_values_v);
}

/* ---------------------------------------------------------------------- */
/* requests */

typedef struct FsReq FsReq;
typedef JSValue (*AfterFn)(FsReq *r, JSContext *ctx); /* the result, or JS_EXCEPTION */

typedef struct ReqCallback {
  AsyncWrap aw;
  bool bigint;
} ReqCallback;

struct FsReq {
  uv_fs_t req;
  Env *env;
  JSValue target;      /* FSReqCallback object, or JS_UNDEFINED for promises */
  JSValue resolve, reject;
  AsyncWrap promise_aw;
  bool is_promise;
  bool bigint;
  int encoding;
  bool with_types;
  const char *syscall;
  char *path, *dest;
  JSValue keep[2];      /* values kept alive while the request runs */
  uv_buf_t *iovs;
  char *buf;            /* owned data (writeString) */
  AfterFn after;
  char *first_path;     /* mkdirp */
  int fd;               /* openFileHandle */
  bool no_throw_noent;  /* stat(..., throwIfNoEntry = false): ENOENT gives undefined */
};

static void fsreq_free(FsReq *r) {
  JSContext *ctx = r->env->ctx;
  uv_fs_req_cleanup(&r->req);
  JS_FreeValue(ctx, r->target);
  JS_FreeValue(ctx, r->resolve);
  JS_FreeValue(ctx, r->reject);
  JS_FreeValue(ctx, r->keep[0]);
  JS_FreeValue(ctx, r->keep[1]);
  if (r->is_promise)
    async_wrap_destroy(&r->promise_aw);
  free(r->path);
  free(r->dest);
  free(r->iovs);
  free(r->buf);
  free(r->first_path);
  free(r);
}

static JSValue fs_exception(JSContext *ctx, int err, const char *syscall, const char *path,
                            const char *dest) {
  return node_uv_exception(ctx, err, syscall, NULL, path, dest);
}

/* calls back, resolves or rejects with the request's outcome */
static void fsreq_complete(FsReq *r) {
  Env *env = r->env;
  JSContext *ctx = env->ctx;
  JSValue result = JS_UNDEFINED, err = JS_NULL;
  int res = (int)r->req.result;
  if (res < 0 && r->no_throw_noent && (res == UV_ENOENT || res == UV_ENOTDIR)) {
    /* AfterStatNoThrowIfNoEntry: resolved with undefined */
  } else if (res < 0) {
    err = fs_exception(ctx, res, r->syscall, r->path, r->dest);
  } else if (r->after) {
    result = r->after(r, ctx);
    if (JS_IsException(result)) {
      err = JS_GetException(ctx);
      result = JS_UNDEFINED;
    }
  }
  if (r->is_promise) {
    bool failed = !JS_IsNull(err);
    JSValueConst fn = failed ? r->reject : r->resolve;
    JSValue arg = failed ? err : result, ret;
    ret = node_make_callback(env, r->promise_aw.object, JS_UNDEFINED, fn, 1,
                             (JSValueConst *)&arg, r->promise_aw.async_id,
                             r->promise_aw.trigger_async_id, r->promise_aw.context_frame);
    JS_FreeValue(ctx, ret);
  } else {
    ReqCallback *rc = JS_GetOpaque(r->target, req_callback_class_id);
    JSValue args[2], ret;
    int n = 1;
    args[0] = err;
    if (JS_IsNull(err) && !JS_IsUndefined(result)) {
      args[1] = result;
      n = 2;
    } else if (JS_IsNull(err) && r->after) {
      args[1] = JS_UNDEFINED;
      n = 2;
    }
    if (rc) {
      ret = async_wrap_make_callback_name(&rc->aw, "oncomplete", n, (JSValueConst *)args);
      JS_FreeValue(ctx, ret);
    }
  }
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, result);
}

static void after_cb(uv_fs_t *req) {
  FsReq *r = req->data;
  fsreq_complete(r);
  fsreq_free(r);
}

/* the request for an async call, or NULL for a synchronous one; *ret gets
   the promise for kUsePromises */
static FsReq *get_req(JSContext *ctx, JSValueConst reqv, const char *syscall, JSValue *ret,
                      bool *is_async) {
  Env *env = env_get(ctx);
  FsReq *r;
  *is_async = false;
  *ret = JS_UNDEFINED;
  if (JS_IsUndefined(reqv))
    return NULL;
  r = calloc(1, sizeof(*r));
  r->env = env;
  r->syscall = syscall;
  r->target = JS_UNDEFINED;
  r->resolve = JS_UNDEFINED;
  r->reject = JS_UNDEFINED;
  r->keep[0] = r->keep[1] = JS_UNDEFINED;
  r->req.data = r;
  if (JS_IsSymbol(reqv)) {
    JSValue res[2], p;
    p = JS_NewPromiseCapability(ctx, res);
    r->resolve = res[0];
    r->reject = res[1];
    r->is_promise = true;
    async_wrap_init(&r->promise_aw, env, p, PROVIDER_FSREQPROMISE, -1);
    r->promise_aw.object = JS_UNDEFINED;
    *ret = p;
  } else {
    ReqCallback *rc = JS_GetOpaque(reqv, req_callback_class_id);
    r->target = JS_DupValue(ctx, reqv);
    r->bigint = rc ? rc->bigint : false;
  }
  *is_async = true;
  return r;
}

/* starts an async request; on a synchronous failure to start it, completes
   it with the error */
static JSValue dispatch(FsReq *r, int err, JSValue ret) {
  if (err < 0) {
    r->req.result = err;
    fsreq_complete(r);
    fsreq_free(r);
  }
  return ret;
}

#define SYNC_THROW(err, syscall, path, dest) \
  JS_Throw(ctx, fs_exception(ctx, (err), (syscall), (path), (dest)))

/* ---------------------------------------------------------------------- */
/* after functions */

static JSValue after_integer(FsReq *r, JSContext *ctx) {
  return JS_NewInt64(ctx, r->req.result);
}

static JSValue after_stat(FsReq *r, JSContext *ctx) {
  if (r->is_promise)
    return new_stats(ctx, &r->req.statbuf, r->bigint);
  return global_stats(ctx, &r->req.statbuf, r->bigint, 0);
}

static JSValue after_statfs(FsReq *r, JSContext *ctx) {
  return statfs_values(ctx, r->req.ptr, r->bigint, r->is_promise);
}

static JSValue after_string_ptr(FsReq *r, JSContext *ctx) {
  const char *s = r->req.ptr;
  return node_string_encode(ctx, (const uint8_t *)s, strlen(s), r->encoding);
}

static JSValue after_string_path(FsReq *r, JSContext *ctx) {
  const char *s = r->req.path;
  return node_string_encode(ctx, (const uint8_t *)s, strlen(s), r->encoding);
}

static JSValue after_scandir(FsReq *r, JSContext *ctx) {
  JSValue names = JS_NewArray(ctx), types = JS_NewArray(ctx), res;
  uv_dirent_t ent;
  uint32_t i = 0;
  int e;
  while ((e = uv_fs_scandir_next(&r->req, &ent)) != UV_EOF) {
    if (e < 0) {
      JS_FreeValue(ctx, names);
      JS_FreeValue(ctx, types);
      return JS_Throw(ctx, fs_exception(ctx, e, "scandir", r->path, NULL));
    }
    JS_SetPropertyUint32(ctx, names, i,
                         node_string_encode(ctx, (const uint8_t *)ent.name, strlen(ent.name),
                                            r->encoding));
    JS_SetPropertyUint32(ctx, types, i, JS_NewInt32(ctx, ent.type));
    i++;
  }
  if (!r->with_types) {
    JS_FreeValue(ctx, types);
    return names;
  }
  res = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, res, 0, names);
  JS_SetPropertyUint32(ctx, res, 1, types);
  return res;
}

static JSValue after_mkdirp(FsReq *r, JSContext *ctx) {
  if (r->first_path)
    return node_new_utf8_string(ctx, (const uint8_t *)r->first_path, strlen(r->first_path));
  return JS_UNDEFINED;
}

static JSValue file_handle_new(Env *env, int fd);

static JSValue after_open_file_handle(FsReq *r, JSContext *ctx) {
  return file_handle_new(r->env, (int)r->req.result);
}

/* ---------------------------------------------------------------------- */
/* mkdir -p */

static int mkdirp_sync(const char *path, int mode, char **first) {
  char *p = strdup(path), *s;
  struct stat st;
  int err = 0;
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/')
    p[--n] = 0;
  if (mkdir(p, mode) == 0) {
    if (first && !*first)
      *first = strdup(p);
    free(p);
    return 0;
  }
  err = errno;
  if (err == EEXIST) {
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
      free(p);
      return 0;
    }
    free(p);
    return -EEXIST;
  }
  if (err != ENOENT) {
    free(p);
    return -err;
  }
  s = strrchr(p, '/');
  if (!s || s == p) {
    free(p);
    return -err;
  }
  *s = 0;
  err = mkdirp_sync(p, mode, first);
  *s = '/';
  if (err < 0) {
    free(p);
    return err;
  }
  if (mkdir(p, mode) == 0) {
    if (first && !*first)
      *first = strdup(p);
    free(p);
    return 0;
  }
  err = errno;
  if (err == EEXIST && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
    free(p);
    return 0;
  }
  free(p);
  return err == EEXIST ? -ENOTDIR : -err;
}

/* the async form runs the synchronous one on the thread pool */
typedef struct {
  uv_work_t work;
  FsReq *r;
  int mode;
  int result;
} MkdirpWork;

static void mkdirp_work(uv_work_t *w) {
  MkdirpWork *m = w->data;
  m->result = mkdirp_sync(m->r->path, m->mode, &m->r->first_path);
}

static void mkdirp_done(uv_work_t *w, int status) {
  MkdirpWork *m = w->data;
  m->r->req.result = status < 0 ? status : m->result;
  fsreq_complete(m->r);
  fsreq_free(m->r);
  free(m);
}

/* ---------------------------------------------------------------------- */
/* the functions */

#define PATH_OR_FAIL(var, v)                                                   \
  char *var = path_arg(ctx, v);                                                \
  if (!var)                                                                    \
    return JS_EXCEPTION;

/* GetValidFileMode (util.cc): access modes 0..7, copy modes 0..7; -1 with the
   error thrown */
static int valid_file_mode(JSContext *ctx, JSValueConst v, bool copy) {
  double d;
  int mode;
  if (JS_IsNumber(v)) {
    JS_ToFloat64(ctx, &d, v);
    if (isinf(d) || isnan(d)) {
      node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "mode is out of range");
      return -1;
    }
  } else if (!JS_IsNull(v) && !JS_IsUndefined(v)) {
    node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "mode must be int32 or null/undefined");
    return -1;
  } else {
    return copy ? 0 : F_OK;
  }
  JS_ToInt32(ctx, &mode, v);
  if (mode < 0 || mode > 7) {
    node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "mode is out of range: >= %d && <= %d", 0, 7);
    return -1;
  }
  return mode;
}

static JSValue fs_access(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t mode = valid_file_mode(ctx, ARG(1), false);
  JSValue ret;
  bool async;
  FsReq *r;
  if (mode < 0)
    return JS_EXCEPTION;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? get_req(ctx, ARG(2), "access", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    return dispatch(r, uv_fs_access(r->env->loop, &r->req, path, mode, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_access(NULL, &req, path, mode, NULL);
    uv_fs_req_cleanup(&req);
    if (err < 0) {
      JSValue e = SYNC_THROW(err, "access", path, NULL);
      free(path);
      return e;
    }
    free(path);
    return JS_UNDEFINED;
  }
}

static JSValue fs_exists_sync(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  char *path = path_arg(ctx, ARG(0));
  bool r;
  if (!path) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_FALSE;
  }
  r = access(path, F_OK) == 0;
  free(path);
  return JS_NewBool(ctx, r);
}

static JSValue fs_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t flags = nb_int32(ctx, ARG(1), 0), mode = nb_int32(ctx, ARG(2), 0666);
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 3 ? get_req(ctx, ARG(3), "open", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    r->after = after_integer;
    return dispatch(r, uv_fs_open(r->env->loop, &r->req, path, flags | O_CLOEXEC, mode, after_cb), ret);
  } else {
    uv_fs_t req;
    int fd = uv_fs_open(NULL, &req, path, flags | O_CLOEXEC, mode, NULL);
    uv_fs_req_cleanup(&req);
    if (fd < 0) {
      JSValue e = SYNC_THROW(fd, "open", path, NULL);
      free(path);
      return e;
    }
    free(path);
    return JS_NewInt32(ctx, fd);
  }
}

static JSValue fs_open_file_handle(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  int32_t flags = nb_int32(ctx, ARG(1), 0), mode = nb_int32(ctx, ARG(2), 0666);
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = get_req(ctx, ARG(3), "open", &ret, &async);
  if (r) {
    r->path = path;
    r->after = after_open_file_handle;
    return dispatch(r, uv_fs_open(env->loop, &r->req, path, flags | O_CLOEXEC, mode, after_cb), ret);
  } else {
    uv_fs_t req;
    int fd = uv_fs_open(NULL, &req, path, flags | O_CLOEXEC, mode, NULL);
    uv_fs_req_cleanup(&req);
    free(path);
    if (fd < 0) {
      if (JS_IsObject(ARG(4))) {
        JS_SetPropertyStr(ctx, ARG(4), "errno", JS_NewInt32(ctx, fd));
        JS_SetPropertyStr(ctx, ARG(4), "syscall", JS_NewString(ctx, "open"));
        return JS_UNDEFINED;
      }
      return SYNC_THROW(fd, "open", NULL, NULL);
    }
    return file_handle_new(env, fd);
  }
}

/* DetermineSpecificErrorType (util.cc): how ERR_INVALID_ARG_TYPE describes
   a value (malloc'ed) */
char *node_specific_error_type(JSContext *ctx, JSValueConst v) {
  char buf[256];
  const char *s;
  if (JS_IsFunction(ctx, v))
    return strdup("function");
  if (JS_IsString(v)) {
    char val[40];
    s = JS_ToCString(ctx, v);
    if (s && strlen(s) > 28)
      snprintf(val, sizeof(val), "%.25s...", s);
    else
      snprintf(val, sizeof(val), "%s", s ? s : "");
    JS_FreeCString(ctx, s);
    if (!strchr(val, '\'')) {
      snprintf(buf, sizeof(buf), "type string ('%s')", val);
    } else {
      JSValue j = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
      s = JS_ToCString(ctx, j);
      snprintf(buf, sizeof(buf), "type string (%s)", s ? s : "");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, j);
    }
    return strdup(buf);
  }
  if (JS_IsObject(v)) {
    JSValue c = JS_GetPropertyStr(ctx, v, "constructor"), n = JS_UNDEFINED;
    if (JS_IsFunction(ctx, c))
      n = JS_GetPropertyStr(ctx, c, "name");
    if (JS_IsException(c) || JS_IsException(n))
      JS_FreeValue(ctx, JS_GetException(ctx));
    s = JS_IsString(n) ? JS_ToCString(ctx, n) : NULL;
    snprintf(buf, sizeof(buf), "an instance of %s", s && *s ? s : "Object");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, c);
    return strdup(buf);
  }
  if (JS_IsSymbol(v)) {
    JSValue d = JS_GetPropertyStr(ctx, v, "description");
    s = JS_IsString(d) ? JS_ToCString(ctx, d) : NULL;
    snprintf(buf, sizeof(buf), "Symbol(%s)", s ? s : "");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, d);
    return strdup(buf);
  }
  if (JS_IsNumber(v)) {
    double d;
    JS_ToFloat64(ctx, &d, v);
    if (isnan(d))
      return strdup("type number (NaN)");
    if (isinf(d))
      return strdup("type number (Infinity)");
  }
  {
    JSValue str = JS_ToString(ctx, v);
    s = JS_ToCString(ctx, str);
    if (JS_IsNumber(v))
      snprintf(buf, sizeof(buf), "type number (%s)", s ? s : "");
    else if (JS_IsBool(v))
      snprintf(buf, sizeof(buf), "type boolean (%s)", s ? s : "");
    else if (JS_IsBigInt(v))
      snprintf(buf, sizeof(buf), "type bigint (%s)", s ? s : "");
    else
      snprintf(buf, sizeof(buf), "%s", s ? s : "");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, str);
  }
  return strdup(buf);
}

#define FD_INVALID INT32_MIN

/* GetValidatedFd (util.cc): the "fd" argument, or FD_INVALID with the error
   thrown */
static int32_t validated_fd(JSContext *ctx, JSValueConst v) {
  double d;
  if (!JS_IsNumber(v)) {
    char *t = node_specific_error_type(ctx, v);
    node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                          "The \"fd\" argument must be of type number. Received %s", t);
    free(t);
    return FD_INVALID;
  }
  JS_ToFloat64(ctx, &d, v);
  if (d < 0 || d > INT32_MAX || d != floor(d) || isnan(d)) {
    JSValue str = JS_ToString(ctx, v);
    const char *s = JS_ToCString(ctx, str);
    if ((d < 0 || d > INT32_MAX) && !isinf(d))
      node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                             "The value of \"fd\" is out of range. It must be >= 0 && <= %d. "
                             "Received %s", INT32_MAX, s ? s : "");
    else
      node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                             "The value of \"fd\" is out of range. It must be an integer. "
                             "Received %s", s ? s : "");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, str);
    return FD_INVALID;
  }
  return (int32_t)d;
}

static JSValue fs_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  JSValue ret;
  bool async;
  FsReq *r = argc > 1 ? get_req(ctx, ARG(1), "close", &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (r)
    return dispatch(r, uv_fs_close(r->env->loop, &r->req, fd, after_cb), ret);
  {
    uv_fs_t req;
    int err = uv_fs_close(NULL, &req, fd, NULL);
    uv_fs_req_cleanup(&req);
    if (err < 0)
      return SYNC_THROW(err, "close", NULL, NULL);
    return JS_UNDEFINED;
  }
}

static JSValue do_stat(JSContext *ctx, int argc, JSValueConst *argv, int kind) {
  /* kind 0 stat, 1 lstat, 2 fstat */
  static const char *const names[] = { "stat", "lstat", "fstat" };
  bool bigint = JS_ToBool(ctx, ARG(1));
  JSValue ret;
  bool async;
  char *path = NULL;
  int32_t fd = -1;
  FsReq *r;
  int err;
  if (kind == 2) {
    if ((fd = validated_fd(ctx, ARG(0))) == FD_INVALID)
      return JS_EXCEPTION;
  } else if (!(path = path_arg(ctx, ARG(0)))) {
    return JS_EXCEPTION;
  }
  r = get_req(ctx, ARG(2), names[kind], &ret, &async);
  if (r) {
    r->path = path;
    r->bigint = r->is_promise ? bigint : r->bigint || bigint;
    r->after = after_stat;
    r->no_throw_noent = kind == 0 && JS_IsBool(ARG(3)) && !JS_ToBool(ctx, ARG(3));
    switch (kind) {
    case 0: err = uv_fs_stat(r->env->loop, &r->req, path, after_cb); break;
    case 1: err = uv_fs_lstat(r->env->loop, &r->req, path, after_cb); break;
    default: err = uv_fs_fstat(r->env->loop, &r->req, fd, after_cb); break;
    }
    return dispatch(r, err, ret);
  } else {
    uv_fs_t req;
    switch (kind) {
    case 0: err = uv_fs_stat(NULL, &req, path, NULL); break;
    case 1: err = uv_fs_lstat(NULL, &req, path, NULL); break;
    default: err = uv_fs_fstat(NULL, &req, fd, NULL); break;
    }
    if (err < 0) {
      uv_fs_req_cleanup(&req);
      if (kind < 2 && JS_IsBool(ARG(3)) && !JS_ToBool(ctx, ARG(3)) &&
          (err == UV_ENOENT || err == UV_ENOTDIR)) {
        free(path);
        return JS_UNDEFINED;
      }
      if (kind == 2 && JS_ToBool(ctx, ARG(3))) {
        free(path);
        return JS_UNDEFINED;
      }
      ret = SYNC_THROW(err, names[kind], path, NULL);
      free(path);
      return ret;
    }
    ret = global_stats(ctx, &req.statbuf, bigint, 0);
    uv_fs_req_cleanup(&req);
    free(path);
    return ret;
  }
}

static JSValue fs_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return do_stat(ctx, argc, argv, 0);
}
static JSValue fs_lstat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return do_stat(ctx, argc, argv, 1);
}
static JSValue fs_fstat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return do_stat(ctx, argc, argv, 2);
}

static JSValue fs_statfs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  bool bigint = JS_ToBool(ctx, ARG(1));
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? get_req(ctx, ARG(2), "statfs", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    r->bigint = bigint;
    r->after = after_statfs;
    return dispatch(r, uv_fs_statfs(r->env->loop, &r->req, path, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_statfs(NULL, &req, path, NULL);
    if (err < 0) {
      uv_fs_req_cleanup(&req);
      ret = SYNC_THROW(err, "statfs", path, NULL);
      free(path);
      return ret;
    }
    ret = statfs_values(ctx, req.ptr, bigint, false);
    uv_fs_req_cleanup(&req);
    free(path);
    return ret;
  }
}

static JSValue fs_internal_module_stat(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  struct stat st;
  char *path = path_arg(ctx, ARG(argc > 1 ? 1 : 0));
  int rc;
  if (!path)
    return JS_EXCEPTION;
  if (stat(path, &st) == 0)
    rc = S_ISDIR(st.st_mode) ? 1 : 0;
  else
    rc = -errno;
  free(path);
  return JS_NewInt32(ctx, rc);
}

/* simple path calls: (path[, dest], ...args, req) */
static JSValue fs_rename(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue ret;
  bool async;
  FsReq *r;
  char *from = path_arg(ctx, ARG(0)), *to;
  if (!from)
    return JS_EXCEPTION;
  to = path_arg(ctx, ARG(1));
  if (!to) {
    free(from);
    return JS_EXCEPTION;
  }
  r = argc > 2 ? get_req(ctx, ARG(2), "rename", &ret, &async) : NULL;
  if (r) {
    r->path = from;
    r->dest = to;
    return dispatch(r, uv_fs_rename(r->env->loop, &r->req, from, to, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_rename(NULL, &req, from, to, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "rename", from, to) : JS_UNDEFINED;
    free(from);
    free(to);
    return ret;
  }
}

static JSValue fs_link(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue ret;
  bool async;
  FsReq *r;
  char *from = path_arg(ctx, ARG(0)), *to;
  if (!from)
    return JS_EXCEPTION;
  to = path_arg(ctx, ARG(1));
  if (!to) {
    free(from);
    return JS_EXCEPTION;
  }
  r = argc > 2 ? get_req(ctx, ARG(2), "link", &ret, &async) : NULL;
  if (r) {
    r->path = from;
    r->dest = to;
    return dispatch(r, uv_fs_link(r->env->loop, &r->req, from, to, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_link(NULL, &req, from, to, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "link", from, to) : JS_UNDEFINED;
    free(from);
    free(to);
    return ret;
  }
}

static JSValue fs_symlink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue ret;
  bool async;
  FsReq *r;
  int32_t flags = nb_int32(ctx, ARG(2), 0);
  char *target = path_arg(ctx, ARG(0)), *path;
  if (!target)
    return JS_EXCEPTION;
  path = path_arg(ctx, ARG(1));
  if (!path) {
    free(target);
    return JS_EXCEPTION;
  }
  r = argc > 3 ? get_req(ctx, ARG(3), "symlink", &ret, &async) : NULL;
  if (r) {
    r->path = target;
    r->dest = path;
    return dispatch(r, uv_fs_symlink(r->env->loop, &r->req, target, path, flags, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_symlink(NULL, &req, target, path, flags, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "symlink", target, path) : JS_UNDEFINED;
    free(target);
    free(path);
    return ret;
  }
}

static JSValue fs_readlink(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  int enc = node_parse_encoding(ctx, ARG(1), ENC_UTF8);
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? get_req(ctx, ARG(2), "readlink", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    r->encoding = enc;
    r->after = after_string_ptr;
    return dispatch(r, uv_fs_readlink(r->env->loop, &r->req, path, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_readlink(NULL, &req, path, NULL);
    if (err < 0)
      ret = SYNC_THROW(err, "readlink", path, NULL);
    else
      ret = node_string_encode(ctx, req.ptr, strlen(req.ptr), enc);
    uv_fs_req_cleanup(&req);
    free(path);
    return ret;
  }
}

static JSValue fs_realpath(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  int enc = node_parse_encoding(ctx, ARG(1), ENC_UTF8);
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? get_req(ctx, ARG(2), "realpath", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    r->encoding = enc;
    r->after = after_string_ptr;
    return dispatch(r, uv_fs_realpath(r->env->loop, &r->req, path, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_realpath(NULL, &req, path, NULL);
    if (err < 0)
      ret = SYNC_THROW(err, "realpath", path, NULL);
    else
      ret = node_string_encode(ctx, req.ptr, strlen(req.ptr), enc);
    uv_fs_req_cleanup(&req);
    free(path);
    return ret;
  }
}

static JSValue fs_unlink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 1 ? get_req(ctx, ARG(1), "unlink", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    return dispatch(r, uv_fs_unlink(r->env->loop, &r->req, path, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_unlink(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "unlink", path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_rmdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 1 ? get_req(ctx, ARG(1), "rmdir", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    return dispatch(r, uv_fs_rmdir(r->env->loop, &r->req, path, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_rmdir(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "rmdir", path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_mkdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t mode = nb_int32(ctx, ARG(1), 0777);
  bool recursive = JS_ToBool(ctx, ARG(2));
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 3 ? get_req(ctx, ARG(3), "mkdir", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    if (recursive) {
      MkdirpWork *w = calloc(1, sizeof(*w));
      w->r = r;
      w->mode = mode;
      w->work.data = w;
      r->after = after_mkdirp;
      uv_queue_work(r->env->loop, &w->work, mkdirp_work, mkdirp_done);
      return ret;
    }
    return dispatch(r, uv_fs_mkdir(r->env->loop, &r->req, path, mode, after_cb), ret);
  } else if (recursive) {
    char *first = NULL;
    int err = mkdirp_sync(path, mode, &first);
    if (err < 0) {
      ret = SYNC_THROW(err, "mkdir", path, NULL);
      free(path);
      free(first);
      return ret;
    }
    free(path);
    ret = first ? node_new_utf8_string(ctx, (uint8_t *)first, strlen(first)) : JS_UNDEFINED;
    free(first);
    return ret;
  } else {
    uv_fs_t req;
    int err = uv_fs_mkdir(NULL, &req, path, mode, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "mkdir", path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_readdir(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  int enc = node_parse_encoding(ctx, ARG(1), ENC_UTF8);
  bool with_types = JS_ToBool(ctx, ARG(2));
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 3 ? get_req(ctx, ARG(3), "scandir", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    r->encoding = enc;
    r->with_types = with_types;
    r->after = after_scandir;
    return dispatch(r, uv_fs_scandir(r->env->loop, &r->req, path, 0, after_cb), ret);
  } else {
    FsReq tmp;
    int err;
    memset(&tmp, 0, sizeof(tmp));
    err = uv_fs_scandir(NULL, &tmp.req, path, 0, NULL);
    if (err < 0) {
      uv_fs_req_cleanup(&tmp.req);
      ret = SYNC_THROW(err, "scandir", path, NULL);
      free(path);
      return ret;
    }
    tmp.encoding = enc;
    tmp.with_types = with_types;
    tmp.path = path;
    ret = after_scandir(&tmp, ctx);
    uv_fs_req_cleanup(&tmp.req);
    free(path);
    return ret;
  }
}

static JSValue fs_mkdtemp(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  int enc = node_parse_encoding(ctx, ARG(1), ENC_UTF8);
  JSValue ret;
  bool async;
  FsReq *r;
  char *prefix = path_arg(ctx, ARG(0)), *tmpl;
  if (!prefix)
    return JS_EXCEPTION;
  tmpl = malloc(strlen(prefix) + 8);
  sprintf(tmpl, "%sXXXXXX", prefix);
  free(prefix);
  r = argc > 2 ? get_req(ctx, ARG(2), "mkdtemp", &ret, &async) : NULL;
  if (r) {
    r->path = tmpl;
    r->encoding = enc;
    r->after = after_string_path;
    return dispatch(r, uv_fs_mkdtemp(r->env->loop, &r->req, tmpl, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_mkdtemp(NULL, &req, tmpl, NULL);
    if (err < 0)
      ret = SYNC_THROW(err, "mkdtemp", tmpl, NULL);
    else
      ret = node_string_encode(ctx, (const uint8_t *)req.path, strlen(req.path), enc);
    uv_fs_req_cleanup(&req);
    free(tmpl);
    return ret;
  }
}

static JSValue fs_copy_file(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  int32_t flags = valid_file_mode(ctx, ARG(2), true);
  JSValue ret;
  bool async;
  FsReq *r;
  char *src, *dest;
  if (flags < 0)
    return JS_EXCEPTION;
  src = path_arg(ctx, ARG(0));
  if (!src)
    return JS_EXCEPTION;
  dest = path_arg(ctx, ARG(1));
  if (!dest) {
    free(src);
    return JS_EXCEPTION;
  }
  r = argc > 3 ? get_req(ctx, ARG(3), "copyfile", &ret, &async) : NULL;
  if (r) {
    r->path = src;
    r->dest = dest;
    return dispatch(r, uv_fs_copyfile(r->env->loop, &r->req, src, dest, flags, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_copyfile(NULL, &req, src, dest, flags, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "copyfile", src, dest) : JS_UNDEFINED;
    free(src);
    free(dest);
    return ret;
  }
}

/* fd calls with integer args: ftruncate, fsync, fdatasync, fchmod, fchown */
static JSValue fs_ftruncate(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  int64_t len = offset_arg(ctx, ARG(1));
  JSValue ret;
  bool async;
  FsReq *r = argc > 2 ? get_req(ctx, ARG(2), "ftruncate", &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (len < 0)
    len = 0;
  if (r)
    return dispatch(r, uv_fs_ftruncate(r->env->loop, &r->req, fd, len, after_cb), ret);
  {
    uv_fs_t req;
    int err = uv_fs_ftruncate(NULL, &req, fd, len, NULL);
    uv_fs_req_cleanup(&req);
    return err < 0 ? SYNC_THROW(err, "ftruncate", NULL, NULL) : JS_UNDEFINED;
  }
}

static JSValue fs_fsync_common(JSContext *ctx, int argc, JSValueConst *argv, bool data) {
  int32_t fd = validated_fd(ctx, ARG(0));
  const char *name = data ? "fdatasync" : "fsync";
  JSValue ret;
  bool async;
  FsReq *r = argc > 1 ? get_req(ctx, ARG(1), name, &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (r)
    return dispatch(r, data ? uv_fs_fdatasync(r->env->loop, &r->req, fd, after_cb)
                            : uv_fs_fsync(r->env->loop, &r->req, fd, after_cb), ret);
  {
    uv_fs_t req;
    int err = data ? uv_fs_fdatasync(NULL, &req, fd, NULL) : uv_fs_fsync(NULL, &req, fd, NULL);
    uv_fs_req_cleanup(&req);
    return err < 0 ? SYNC_THROW(err, name, NULL, NULL) : JS_UNDEFINED;
  }
}

static JSValue fs_fsync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_fsync_common(ctx, argc, argv, false);
}
static JSValue fs_fdatasync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_fsync_common(ctx, argc, argv, true);
}

static JSValue fs_chmod(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t mode = nb_int32(ctx, ARG(1), 0);
  JSValue ret;
  bool async;
  FsReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? get_req(ctx, ARG(2), "chmod", &ret, &async) : NULL;
  if (r) {
    r->path = path;
    return dispatch(r, uv_fs_chmod(r->env->loop, &r->req, path, mode, after_cb), ret);
  } else {
    uv_fs_t req;
    int err = uv_fs_chmod(NULL, &req, path, mode, NULL);
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, "chmod", path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_fchmod(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0)), mode = fd == FD_INVALID ? 0 : nb_int32(ctx, ARG(1), 0);
  JSValue ret;
  bool async;
  FsReq *r = argc > 2 ? get_req(ctx, ARG(2), "fchmod", &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (r)
    return dispatch(r, uv_fs_fchmod(r->env->loop, &r->req, fd, mode, after_cb), ret);
  {
    uv_fs_t req;
    int err = uv_fs_fchmod(NULL, &req, fd, mode, NULL);
    uv_fs_req_cleanup(&req);
    return err < 0 ? SYNC_THROW(err, "fchmod", NULL, NULL) : JS_UNDEFINED;
  }
}

static JSValue fs_chown_common(JSContext *ctx, int argc, JSValueConst *argv, int kind) {
  static const char *const names[] = { "chown", "lchown", "fchown" };
  uint32_t uid = nb_uint32(ctx, ARG(1), (uint32_t)-1), gid = nb_uint32(ctx, ARG(2), (uint32_t)-1);
  JSValue ret;
  bool async;
  FsReq *r;
  char *path = NULL;
  int32_t fd = -1;
  int err;
  if (kind == 2) {
    if ((fd = validated_fd(ctx, ARG(0))) == FD_INVALID)
      return JS_EXCEPTION;
  } else if (!(path = path_arg(ctx, ARG(0)))) {
    return JS_EXCEPTION;
  }
  r = argc > 3 ? get_req(ctx, ARG(3), names[kind], &ret, &async) : NULL;
  if (r) {
    r->path = path;
    switch (kind) {
    case 0: err = uv_fs_chown(r->env->loop, &r->req, path, uid, gid, after_cb); break;
    case 1: err = uv_fs_lchown(r->env->loop, &r->req, path, uid, gid, after_cb); break;
    default: err = uv_fs_fchown(r->env->loop, &r->req, fd, uid, gid, after_cb); break;
    }
    return dispatch(r, err, ret);
  } else {
    uv_fs_t req;
    switch (kind) {
    case 0: err = uv_fs_chown(NULL, &req, path, uid, gid, NULL); break;
    case 1: err = uv_fs_lchown(NULL, &req, path, uid, gid, NULL); break;
    default: err = uv_fs_fchown(NULL, &req, fd, uid, gid, NULL); break;
    }
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, names[kind], path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_chown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_chown_common(ctx, argc, argv, 0);
}
static JSValue fs_lchown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_chown_common(ctx, argc, argv, 1);
}
static JSValue fs_fchown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_chown_common(ctx, argc, argv, 2);
}

static JSValue fs_utimes_common(JSContext *ctx, int argc, JSValueConst *argv, int kind) {
  static const char *const names[] = { "utime", "lutime", "futime" };
  double atime = nb_double(ctx, ARG(1), 0), mtime = nb_double(ctx, ARG(2), 0);
  JSValue ret;
  bool async;
  FsReq *r;
  char *path = NULL;
  int32_t fd = -1;
  int err;
  if (kind == 2) {
    if ((fd = validated_fd(ctx, ARG(0))) == FD_INVALID)
      return JS_EXCEPTION;
  } else if (!(path = path_arg(ctx, ARG(0)))) {
    return JS_EXCEPTION;
  }
  r = argc > 3 ? get_req(ctx, ARG(3), names[kind], &ret, &async) : NULL;
  if (r) {
    r->path = path;
    switch (kind) {
    case 0: err = uv_fs_utime(r->env->loop, &r->req, path, atime, mtime, after_cb); break;
    case 1: err = uv_fs_lutime(r->env->loop, &r->req, path, atime, mtime, after_cb); break;
    default: err = uv_fs_futime(r->env->loop, &r->req, fd, atime, mtime, after_cb); break;
    }
    return dispatch(r, err, ret);
  } else {
    uv_fs_t req;
    switch (kind) {
    case 0: err = uv_fs_utime(NULL, &req, path, atime, mtime, NULL); break;
    case 1: err = uv_fs_lutime(NULL, &req, path, atime, mtime, NULL); break;
    default: err = uv_fs_futime(NULL, &req, fd, atime, mtime, NULL); break;
    }
    uv_fs_req_cleanup(&req);
    ret = err < 0 ? SYNC_THROW(err, names[kind], path, NULL) : JS_UNDEFINED;
    free(path);
    return ret;
  }
}

static JSValue fs_utimes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_utimes_common(ctx, argc, argv, 0);
}
static JSValue fs_lutimes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_utimes_common(ctx, argc, argv, 1);
}
static JSValue fs_futimes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return fs_utimes_common(ctx, argc, argv, 2);
}

/* read(fd, buffer, offset, length, position[, req]) */
static JSValue fs_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  size_t blen;
  uint8_t *buf = nb_buffer_data(ctx, ARG(1), &blen);
  int64_t off = nb_int64(ctx, ARG(2), 0), len = nb_int64(ctx, ARG(3), 0);
  int64_t pos = offset_arg(ctx, ARG(4));
  uv_buf_t uvbuf;
  JSValue ret;
  bool async;
  FsReq *r;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (!buf)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "buffer must be a Buffer");
  if (off < 0 || (size_t)off > blen || len < 0 || (size_t)(off + len) > blen)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "offset/length out of range");
  uvbuf = uv_buf_init((char *)buf + off, (unsigned)len);
  r = argc > 5 ? get_req(ctx, ARG(5), "read", &ret, &async) : NULL;
  if (r) {
    r->keep[0] = JS_DupValue(ctx, ARG(1));
    r->after = after_integer;
    return dispatch(r, uv_fs_read(r->env->loop, &r->req, fd, &uvbuf, 1, pos, after_cb), ret);
  } else {
    uv_fs_t req;
    int n = uv_fs_read(NULL, &req, fd, &uvbuf, 1, pos, NULL);
    uv_fs_req_cleanup(&req);
    if (n < 0)
      return SYNC_THROW(n, "read", NULL, NULL);
    return JS_NewInt32(ctx, n);
  }
}

static uv_buf_t *iovs_from_array(JSContext *ctx, JSValueConst arr, unsigned *pn) {
  JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t n = 0, i;
  uv_buf_t *iovs;
  JS_ToUint32(ctx, &n, lenv);
  iovs = calloc(n ? n : 1, sizeof(uv_buf_t));
  for (i = 0; i < n; i++) {
    JSValue b = JS_GetPropertyUint32(ctx, arr, i);
    size_t l = 0;
    uint8_t *p = nb_buffer_data(ctx, b, &l);
    JS_FreeValue(ctx, b);
    iovs[i] = uv_buf_init((char *)p, (unsigned)l);
  }
  *pn = n;
  return iovs;
}

static JSValue fs_read_buffers(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  int64_t pos = offset_arg(ctx, ARG(2));
  unsigned n;
  uv_buf_t *iovs = iovs_from_array(ctx, ARG(1), &n);
  JSValue ret;
  bool async;
  FsReq *r = argc > 3 ? get_req(ctx, ARG(3), "read", &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (r) {
    r->iovs = iovs;
    r->keep[0] = JS_DupValue(ctx, ARG(1));
    r->after = after_integer;
    return dispatch(r, uv_fs_read(r->env->loop, &r->req, fd, iovs, n, pos, after_cb), ret);
  } else {
    uv_fs_t req;
    int got = uv_fs_read(NULL, &req, fd, iovs, n, pos, NULL);
    uv_fs_req_cleanup(&req);
    free(iovs);
    if (got < 0)
      return SYNC_THROW(got, "read", NULL, NULL);
    return JS_NewInt32(ctx, got);
  }
}

static JSValue sync_ctx_error(JSContext *ctx, JSValueConst ctxobj, int err, const char *syscall) {
  if (JS_IsObject(ctxobj)) {
    JS_SetPropertyStr(ctx, ctxobj, "errno", JS_NewInt32(ctx, err));
    JS_SetPropertyStr(ctx, ctxobj, "syscall", JS_NewString(ctx, syscall));
    return JS_NewInt32(ctx, err);
  }
  return SYNC_THROW(err, syscall, NULL, NULL);
}

/* writeBuffer(fd, buffer, offset, length, position, req | (undefined, ctx)) */
static JSValue fs_write_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  size_t blen;
  uint8_t *buf = nb_buffer_data(ctx, ARG(1), &blen);
  int64_t off = nb_int64(ctx, ARG(2), 0), len = nb_int64(ctx, ARG(3), 0);
  int64_t pos = offset_arg(ctx, ARG(4));
  uv_buf_t uvbuf;
  JSValue ret;
  bool async;
  FsReq *r;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (!buf)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "buffer must be a Buffer");
  if (off < 0 || (size_t)off > blen || len < 0 || (size_t)(off + len) > blen)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "offset/length out of range");
  uvbuf = uv_buf_init((char *)buf + off, (unsigned)len);
  r = get_req(ctx, ARG(5), "write", &ret, &async);
  if (r) {
    r->keep[0] = JS_DupValue(ctx, ARG(1));
    r->after = after_integer;
    return dispatch(r, uv_fs_write(r->env->loop, &r->req, fd, &uvbuf, 1, pos, after_cb), ret);
  } else {
    uv_fs_t req;
    int n = uv_fs_write(NULL, &req, fd, &uvbuf, 1, pos, NULL);
    uv_fs_req_cleanup(&req);
    if (n < 0)
      return sync_ctx_error(ctx, ARG(6), n, "write");
    return JS_NewInt32(ctx, n);
  }
}

static JSValue fs_write_buffers(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  int64_t pos = offset_arg(ctx, ARG(2));
  unsigned n;
  uv_buf_t *iovs = iovs_from_array(ctx, ARG(1), &n);
  JSValue ret;
  bool async;
  FsReq *r = argc > 3 ? get_req(ctx, ARG(3), "write", &ret, &async) : NULL;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  if (r) {
    r->iovs = iovs;
    r->keep[0] = JS_DupValue(ctx, ARG(1));
    r->after = after_integer;
    return dispatch(r, uv_fs_write(r->env->loop, &r->req, fd, iovs, n, pos, after_cb), ret);
  } else {
    uv_fs_t req;
    int w = uv_fs_write(NULL, &req, fd, iovs, n, pos, NULL);
    uv_fs_req_cleanup(&req);
    free(iovs);
    if (w < 0)
      return SYNC_THROW(w, "write", NULL, NULL);
    return JS_NewInt32(ctx, w);
  }
}

/* writeString(fd, string, pos, encoding, req | (undefined, ctx)) */
static JSValue fs_write_string(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  int32_t fd = validated_fd(ctx, ARG(0));
  int64_t pos = offset_arg(ctx, ARG(2));
  int enc = node_parse_encoding(ctx, ARG(3), ENC_UTF8);
  size_t cap = node_string_bytes_size(ctx, ARG(1), enc), len;
  char *buf = malloc(cap + 1);
  uv_buf_t uvbuf;
  JSValue ret;
  bool async;
  FsReq *r;
  if (fd == FD_INVALID)
    return JS_EXCEPTION;
  len = node_string_write(ctx, (uint8_t *)buf, cap, ARG(1), enc, NULL);
  uvbuf = uv_buf_init(buf, (unsigned)len);
  r = get_req(ctx, ARG(4), "write", &ret, &async);
  if (r) {
    r->buf = buf;
    r->after = after_integer;
    return dispatch(r, uv_fs_write(r->env->loop, &r->req, fd, &uvbuf, 1, pos, after_cb), ret);
  } else {
    uv_fs_t req;
    int n = uv_fs_write(NULL, &req, fd, &uvbuf, 1, pos, NULL);
    uv_fs_req_cleanup(&req);
    free(buf);
    if (n < 0)
      return sync_ctx_error(ctx, ARG(5), n, "write");
    return JS_NewInt32(ctx, n);
  }
}

/* readFileUtf8(path | fd, flags) */
static JSValue fs_read_file_utf8(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  int32_t flags = nb_int32(ctx, ARG(1), O_RDONLY);
  int fd;
  bool is_fd = JS_IsNumber(ARG(0));
  char *path = NULL, *data = NULL;
  size_t len = 0, cap = 0;
  JSValue ret;
  if (is_fd) {
    fd = nb_int32(ctx, ARG(0), -1);
  } else {
    if (!(path = path_arg(ctx, ARG(0))))
      return JS_EXCEPTION;
    fd = open(path, flags | O_CLOEXEC, 0666);
    if (fd < 0) {
      ret = SYNC_THROW(-errno, "open", path, NULL);
      free(path);
      return ret;
    }
  }
  {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
      cap = (size_t)st.st_size + 1;
  }
  if (cap < 8192)
    cap = 8192;
  data = malloc(cap);
  for (;;) {
    ssize_t n;
    if (len == cap) {
      cap *= 2;
      data = realloc(data, cap);
    }
    n = read(fd, data + len, cap - len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ret = SYNC_THROW(-errno, "read", NULL, NULL);
      free(data);
      if (!is_fd)
        close(fd);
      free(path);
      return ret;
    }
    if (n == 0)
      break;
    len += n;
  }
  if (!is_fd)
    close(fd);
  free(path);
  ret = node_new_utf8_string(ctx, (uint8_t *)data, len);
  free(data);
  return ret;
}

/* writeFileUtf8(path | fd, data, flags, mode) */
static JSValue fs_write_file_utf8(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  int32_t flags = nb_int32(ctx, ARG(2), O_WRONLY | O_CREAT | O_TRUNC);
  int32_t mode = nb_int32(ctx, ARG(3), 0666);
  bool is_fd = JS_IsNumber(ARG(0));
  size_t len, off = 0;
  char *data, *path = NULL;
  int fd;
  JSValue ret = JS_UNDEFINED;
  if (JS_IsString(ARG(1))) {
    data = node_string_to_utf8(ctx, ARG(1), &len);
  } else {
    size_t l;
    uint8_t *b = nb_buffer_data(ctx, ARG(1), &l);
    data = malloc(l + 1);
    if (b)
      memcpy(data, b, l);
    len = b ? l : 0;
  }
  if (is_fd) {
    fd = nb_int32(ctx, ARG(0), -1);
  } else {
    if (!(path = path_arg(ctx, ARG(0)))) {
      free(data);
      return JS_EXCEPTION;
    }
    fd = open(path, flags | O_CLOEXEC, mode);
    if (fd < 0) {
      ret = SYNC_THROW(-errno, "open", path, NULL);
      free(path);
      free(data);
      return ret;
    }
  }
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ret = SYNC_THROW(-errno, "write", NULL, NULL);
      break;
    }
    off += n;
  }
  if (!is_fd && close(fd) < 0 && !JS_IsException(ret))
    ret = SYNC_THROW(-errno, "close", NULL, NULL);
  free(path);
  free(data);
  return ret;
}

/* rmSync(path, maxRetries, recursive, retryDelay) */
static int rm_entry(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  int r = (typeflag == FTW_DP || typeflag == FTW_D) ? rmdir(fpath) : unlink(fpath);
  return r ? errno : 0;
}

static JSValue fs_rm_sync(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  struct stat st;
  int32_t retries = nb_int32(ctx, ARG(1), 0), delay = nb_int32(ctx, ARG(3), 100);
  bool recursive = JS_ToBool(ctx, ARG(2));
  int err = 0, i = 1;
  JSValue ret;
  PATH_OR_FAIL(path, ARG(0));
  if (lstat(path, &st) < 0) {
    free(path);
    return JS_UNDEFINED;
  }
  if (S_ISDIR(st.st_mode) && !recursive) {
    ret = node_throw_error(ctx, "ERR_FS_EISDIR", "Path is a directory: %s", path);
    free(path);
    return ret;
  }
  while (retries >= 0) {
    if (S_ISDIR(st.st_mode))
      err = nftw(path, rm_entry, 64, FTW_DEPTH | FTW_PHYS);
    else
      err = unlink(path) ? errno : 0;
    if (err == 0 || err == ENOENT) {
      free(path);
      return JS_UNDEFINED;
    }
    if (err != EBUSY && err != EMFILE && err != ENFILE && err != ENOTEMPTY && err != EPERM)
      break;
    if (delay > 0)
      uv_sleep(i * delay);
    retries--;
    i++;
  }
  if (err == EPERM)
    err = EACCES;
  ret = SYNC_THROW(-err, "rm", path, NULL);
  free(path);
  return ret;
}

static JSValue fs_get_format_of_extensionless_file(JSContext *ctx, JSValueConst this_val,
                                                   int argc, JSValueConst *argv) {
  char *path = path_arg(ctx, ARG(0));
  unsigned char b[4];
  int fd, r = 0;
  if (!path)
    return JS_EXCEPTION;
  fd = open(path, O_RDONLY | O_CLOEXEC);
  free(path);
  if (fd >= 0) {
    if (read(fd, b, 4) == 4 && b[0] == 0 && b[1] == 'a' && b[2] == 's' && b[3] == 'm')
      r = 1;
    close(fd);
  }
  return JS_NewInt32(ctx, r);
}

static bool is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

/* path.resolve(a, b) for absolute a */
static char *path_join(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *r = malloc(la + lb + 2), *src, *out;
  if (b[0] == '/') {
    strcpy(r, b);
  } else {
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
  }
  /* normalize . and .. segments */
  src = r;
  out = r;
  while (*src) {
    if (src[0] == '/' && src[1] == '/') { src++; continue; }
    if (src[0] == '/' && src[1] == '.' && (src[2] == '/' || !src[2])) { src += 2; continue; }
    if (src[0] == '/' && src[1] == '.' && src[2] == '.' && (src[3] == '/' || !src[3])) {
      src += 3;
      while (out > r && out[-1] != '/')
        out--;
      if (out > r)
        out--;
      continue;
    }
    *out++ = *src++;
  }
  *out = 0;
  if (!r[0])
    strcpy(r, "/");
  return r;
}

static const char *const legacy_main_ext[] = {
  "", ".js", ".json", ".node", "/index.js", "/index.json", "/index.node", ".js", ".json", ".node",
};

static JSValue fs_legacy_main_resolve(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  char *pkg = node_string_to_utf8(ctx, ARG(0), NULL), *initial = NULL, *base;
  int i;
  JSValue ret;
  if (!pkg)
    return JS_EXCEPTION;
  if (JS_IsString(ARG(1))) {
    char *main = node_string_to_utf8(ctx, ARG(1), NULL);
    initial = path_join(pkg, main);
    free(main);
    for (i = 0; i < 7; i++) {
      char *p = malloc(strlen(initial) + 16);
      bool ok;
      sprintf(p, "%s%s", initial, legacy_main_ext[i]);
      ok = is_file(p);
      free(p);
      if (ok) {
        free(pkg);
        free(initial);
        return JS_NewInt32(ctx, i);
      }
    }
  }
  base = path_join(pkg, "./index");
  for (i = 7; i < 10; i++) {
    char *p = malloc(strlen(base) + 16);
    bool ok;
    sprintf(p, "%s%s", base, legacy_main_ext[i]);
    ok = is_file(p);
    free(p);
    if (ok) {
      free(pkg);
      free(initial);
      free(base);
      return JS_NewInt32(ctx, i);
    }
  }
  if (!initial) {
    initial = malloc(strlen(base) + 4);
    sprintf(initial, "%s.js", base);
  }
  {
    const char *b = NULL;
    char *bs = JS_IsString(ARG(2)) ? node_string_to_utf8(ctx, ARG(2), NULL) : NULL;
    if (!bs) {
      ret = node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
          "The \"base\" argument must be of type string or an instance of URL.");
    } else {
      b = bs;
      if (!strncmp(b, "file://", 7))
        b += 7;
      ret = node_throw_error(ctx, "ERR_MODULE_NOT_FOUND",
                             "Cannot find package '%s' imported from %s", initial, b);
    }
    free(bs);
  }
  free(pkg);
  free(initial);
  free(base);
  return ret;
}

/* ---- fs.cpSync helpers ---- */

static JSValue fs_cp_sync_check_paths(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  struct stat ss, ds;
  bool deref = JS_ToBool(ctx, ARG(2)), recursive = JS_ToBool(ctx, ARG(3));
  int sr, dr;
  JSValue ret = JS_UNDEFINED;
  char *src = path_arg(ctx, ARG(0)), *dest;
  if (!src)
    return JS_EXCEPTION;
  dest = path_arg(ctx, ARG(1));
  sr = deref ? stat(src, &ss) : lstat(src, &ss);
  dr = deref ? stat(dest, &ds) : lstat(dest, &ds);
  if (sr < 0) {
    ret = SYNC_THROW(-errno, "cp", src, NULL);
  } else if (S_ISDIR(ss.st_mode) && !recursive) {
    ret = node_throw_error(ctx, "ERR_FS_EISDIR", "Recursive option is required to copy a directory: %s", src);
  } else if (dr == 0) {
    if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino)
      ret = node_throw_error(ctx, "ERR_FS_CP_EINVAL", "src and dest cannot be the same %s", dest);
    else if (S_ISDIR(ss.st_mode) && !S_ISDIR(ds.st_mode))
      ret = node_throw_error(ctx, "ERR_FS_CP_DIR_TO_NON_DIR",
                             "cannot overwrite non-directory %s with directory %s", dest, src);
    else if (!S_ISDIR(ss.st_mode) && S_ISDIR(ds.st_mode))
      ret = node_throw_error(ctx, "ERR_FS_CP_NON_DIR_TO_DIR",
                             "cannot overwrite directory %s with non-directory %s", dest, src);
  }
  if (!JS_IsException(ret) && S_ISDIR(ss.st_mode)) {
    size_t l = strlen(src);
    if (!strncmp(dest, src, l) && (dest[l] == '/' || !dest[l]) && strcmp(dest, src))
      ret = node_throw_error(ctx, "ERR_FS_CP_EINVAL",
                             "cannot copy %s to a subdirectory of self %s", src, dest);
  }
  free(src);
  free(dest);
  return ret;
}

static int copy_file_contents(const char *src, const char *dest, mode_t mode, bool preserve) {
  uv_fs_t req;
  int err = uv_fs_copyfile(NULL, &req, src, dest, 0, NULL);
  uv_fs_req_cleanup(&req);
  if (err < 0)
    return err;
  chmod(dest, mode & 07777);
  if (preserve) {
    struct stat st;
    if (stat(src, &st) == 0) {
      uv_fs_utime(NULL, &req, dest, (double)st.st_atime, (double)st.st_mtime, NULL);
      uv_fs_req_cleanup(&req);
    }
  }
  return 0;
}

static JSValue fs_cp_sync_override_file(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  char *src = path_arg(ctx, ARG(0)), *dest = path_arg(ctx, ARG(1));
  int32_t mode = src && dest ? valid_file_mode(ctx, ARG(2), true) : 0;
  bool preserve = JS_ToBool(ctx, ARG(3));
  int err;
  JSValue ret = JS_UNDEFINED;
  if (!src || !dest || mode < 0) {
    free(src);
    free(dest);
    return JS_EXCEPTION;
  }
  unlink(dest);
  err = copy_file_contents(src, dest, mode, preserve);
  if (err < 0)
    ret = SYNC_THROW(err, "cp", src, dest);
  free(src);
  free(dest);
  return ret;
}

static int copy_dir(const char *src, const char *dest, bool force, bool deref,
                    bool error_on_exist, bool verbatim, bool preserve, char **errpath) {
  DIR *d = opendir(src);
  struct dirent *e;
  struct stat st;
  int err = 0;
  if (!d)
    return -errno;
  if (stat(src, &st) == 0)
    mkdir(dest, st.st_mode & 07777);
  while ((e = readdir(d))) {
    char *s, *t;
    struct stat es, ts;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    s = malloc(strlen(src) + strlen(e->d_name) + 2);
    t = malloc(strlen(dest) + strlen(e->d_name) + 2);
    sprintf(s, "%s/%s", src, e->d_name);
    sprintf(t, "%s/%s", dest, e->d_name);
    if ((deref ? stat(s, &es) : lstat(s, &es)) < 0) {
      err = -errno;
    } else if (S_ISDIR(es.st_mode)) {
      err = copy_dir(s, t, force, deref, error_on_exist, verbatim, preserve, errpath);
    } else if (S_ISLNK(es.st_mode)) {
      char buf[4096];
      ssize_t n = readlink(s, buf, sizeof(buf) - 1);
      if (n >= 0) {
        buf[n] = 0;
        if (lstat(t, &ts) == 0) {
          if (force)
            unlink(t);
          else if (error_on_exist) {
            err = -EEXIST;
            *errpath = strdup(t);
          }
        }
        if (!err && symlink(buf, t) < 0 && errno != EEXIST)
          err = -errno;
      }
    } else {
      if (lstat(t, &ts) == 0) {
        if (force) {
          unlink(t);
          err = copy_file_contents(s, t, es.st_mode, preserve);
        } else if (error_on_exist) {
          err = -EEXIST;
          *errpath = strdup(t);
        }
      } else {
        err = copy_file_contents(s, t, es.st_mode, preserve);
      }
    }
    if (err < 0 && !*errpath)
      *errpath = strdup(s);
    free(s);
    free(t);
    if (err < 0)
      break;
  }
  closedir(d);
  return err;
}

static JSValue fs_cp_sync_copy_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  char *src = path_arg(ctx, ARG(0)), *dest = path_arg(ctx, ARG(1)), *errpath = NULL;
  int err;
  JSValue ret = JS_UNDEFINED;
  if (!src || !dest) {
    free(src);
    free(dest);
    return JS_EXCEPTION;
  }
  err = copy_dir(src, dest, JS_ToBool(ctx, ARG(2)), JS_ToBool(ctx, ARG(3)),
                 JS_ToBool(ctx, ARG(4)), JS_ToBool(ctx, ARG(5)), JS_ToBool(ctx, ARG(6)),
                 &errpath);
  if (err == -EEXIST && errpath)
    ret = node_throw_error(ctx, "ERR_FS_CP_EEXIST", "Target already exists: cp returned EEXIST (%s already exists)", errpath);
  else if (err < 0)
    ret = SYNC_THROW(err, "cp", errpath ? errpath : src, NULL);
  free(src);
  free(dest);
  free(errpath);
  return ret;
}

static JSValue fs_handle_to_fd(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  return node_throw_error(ctx, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM", "Windows handles are not available");
}

/* ---------------------------------------------------------------------- */
/* FSReqCallback */

static void req_callback_finalizer(JSRuntime *rt, JSValueConst val) {
  ReqCallback *rc = JS_GetOpaque(val, req_callback_class_id);
  if (rc) {
    async_wrap_destroy(&rc->aw);
    free(rc);
  }
}

static JSValue req_callback_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                 JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, new_target, req_callback_class_id);
  ReqCallback *rc;
  if (JS_IsException(obj))
    return obj;
  rc = calloc(1, sizeof(*rc));
  rc->bigint = JS_ToBool(ctx, ARG(0));
  async_wrap_init(&rc->aw, env_get(ctx), obj, PROVIDER_FSREQCALLBACK, -1);
  JS_SetOpaque(obj, rc);
  return obj;
}

static JSValue req_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ReqCallback *rc = JS_GetOpaque(this_val, req_callback_class_id);
  return JS_NewFloat64(ctx, rc ? rc->aw.async_id : -1);
}

static const JSCFunctionListEntry req_callback_proto[] = {
  JS_CFUNC_DEF("getAsyncId", 0, req_get_async_id),
};

/* ---------------------------------------------------------------------- */
/* FileHandle (fs/promises) */

typedef struct {
  AsyncWrap aw;
  int fd;
  bool closing, closed;
  int64_t read_offset, read_length; /* for reading it as a stream (-1: as it goes / all) */
} FileHandle;

static void file_handle_finalizer(JSRuntime *rt, JSValueConst val) {
  FileHandle *h = JS_GetOpaque(val, file_handle_class_id);
  if (h) {
    if (!h->closed && !h->closing && h->fd >= 0) {
      /* Node closes leaked handles with a warning; close quietly */
      close(h->fd);
    }
    async_wrap_destroy(&h->aw);
    free(h);
  }
}

static JSValue file_handle_new(Env *env, int fd) {
  JSContext *ctx = env->ctx;
  JSValue proto = JS_GetClassProto(ctx, file_handle_class_id);
  JSValue obj = JS_NewObjectProtoClass(ctx, proto, file_handle_class_id);
  FileHandle *h = calloc(1, sizeof(*h));
  JS_FreeValue(ctx, proto);
  h->fd = fd;
  async_wrap_init(&h->aw, env, obj, PROVIDER_FILEHANDLE, -1);
  JS_SetOpaque(obj, h);
  return obj;
}

static JSValue file_handle_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, new_target, file_handle_class_id);
  FileHandle *h;
  if (JS_IsException(obj))
    return obj;
  h = calloc(1, sizeof(*h));
  h->fd = nb_int32(ctx, ARG(0), -1);
  h->read_offset = JS_IsNumber(ARG(1)) ? (int64_t)nb_double(ctx, ARG(1), -1) : -1;
  h->read_length = JS_IsNumber(ARG(2)) ? (int64_t)nb_double(ctx, ARG(2), -1) : -1;
  async_wrap_init(&h->aw, env_get(ctx), obj, PROVIDER_FILEHANDLE, -1);
  JS_SetOpaque(obj, h);
  return obj;
}

static JSValue file_handle_fd(JSContext *ctx, JSValueConst this_val) {
  FileHandle *h = JS_GetOpaque(this_val, file_handle_class_id);
  return JS_NewInt32(ctx, h ? h->fd : -1);
}

typedef struct {
  uv_fs_t req;
  Env *env;
  JSValue resolve, reject, handle;
} CloseReq;

static void file_handle_close_cb(uv_fs_t *req) {
  CloseReq *c = req->data;
  Env *env = c->env;
  JSContext *ctx = env->ctx;
  FileHandle *h = JS_GetOpaque(c->handle, file_handle_class_id);
  JSValue r, arg;
  if (h) {
    h->closed = true;
    h->closing = false;
  }
  if (req->result < 0) {
    arg = fs_exception(ctx, (int)req->result, "close", NULL, NULL);
    r = node_make_callback(env, c->handle, JS_UNDEFINED, c->reject, 1, (JSValueConst *)&arg,
                           h ? h->aw.async_id : 0, h ? h->aw.trigger_async_id : 0, JS_UNDEFINED);
    JS_FreeValue(ctx, arg);
  } else {
    arg = JS_UNDEFINED;
    r = node_make_callback(env, c->handle, JS_UNDEFINED, c->resolve, 1, (JSValueConst *)&arg,
                           h ? h->aw.async_id : 0, h ? h->aw.trigger_async_id : 0, JS_UNDEFINED);
  }
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, c->resolve);
  JS_FreeValue(ctx, c->reject);
  JS_FreeValue(ctx, c->handle);
  uv_fs_req_cleanup(req);
  free(c);
}

static JSValue file_handle_close(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  Env *env = env_get(ctx);
  FileHandle *h = JS_GetOpaque2(ctx, this_val, file_handle_class_id);
  CloseReq *c;
  JSValue res[2], p;
  int err;
  if (!h)
    return JS_EXCEPTION;
  p = JS_NewPromiseCapability(ctx, res);
  if (h->closed || h->closing || h->fd < 0) {
    JSValue u = JS_UNDEFINED;
    JS_FreeValue(ctx, JS_Call(ctx, res[0], JS_UNDEFINED, 1, (JSValueConst *)&u));
    JS_FreeValue(ctx, res[0]);
    JS_FreeValue(ctx, res[1]);
    return p;
  }
  h->closing = true;
  c = calloc(1, sizeof(*c));
  c->env = env;
  c->resolve = res[0];
  c->reject = res[1];
  c->handle = JS_DupValue(ctx, this_val);
  c->req.data = c;
  err = uv_fs_close(env->loop, &c->req, h->fd, file_handle_close_cb);
  if (err < 0) {
    c->req.result = err;
    file_handle_close_cb(&c->req);
  }
  return p;
}

static JSValue file_handle_release_fd(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  FileHandle *h = JS_GetOpaque2(ctx, this_val, file_handle_class_id);
  int fd;
  if (!h)
    return JS_EXCEPTION;
  fd = h->fd;
  h->fd = -1;
  h->closed = true;
  return JS_NewInt32(ctx, fd);
}

/* closeSync(): FileHandle::CloseSync */
static JSValue file_handle_close_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  FileHandle *h = JS_GetOpaque2(ctx, this_val, file_handle_class_id);
  uv_fs_t req;
  int err;
  if (!h)
    return JS_EXCEPTION;
  if (h->closed || h->closing || h->fd < 0)
    return JS_UNDEFINED;
  err = uv_fs_close(NULL, &req, h->fd, NULL);
  uv_fs_req_cleanup(&req);
  h->closed = true;
  if (err < 0)
    return SYNC_THROW(err, "close", NULL, NULL);
  return JS_UNDEFINED;
}

static JSValue file_handle_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  FileHandle *h = JS_GetOpaque(this_val, file_handle_class_id);
  return JS_NewFloat64(ctx, h ? h->aw.async_id : -1);
}

/* for StreamPipe: the descriptor, and where and how much to read */
bool node_file_handle_read_info(JSValueConst obj, int *fd, int64_t *offset, int64_t *length) {
  FileHandle *h = JS_GetOpaque(obj, file_handle_class_id);
  if (!h || h->closed || h->closing || h->fd < 0)
    return false;
  *fd = h->fd;
  *offset = h->read_offset;
  *length = h->read_length;
  return true;
}

static const JSCFunctionListEntry file_handle_proto[] = {
  JS_CGETSET_DEF("fd", file_handle_fd, NULL),
  JS_CFUNC_DEF("close", 0, file_handle_close),
  JS_CFUNC_DEF("closeSync", 0, file_handle_close_sync),
  JS_CFUNC_DEF("releaseFD", 0, file_handle_release_fd),
  JS_CFUNC_DEF("getAsyncId", 0, file_handle_get_async_id),
};

/* ---------------------------------------------------------------------- */
/* StatWatcher (fs.watchFile) */

typedef struct {
  AsyncWrap aw;
  uv_fs_poll_t poll;
  bool bigint, active, open;
} StatWatcher;

static void stat_watcher_free(uv_handle_t *h) {
  free((char *)h - offsetof(StatWatcher, poll));
}

static void stat_watcher_finalizer(JSRuntime *rt, JSValueConst val) {
  StatWatcher *w = JS_GetOpaque(val, stat_watcher_class_id);
  if (w) {
    async_wrap_destroy(&w->aw);
    w->poll.data = NULL;
    if (w->open && uv_is_closing((uv_handle_t *)&w->poll)) {
      /* closed with the rest of a worker's loop: the loop may still refer to it */
    } else if (w->open) {
      w->open = false;
      uv_close((uv_handle_t *)&w->poll, stat_watcher_free);
    } else {
      free(w);
    }
  }
}

static void stat_watcher_cb(uv_fs_poll_t *h, int status, const uv_stat_t *prev,
                            const uv_stat_t *curr) {
  StatWatcher *w = h->data;
  JSContext *ctx;
  JSValue args[2], arr, r;
  if (!w)
    return;
  ctx = w->aw.env->ctx;
  {
    FsBindingData *fsd = fsd_of(w->aw.env);
    fill_stats(fsd->stat_values, fsd->bigint_stat_values, curr, w->bigint);
    fill_stats(fsd->stat_values + kFsStatsFieldsNumber,
               fsd->bigint_stat_values + kFsStatsFieldsNumber, prev, w->bigint);
    arr = JS_DupValue(ctx, w->bigint ? fsd->bigint_stat_values_v : fsd->stat_values_v);
  }
  args[0] = JS_NewInt32(ctx, status);
  args[1] = arr;
  r = async_wrap_make_callback_name(&w->aw, "onchange", 2, (JSValueConst *)args);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, arr);
}

static JSValue stat_watcher_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                 JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, new_target, stat_watcher_class_id);
  StatWatcher *w;
  if (JS_IsException(obj))
    return obj;
  w = calloc(1, sizeof(*w));
  w->bigint = JS_ToBool(ctx, ARG(0));
  async_wrap_init(&w->aw, env, obj, PROVIDER_STATWATCHER, -1);
  uv_fs_poll_init(env->loop, &w->poll);
  w->poll.data = w;
  w->open = true;
  JS_SetOpaque(obj, w);
  return obj;
}

static JSValue stat_watcher_start(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque2(ctx, this_val, stat_watcher_class_id);
  char *path;
  int err;
  if (!w)
    return JS_EXCEPTION;
  if (w->active)
    return JS_NewInt32(ctx, 0);
  path = path_arg(ctx, ARG(0));
  if (!path)
    return JS_EXCEPTION;
  err = uv_fs_poll_start(&w->poll, stat_watcher_cb, path, nb_uint32(ctx, ARG(1), 5007));
  free(path);
  if (err == 0) {
    w->active = true;
    async_wrap_ref(&w->aw);
  }
  return JS_NewInt32(ctx, err);
}

static JSValue stat_watcher_close(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque2(ctx, this_val, stat_watcher_class_id);
  if (!w)
    return JS_EXCEPTION;
  if (w->active) {
    uv_fs_poll_stop(&w->poll);
    w->active = false;
    async_wrap_unref(&w->aw);
  }
  return JS_UNDEFINED;
}

static JSValue stat_watcher_ref(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque2(ctx, this_val, stat_watcher_class_id);
  if (w && w->open)
    uv_ref((uv_handle_t *)&w->poll);
  return JS_DupValue(ctx, this_val);
}

static JSValue stat_watcher_unref(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque2(ctx, this_val, stat_watcher_class_id);
  if (w && w->open)
    uv_unref((uv_handle_t *)&w->poll);
  return JS_DupValue(ctx, this_val);
}

static JSValue stat_watcher_has_ref(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque2(ctx, this_val, stat_watcher_class_id);
  return JS_NewBool(ctx, w && w->open && uv_has_ref((uv_handle_t *)&w->poll));
}

static JSValue stat_watcher_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  StatWatcher *w = JS_GetOpaque(this_val, stat_watcher_class_id);
  return JS_NewFloat64(ctx, w ? w->aw.async_id : -1);
}

static const JSCFunctionListEntry stat_watcher_proto[] = {
  JS_CFUNC_DEF("getAsyncId", 0, stat_watcher_get_async_id),
  JS_CFUNC_DEF("start", 2, stat_watcher_start),
  JS_CFUNC_DEF("close", 0, stat_watcher_close),
  JS_CFUNC_DEF("ref", 0, stat_watcher_ref),
  JS_CFUNC_DEF("unref", 0, stat_watcher_unref),
  JS_CFUNC_DEF("hasRef", 0, stat_watcher_has_ref),
};

/* ---------------------------------------------------------------------- */

JSValue binding_init_fs(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC("access", 3, fs_access),
    NB_FUNC("close", 2, fs_close),
    NB_FUNC("existsSync", 1, fs_exists_sync),
    NB_FUNC("open", 4, fs_open),
    NB_FUNC("openFileHandle", 5, fs_open_file_handle),
    NB_FUNC("read", 6, fs_read),
    NB_FUNC("readBuffers", 4, fs_read_buffers),
    NB_FUNC("readFileUtf8", 2, fs_read_file_utf8),
    NB_FUNC("fdatasync", 2, fs_fdatasync),
    NB_FUNC("fsync", 2, fs_fsync),
    NB_FUNC("rename", 3, fs_rename),
    NB_FUNC("ftruncate", 3, fs_ftruncate),
    NB_FUNC("rmdir", 2, fs_rmdir),
    NB_FUNC("rmSync", 4, fs_rm_sync),
    NB_FUNC("mkdir", 4, fs_mkdir),
    NB_FUNC("readdir", 4, fs_readdir),
    NB_FUNC("internalModuleStat", 1, fs_internal_module_stat),
    NB_FUNC("stat", 4, fs_stat),
    NB_FUNC("lstat", 4, fs_lstat),
    NB_FUNC("fstat", 4, fs_fstat),
    NB_FUNC("statfs", 3, fs_statfs),
    NB_FUNC("link", 3, fs_link),
    NB_FUNC("symlink", 4, fs_symlink),
    NB_FUNC("readlink", 3, fs_readlink),
    NB_FUNC("unlink", 2, fs_unlink),
    NB_FUNC("writeBuffer", 7, fs_write_buffer),
    NB_FUNC("writeBuffers", 4, fs_write_buffers),
    NB_FUNC("writeString", 6, fs_write_string),
    NB_FUNC("writeFileUtf8", 4, fs_write_file_utf8),
    NB_FUNC("realpath", 3, fs_realpath),
    NB_FUNC("copyFile", 4, fs_copy_file),
    NB_FUNC("chmod", 3, fs_chmod),
    NB_FUNC("fchmod", 3, fs_fchmod),
    NB_FUNC("chown", 4, fs_chown),
    NB_FUNC("fchown", 4, fs_fchown),
    NB_FUNC("lchown", 4, fs_lchown),
    NB_FUNC("utimes", 4, fs_utimes),
    NB_FUNC("futimes", 4, fs_futimes),
    NB_FUNC("lutimes", 4, fs_lutimes),
    NB_FUNC("mkdtemp", 3, fs_mkdtemp),
    NB_FUNC("getFormatOfExtensionlessFile", 1, fs_get_format_of_extensionless_file),
    NB_FUNC("legacyMainResolve", 3, fs_legacy_main_resolve),
    NB_FUNC("cpSyncCheckPaths", 4, fs_cp_sync_check_paths),
    NB_FUNC("cpSyncOverrideFile", 4, fs_cp_sync_override_file),
    NB_FUNC("cpSyncCopyDir", 7, fs_cp_sync_copy_dir),
    NB_FUNC("handleToFd", 2, fs_handle_to_fd),
  };
  NodeClassDef rdef = { .name = "FSReqCallback", .class_id = &req_callback_class_id,
                        .ctor = req_callback_ctor, .ctor_length = 1,
                        .finalizer = req_callback_finalizer, .proto_funcs = req_callback_proto,
                        .proto_funcs_count = countof(req_callback_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef hdef = { .name = "FileHandle", .class_id = &file_handle_class_id,
                        .ctor = file_handle_ctor, .ctor_length = 1,
                        .finalizer = file_handle_finalizer, .proto_funcs = file_handle_proto,
                        .proto_funcs_count = countof(file_handle_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef wdef = { .name = "StatWatcher", .class_id = &stat_watcher_class_id,
                        .ctor = stat_watcher_ctor, .ctor_length = 1,
                        .finalizer = stat_watcher_finalizer, .proto_funcs = stat_watcher_proto,
                        .proto_funcs_count = countof(stat_watcher_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &rdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &hdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &wdef));
  nb_set_int(ctx, t, "kFsStatsFieldsNumber", kFsStatsFieldsNumber);
  nb_set(ctx, t, "kUsePromises", env_get_symbol(env, "fs_use_promises_symbol"));
  {
    FsBindingData *fsd = fsd_of(env);
    nb_set(ctx, t, "statValues", JS_DupValue(ctx, fsd->stat_values_v));
    nb_set(ctx, t, "bigintStatValues", JS_DupValue(ctx, fsd->bigint_stat_values_v));
    nb_set(ctx, t, "statFsValues", JS_DupValue(ctx, fsd->statfs_values_v));
    nb_set(ctx, t, "bigintStatFsValues", JS_DupValue(ctx, fsd->bigint_statfs_values_v));
  }
  return t;
}

/* ---------------------------------------------------------------------- */
/* fs_dir: opendir */

typedef struct {
  AsyncWrap aw;
  uv_dir_t *dir;
  uv_dirent_t dirents[32];
  bool closed;
} DirHandle;

static void dir_finalizer(JSRuntime *rt, JSValueConst val) {
  DirHandle *d = JS_GetOpaque(val, fs_dir_class_id);
  if (d) {
    if (d->dir && !d->closed) {
      uv_fs_t req;
      uv_fs_closedir(NULL, &req, d->dir, NULL);
      uv_fs_req_cleanup(&req);
    }
    async_wrap_destroy(&d->aw);
    free(d);
  }
}

static JSValue dir_new(Env *env, uv_dir_t *dir) {
  JSContext *ctx = env->ctx;
  JSValue proto = JS_GetClassProto(ctx, fs_dir_class_id);
  JSValue obj = JS_NewObjectProtoClass(ctx, proto, fs_dir_class_id);
  DirHandle *d = calloc(1, sizeof(*d));
  JS_FreeValue(ctx, proto);
  d->dir = dir;
  dir->dirents = d->dirents;
  dir->nentries = countof(d->dirents);
  async_wrap_init(&d->aw, env, obj, PROVIDER_DIRHANDLE, -1);
  JS_SetOpaque(obj, d);
  return obj;
}

static JSValue dir_entries(JSContext *ctx, DirHandle *d, int n, int enc) {
  JSValue arr = JS_NewArray(ctx);
  int i;
  for (i = 0; i < n; i++) {
    JS_SetPropertyUint32(ctx, arr, 2 * i,
                         node_string_encode(ctx, (const uint8_t *)d->dirents[i].name,
                                            strlen(d->dirents[i].name), enc));
    JS_SetPropertyUint32(ctx, arr, 2 * i + 1, JS_NewInt32(ctx, d->dirents[i].type));
  }
  return arr;
}

typedef struct {
  uv_fs_t req;
  Env *env;
  JSValue target;
  JSValue resolve, reject;
  bool is_promise;
  JSValue handle;
  int enc;
  int op; /* 0 open, 1 read, 2 close */
  char *path;
} DirReq;

static void dir_req_done(uv_fs_t *req) {
  DirReq *r = req->data;
  Env *env = r->env;
  JSContext *ctx = env->ctx;
  JSValue err = JS_NULL, result = JS_UNDEFINED;
  static const char *const names[] = { "opendir", "scandir", "closedir" };
  if (req->result < 0) {
    err = fs_exception(ctx, (int)req->result, names[r->op], r->path, NULL);
  } else if (r->op == 0) {
    result = dir_new(env, req->ptr);
  } else if (r->op == 1) {
    DirHandle *d = JS_GetOpaque(r->handle, fs_dir_class_id);
    result = req->result == 0 ? JS_NULL : dir_entries(ctx, d, (int)req->result, r->enc);
  } else {
    DirHandle *d = JS_GetOpaque(r->handle, fs_dir_class_id);
    if (d)
      d->closed = true;
  }
  if (r->is_promise) {
    JSValue arg = JS_IsNull(err) ? result : err;
    JSValue ret = JS_Call(ctx, JS_IsNull(err) ? r->resolve : r->reject, JS_UNDEFINED, 1,
                          (JSValueConst *)&arg);
    JS_FreeValue(ctx, ret);
    node_run_microtasks(env);
  } else {
    ReqCallback *rc = JS_GetOpaque(r->target, req_callback_class_id);
    JSValue args[2] = { err, result }, ret;
    if (rc) {
      ret = async_wrap_make_callback_name(&rc->aw, "oncomplete", 2, (JSValueConst *)args);
      JS_FreeValue(ctx, ret);
    }
  }
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, r->target);
  JS_FreeValue(ctx, r->resolve);
  JS_FreeValue(ctx, r->reject);
  JS_FreeValue(ctx, r->handle);
  free(r->path);
  uv_fs_req_cleanup(req);
  free(r);
}

static DirReq *dir_req(JSContext *ctx, JSValueConst reqv, int op, JSValue *ret) {
  DirReq *r;
  *ret = JS_UNDEFINED;
  if (JS_IsUndefined(reqv))
    return NULL;
  r = calloc(1, sizeof(*r));
  r->env = env_get(ctx);
  r->op = op;
  r->target = JS_UNDEFINED;
  r->resolve = r->reject = r->handle = JS_UNDEFINED;
  r->req.data = r;
  if (JS_IsSymbol(reqv)) {
    JSValue res[2];
    *ret = JS_NewPromiseCapability(ctx, res);
    r->resolve = res[0];
    r->reject = res[1];
    r->is_promise = true;
  } else {
    r->target = JS_DupValue(ctx, reqv);
  }
  return r;
}

/* opendir(path, encoding, req) */
static JSValue fsd_opendir(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue ret;
  DirReq *r;
  PATH_OR_FAIL(path, ARG(0));
  r = argc > 2 ? dir_req(ctx, ARG(2), 0, &ret) : NULL;
  if (r) {
    int err;
    r->path = path;
    err = uv_fs_opendir(env->loop, &r->req, path, dir_req_done);
    if (err < 0) {
      r->req.result = err;
      dir_req_done(&r->req);
    }
    return ret;
  } else {
    uv_fs_t req;
    int err = uv_fs_opendir(NULL, &req, path, NULL);
    if (err < 0) {
      uv_fs_req_cleanup(&req);
      ret = SYNC_THROW(err, "opendir", path, NULL);
      free(path);
      return ret;
    }
    free(path);
    ret = dir_new(env, req.ptr);
    uv_fs_req_cleanup(&req);
    return ret;
  }
}

/* handle.read(encoding, bufferSize, req) */
static JSValue dir_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  DirHandle *d = JS_GetOpaque2(ctx, this_val, fs_dir_class_id);
  int enc = node_parse_encoding(ctx, ARG(0), ENC_UTF8);
  JSValue ret;
  DirReq *r;
  if (!d)
    return JS_EXCEPTION;
  r = argc > 2 ? dir_req(ctx, ARG(2), 1, &ret) : NULL;
  if (r) {
    int err;
    r->enc = enc;
    r->handle = JS_DupValue(ctx, this_val);
    err = uv_fs_readdir(r->env->loop, &r->req, d->dir, dir_req_done);
    if (err < 0) {
      r->req.result = err;
      dir_req_done(&r->req);
    }
    return ret;
  } else {
    uv_fs_t req;
    int n = uv_fs_readdir(NULL, &req, d->dir, NULL);
    JSValue entries;
    if (n < 0) {
      uv_fs_req_cleanup(&req);
      return SYNC_THROW(n, "scandir", NULL, NULL);
    }
    /* the names are the request's until it is cleaned up */
    entries = n == 0 ? JS_NULL : dir_entries(ctx, d, n, enc);
    uv_fs_req_cleanup(&req);
    return entries;
  }
}

static JSValue dir_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  DirHandle *d = JS_GetOpaque2(ctx, this_val, fs_dir_class_id);
  JSValue ret;
  DirReq *r;
  if (!d)
    return JS_EXCEPTION;
  r = argc > 0 ? dir_req(ctx, ARG(0), 2, &ret) : NULL;
  if (r) {
    int err;
    r->handle = JS_DupValue(ctx, this_val);
    err = uv_fs_closedir(r->env->loop, &r->req, d->dir, dir_req_done);
    if (err < 0) {
      r->req.result = err;
      dir_req_done(&r->req);
    }
    return ret;
  } else {
    uv_fs_t req;
    int err = uv_fs_closedir(NULL, &req, d->dir, NULL);
    uv_fs_req_cleanup(&req);
    d->closed = true;
    if (err < 0)
      return SYNC_THROW(err, "closedir", NULL, NULL);
    return JS_UNDEFINED;
  }
}

static JSValue dir_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static const JSCFunctionListEntry dir_proto[] = {
  JS_CFUNC_DEF("read", 3, dir_read),
  JS_CFUNC_DEF("close", 1, dir_close),
};

/* opendirSync(path): the handle, or throws */
static JSValue fsd_opendir_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  JSValue a[2] = { ARG(0), JS_UNDEFINED };
  return fsd_opendir(ctx, this_val, 2, a);
}

JSValue binding_init_fs_dir(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef def = { .name = "DirHandle", .class_id = &fs_dir_class_id, .ctor = dir_ctor,
                       .finalizer = dir_finalizer, .proto_funcs = dir_proto,
                       .proto_funcs_count = countof(dir_proto), .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  nb_set_method(ctx, t, "opendir", fsd_opendir, 3);
  nb_set_method(ctx, t, "opendirSync", fsd_opendir_sync, 1);
  return t;
}
