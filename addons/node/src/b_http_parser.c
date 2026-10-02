/* internalBinding('http_parser'): llhttp behind the HTTPParser class
 * (node_http_parser.cc), including consume() of a native stream. */
#include <stdlib.h>
#include <string.h>

#include "llhttp.h"
#include "node.h"
#include "streams.h"

enum {
  kOnMessageBegin = 0, kOnHeaders, kOnHeadersComplete, kOnBody,
  kOnMessageComplete, kOnExecute, kOnTimeout
};

#define kMaxHeaderFieldsCount 32
#define kMaxChunkExtensionsSize 16384

enum {
  kLenientNone = 0,
  kLenientHeaders = 1 << 0,
  kLenientChunkedLength = 1 << 1,
  kLenientKeepAlive = 1 << 2,
  kLenientTransferEncoding = 1 << 3,
  kLenientVersion = 1 << 4,
  kLenientDataAfterClose = 1 << 5,
  kLenientOptionalLFAfterCR = 1 << 6,
  kLenientOptionalCRLFAfterChunk = 1 << 7,
  kLenientOptionalCRBeforeLF = 1 << 8,
  kLenientSpacesAfterChunkSize = 1 << 9,
  kLenientHeaderValueRelaxed = 1 << 10,
  kLenientAll = (1 << 11) - 1,
};

static JSClassID parser_class_id, conns_class_id;

typedef struct {
  char *p;
  size_t len, cap;
} SBuf;

static void sb_append(SBuf *b, const char *at, size_t n) {
  if (b->len + n > b->cap) {
    size_t c = b->cap ? b->cap : 64;
    while (c < b->len + n)
      c *= 2;
    b->p = realloc(b->p, c);
    b->cap = c;
  }
  memcpy(b->p + b->len, at, n);
  b->len += n;
}

static JSValue sb_string(JSContext *ctx, const SBuf *b, bool trim) {
  size_t n = b->len;
  if (trim)
    while (n > 0 && (b->p[n - 1] == ' ' || b->p[n - 1] == '\t'))
      n--;
  return node_string_encode(ctx, (const uint8_t *)(b->p ? b->p : ""), n, ENC_LATIN1);
}

typedef struct ConnList ConnList;

typedef struct Parser {
  AsyncWrap aw;
  llhttp_t parser;
  SBuf fields[kMaxHeaderFieldsCount], values[kMaxHeaderFieldsCount], url, status;
  size_t num_fields, num_values;
  bool have_flushed, got_exception, is_being_freed, headers_completed, pending_pause;
  bool initialized;
  size_t header_pairs;
  double max_header_pairs;
  uint64_t header_nread, chunk_ext_nread, max_http_header_size, last_message_start;
  ConnList *conns;
  JSValue conns_obj; /* keeps the list alive while we are in it */
  const char *cur_data;
  size_t cur_len;
  StreamWrap *stream;
  JSValue stream_obj;
  void (*prev_alloc)(StreamWrap *, size_t, uv_buf_t *);
  void (*prev_read)(StreamWrap *, ssize_t, const uv_buf_t *);
  void *prev_listener;
} Parser;

struct ConnList {
  Parser **all, **active;
  size_t nall, nactive, call, cactive;
};

static void list_remove(Parser ***arr, size_t *n, Parser *p) {
  size_t i;
  for (i = 0; i < *n; i++)
    if ((*arr)[i] == p) {
      memmove(&(*arr)[i], &(*arr)[i + 1], (*n - i - 1) * sizeof(Parser *));
      (*n)--;
      return;
    }
}

static void list_add(Parser ***arr, size_t *n, size_t *cap, Parser *p) {
  size_t i;
  for (i = 0; i < *n; i++)
    if ((*arr)[i] == p)
      return;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *arr = realloc(*arr, *cap * sizeof(Parser *));
  }
  (*arr)[(*n)++] = p;
}

static void conns_push(ConnList *l, Parser *p) { list_add(&l->all, &l->nall, &l->call, p); }
static void conns_pop(ConnList *l, Parser *p) { list_remove(&l->all, &l->nall, p); }
static void conns_push_active(ConnList *l, Parser *p) {
  list_add(&l->active, &l->nactive, &l->cactive, p);
}
static void conns_pop_active(ConnList *l, Parser *p) {
  list_remove(&l->active, &l->nactive, p);
}

static void parser_detach_list(Parser *p) {
  if (p->conns) {
    conns_pop(p->conns, p);
    conns_pop_active(p->conns, p);
    p->conns = NULL;
    JS_FreeValue(p->aw.env->ctx, p->conns_obj);
    p->conns_obj = JS_UNDEFINED;
  }
}

/* ---- calling into JS ---- */

static JSValue parser_cb(Parser *p, int index) {
  return JS_GetPropertyUint32(p->aw.env->ctx, p->aw.object, index);
}

static bool cb_failed(JSValueConst r) {
  return JS_IsException(r) || JS_IsUninitialized(r);
}

/* InternalCallbackScope(kSkipTaskQueues) + Call */
static JSValue call_skip_tq(Parser *p, JSValueConst fn, int argc, JSValueConst *argv) {
  Env *env = p->aw.env;
  bool outer = env->callback_scope_depth == 0;
  JSValue r;
  env->callback_scope_depth++;
  r = async_wrap_make_callback(&p->aw, fn, argc, argv);
  env->callback_scope_depth--;
  if (JS_IsException(r) && outer) {
    node_trigger_uncaught_exception(env, JS_GetException(env->ctx), false);
    r = NODE_CB_FAILED;
  }
  return r;
}

static JSValue create_headers(Parser *p) {
  JSContext *ctx = p->aw.env->ctx;
  JSValue arr = JS_NewArray(ctx);
  size_t i;
  for (i = 0; i < p->num_values; i++) {
    JS_SetPropertyUint32(ctx, arr, (uint32_t)(i * 2), sb_string(ctx, &p->fields[i], false));
    JS_SetPropertyUint32(ctx, arr, (uint32_t)(i * 2 + 1), sb_string(ctx, &p->values[i], true));
  }
  return arr;
}

static void flush(Parser *p) {
  JSContext *ctx = p->aw.env->ctx;
  JSValue cb = parser_cb(p, kOnHeaders), args[2], r;
  if (!JS_IsFunction(ctx, cb)) {
    JS_FreeValue(ctx, cb);
    return;
  }
  args[0] = create_headers(p);
  args[1] = sb_string(ctx, &p->url, false);
  r = async_wrap_make_callback(&p->aw, cb, 2, (JSValueConst *)args);
  if (cb_failed(r))
    p->got_exception = true;
  else
    JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, cb);
  p->url.len = 0;
  p->have_flushed = true;
}

/* ---- llhttp callbacks ---- */

#define PARSER(l) ((Parser *)((char *)(l) - offsetof(Parser, parser)))

static int maybe_pause(Parser *p, int rv) {
  if (rv != 0 || !p->pending_pause)
    return rv;
  p->pending_pause = false;
  llhttp_set_error_reason(&p->parser, "Paused in callback");
  return HPE_PAUSED;
}

static int track_header(Parser *p, size_t len) {
  p->header_nread += len;
  if (p->header_nread >= p->max_http_header_size) {
    llhttp_set_error_reason(&p->parser, "HPE_HEADER_OVERFLOW:Header overflow");
    return HPE_USER;
  }
  return 0;
}

static int track_header_pair(Parser *p) {
  JSContext *ctx = p->aw.env->ctx;
  if (p->parser.type != HTTP_REQUEST)
    return 0;
  p->header_pairs += 2;
  if (p->max_header_pairs < 0) {
    JSValue v = JS_GetPropertyStr(ctx, p->aw.object, "maxHeaderPairs");
    double d = 0;
    if (JS_IsException(v)) {
      p->got_exception = true;
      return -1;
    }
    if (JS_IsNumber(v))
      JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    p->max_header_pairs = d > 0 ? d : 0;
  }
  if (p->max_header_pairs > 0 && p->header_pairs > p->max_header_pairs) {
    llhttp_set_error_reason(&p->parser, "HPE_HEADER_OVERFLOW:Header overflow");
    return HPE_USER;
  }
  return 0;
}

static int on_message_begin(llhttp_t *l) {
  Parser *p = PARSER(l);
  JSContext *ctx = p->aw.env->ctx;
  JSValue cb;
  if (p->is_being_freed)
    return 0;
  if (p->conns) {
    conns_pop(p->conns, p);
    conns_pop_active(p->conns, p);
  }
  p->num_fields = p->num_values = 0;
  p->headers_completed = false;
  p->chunk_ext_nread = 0;
  p->last_message_start = uv_hrtime();
  p->url.len = 0;
  p->status.len = 0;
  p->max_header_pairs = -1;
  if (p->conns) {
    conns_push(p->conns, p);
    conns_push_active(p->conns, p);
  }
  cb = parser_cb(p, kOnMessageBegin);
  if (JS_IsFunction(ctx, cb)) {
    JSValue r = call_skip_tq(p, cb, 0, NULL);
    if (!cb_failed(r))
      JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, cb);
  return maybe_pause(p, 0);
}

static int on_url(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  int rv;
  if (p->is_being_freed)
    return 0;
  if ((rv = track_header(p, len)) != 0)
    return rv;
  sb_append(&p->url, at, len);
  return maybe_pause(p, 0);
}

static int on_status(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  int rv;
  if (p->is_being_freed)
    return 0;
  if ((rv = track_header(p, len)) != 0)
    return rv;
  sb_append(&p->status, at, len);
  return maybe_pause(p, 0);
}

static int on_header_field(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  int rv;
  if (p->is_being_freed)
    return 0;
  if ((rv = track_header(p, len)) != 0)
    return rv;
  if (p->num_fields == p->num_values) {
    if ((rv = track_header_pair(p)) != 0)
      return rv;
    p->num_fields++;
    if (p->num_fields == kMaxHeaderFieldsCount) {
      flush(p);
      p->num_fields = 1;
      p->num_values = 0;
    }
    p->fields[p->num_fields - 1].len = 0;
  }
  sb_append(&p->fields[p->num_fields - 1], at, len);
  return maybe_pause(p, 0);
}

static int on_header_value(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  int rv;
  if (p->is_being_freed)
    return 0;
  if ((rv = track_header(p, len)) != 0)
    return rv;
  if (p->num_values != p->num_fields) {
    p->num_values++;
    p->values[p->num_values - 1].len = 0;
  }
  sb_append(&p->values[p->num_values - 1], at, len);
  return maybe_pause(p, 0);
}

static int on_headers_complete(llhttp_t *l) {
  Parser *p = PARSER(l);
  Env *env = p->aw.env;
  JSContext *ctx = env->ctx;
  JSValue cb, args[9], r;
  int i;
  double val;
  if (p->is_being_freed)
    return 0;
  p->headers_completed = true;
  p->header_nread = 0;
  cb = parser_cb(p, kOnHeadersComplete);
  if (!JS_IsFunction(ctx, cb)) {
    JS_FreeValue(ctx, cb);
    return maybe_pause(p, 0);
  }
  for (i = 0; i < 9; i++)
    args[i] = JS_UNDEFINED;
  if (p->have_flushed) {
    flush(p);
  } else {
    args[2] = create_headers(p);
    if (p->parser.type == HTTP_REQUEST)
      args[4] = sb_string(ctx, &p->url, false);
  }
  p->num_fields = p->num_values = 0;
  p->header_pairs = 0;
  p->max_header_pairs = -1;
  if (p->parser.type == HTTP_REQUEST)
    args[3] = JS_NewUint32(ctx, p->parser.method);
  if (p->parser.type == HTTP_RESPONSE) {
    args[5] = JS_NewInt32(ctx, p->parser.status_code);
    args[6] = sb_string(ctx, &p->status, false);
  }
  args[0] = JS_NewInt32(ctx, p->parser.http_major);
  args[1] = JS_NewInt32(ctx, p->parser.http_minor);
  args[7] = JS_NewBool(ctx, p->parser.upgrade);
  args[8] = JS_NewBool(ctx, llhttp_should_keep_alive(&p->parser));
  r = call_skip_tq(p, cb, 9, (JSValueConst *)args);
  for (i = 0; i < 9; i++)
    JS_FreeValue(ctx, args[i]);
  JS_FreeValue(ctx, cb);
  if (cb_failed(r)) {
    p->got_exception = true;
    return -1;
  }
  if (JS_ToFloat64(ctx, &val, r) < 0) {
    JS_FreeValue(ctx, r);
    p->got_exception = true;
    return -1;
  }
  JS_FreeValue(ctx, r);
  return maybe_pause(p, (int)val);
}

static int on_body(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  JSContext *ctx = p->aw.env->ctx;
  JSValue cb, buf, r;
  if (p->is_being_freed || len == 0)
    return 0;
  cb = parser_cb(p, kOnBody);
  if (!JS_IsFunction(ctx, cb)) {
    JS_FreeValue(ctx, cb);
    return maybe_pause(p, 0);
  }
  buf = nb_new_buffer(ctx, at, len);
  r = async_wrap_make_callback(&p->aw, cb, 1, (JSValueConst *)&buf);
  JS_FreeValue(ctx, buf);
  JS_FreeValue(ctx, cb);
  if (cb_failed(r)) {
    p->got_exception = true;
    llhttp_set_error_reason(&p->parser, "HPE_JS_EXCEPTION:JS Exception");
    return HPE_USER;
  }
  JS_FreeValue(ctx, r);
  return maybe_pause(p, 0);
}

static int on_message_complete(llhttp_t *l) {
  Parser *p = PARSER(l);
  JSContext *ctx = p->aw.env->ctx;
  JSValue cb, r;
  if (p->is_being_freed)
    return 0;
  if (p->conns) {
    conns_pop(p->conns, p);
    conns_pop_active(p->conns, p);
  }
  p->last_message_start = 0;
  if (p->conns)
    conns_push(p->conns, p);
  if (p->num_fields)
    flush(p);
  p->header_pairs = 0;
  cb = parser_cb(p, kOnMessageComplete);
  if (!JS_IsFunction(ctx, cb)) {
    JS_FreeValue(ctx, cb);
    return maybe_pause(p, 0);
  }
  r = call_skip_tq(p, cb, 0, NULL);
  JS_FreeValue(ctx, cb);
  if (cb_failed(r)) {
    p->got_exception = true;
    return -1;
  }
  JS_FreeValue(ctx, r);
  return maybe_pause(p, 0);
}

static int on_chunk_extension(llhttp_t *l, const char *at, size_t len) {
  Parser *p = PARSER(l);
  if (p->is_being_freed)
    return 0;
  p->chunk_ext_nread += len;
  if (p->chunk_ext_nread > kMaxChunkExtensionsSize) {
    llhttp_set_error_reason(&p->parser, "HPE_CHUNK_EXTENSIONS_OVERFLOW:Chunk extensions overflow");
    return HPE_USER;
  }
  return maybe_pause(p, 0);
}

static int on_chunk_header(llhttp_t *l) {
  Parser *p = PARSER(l);
  if (p->is_being_freed)
    return 0;
  p->header_nread = 0;
  p->chunk_ext_nread = 0;
  return maybe_pause(p, 0);
}

static int on_chunk_complete(llhttp_t *l) {
  Parser *p = PARSER(l);
  if (p->is_being_freed)
    return 0;
  p->header_nread = 0;
  return maybe_pause(p, 0);
}

static llhttp_settings_t settings;

static void init_settings(void) {
  static bool done;
  if (done)
    return;
  done = true;
  llhttp_settings_init(&settings);
  settings.on_message_begin = on_message_begin;
  settings.on_url = on_url;
  settings.on_status = on_status;
  settings.on_header_field = on_header_field;
  settings.on_header_value = on_header_value;
  settings.on_chunk_extension_name = on_chunk_extension;
  settings.on_chunk_extension_value = on_chunk_extension;
  settings.on_headers_complete = on_headers_complete;
  settings.on_body = on_body;
  settings.on_message_complete = on_message_complete;
  settings.on_chunk_header = on_chunk_header;
  settings.on_chunk_complete = on_chunk_complete;
}

/* Parser::Execute: nread, an Error, or JS_EXCEPTION / JS_UNDEFINED */
static JSValue parser_execute(Parser *p, const char *data, size_t len) {
  JSContext *ctx = p->aw.env->ctx;
  llhttp_errno_t err;
  size_t nread = len;
  p->cur_data = data;
  p->cur_len = len;
  p->got_exception = false;
  if (data == NULL)
    err = llhttp_finish(&p->parser);
  else
    err = llhttp_execute(&p->parser, data, len);
  if (err != HPE_OK) {
    if (data)
      nread = llhttp_get_error_pos(&p->parser) - data;
    if (err == HPE_PAUSED_UPGRADE) {
      err = HPE_OK;
      llhttp_resume_after_upgrade(&p->parser);
    }
  }
  if (p->pending_pause) {
    p->pending_pause = false;
    llhttp_pause(&p->parser);
  }
  p->cur_data = NULL;
  p->cur_len = 0;
  if (p->got_exception)
    return JS_EXCEPTION;
  if (!p->parser.upgrade && err != HPE_OK) {
    JSValue e = JS_NewError(ctx);
    const char *reason = llhttp_get_error_reason(&p->parser);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, "Parse Error"),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_SetPropertyStr(ctx, e, "bytesParsed", JS_NewFloat64(ctx, (double)nread));
    if (!reason)
      reason = "";
    if (err == HPE_USER) {
      const char *colon = strchr(reason, ':');
      if (colon) {
        JS_SetPropertyStr(ctx, e, "code", JS_NewStringLen(ctx, reason, colon - reason));
        JS_SetPropertyStr(ctx, e, "reason", JS_NewString(ctx, colon + 1));
      } else {
        JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "HPE_USER"));
        JS_SetPropertyStr(ctx, e, "reason", JS_NewString(ctx, reason));
      }
    } else {
      JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, llhttp_errno_name(err)));
      JS_SetPropertyStr(ctx, e, "reason", JS_NewString(ctx, reason));
    }
    return e;
  }
  if (data == NULL)
    return JS_UNDEFINED;
  return JS_NewFloat64(ctx, (double)nread);
}

/* ---- JS methods ---- */

static Parser *parser_of(JSContext *ctx, JSValueConst obj) {
  return JS_GetOpaque2(ctx, obj, parser_class_id);
}

static void parser_unconsume(Parser *p);

static void parser_free_bufs(Parser *p) {
  int i;
  for (i = 0; i < kMaxHeaderFieldsCount; i++) {
    free(p->fields[i].p);
    free(p->values[i].p);
  }
  free(p->url.p);
  free(p->status.p);
}

static void parser_finalizer(JSRuntime *rt, JSValueConst val) {
  Parser *p = JS_GetOpaque(val, parser_class_id);
  if (!p)
    return;
  parser_unconsume(p);
  if (p->conns) {
    conns_pop(p->conns, p);
    conns_pop_active(p->conns, p);
    JS_FreeValueRT(rt, p->conns_obj);
  }
  async_wrap_destroy(&p->aw);
  parser_free_bufs(p);
  free(p);
}

static JSValue parser_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, parser_class_id);
  Parser *p;
  if (JS_IsException(obj))
    return obj;
  init_settings();
  p = calloc(1, sizeof(*p));
  p->conns_obj = JS_UNDEFINED;
  p->stream_obj = JS_UNDEFINED;
  async_wrap_init(&p->aw, env_get(ctx), obj, PROVIDER_NONE, -1);
  JS_SetOpaque(obj, p);
  return obj;
}

static JSValue parser_close(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  if (!p)
    return JS_UNDEFINED;
  /* `delete parser`: the object stays but is no longer a parser */
  parser_unconsume(p);
  parser_detach_list(p);
  async_wrap_destroy(&p->aw);
  parser_free_bufs(p);
  JS_SetOpaque(this_val, NULL);
  free(p);
  return JS_UNDEFINED;
}

static JSValue parser_free(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  if (p && p->aw.async_id > 0) {
    /* EmitDestroy: queue the destroy hook, keep the wrap reusable */
    env_queue_destroy_async_id(p->aw.env, p->aw.async_id);
    p->aw.async_id = -1;
  }
  return JS_UNDEFINED;
}

static JSValue parser_remove(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  if (!p)
    return JS_UNDEFINED;
  p->is_being_freed = true;
  if (p->conns) {
    conns_pop(p->conns, p);
    conns_pop_active(p->conns, p);
  }
  return JS_UNDEFINED;
}

static JSValue parser_execute_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  Parser *p = parser_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  if (!p)
    return JS_EXCEPTION;
  d = nb_buffer_data(ctx, ARG(0), &len);
  if (!d)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "argument must be a buffer");
  return parser_execute(p, (const char *)d, len);
}

static JSValue parser_finish(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  Parser *p = parser_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  return parser_execute(p, NULL, 0);
}

/* initialize(type, resource, maxHeaderSize, lenientFlags, connectionsList) */
static JSValue parser_initialize(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  Env *env = env_get(ctx);
  Parser *p = parser_of(ctx, this_val);
  int32_t type = nb_int32(ctx, ARG(0), HTTP_REQUEST);
  double max = argc > 2 ? nb_double(ctx, ARG(2), 0) : 0;
  uint32_t lenient = argc > 3 ? nb_uint32(ctx, ARG(3), 0) : 0;
  ConnList *list = NULL;
  int i;
  if (!p)
    return JS_EXCEPTION;
  if (max == 0)
    max = node_option_number(env->options, "--max-http-header-size", 16384);
  if (argc > 4 && JS_IsObject(ARG(4)))
    list = JS_GetOpaque(ARG(4), conns_class_id);
  p->aw.provider = type == HTTP_REQUEST ? PROVIDER_HTTPINCOMINGMESSAGE
                                        : PROVIDER_HTTPCLIENTREQUEST;
  async_wrap_reset(&p->aw, ARG(1));

  llhttp_init(&p->parser, type, &settings);
  if (lenient & kLenientHeaders) llhttp_set_lenient_headers(&p->parser, 1);
  if (lenient & kLenientChunkedLength) llhttp_set_lenient_chunked_length(&p->parser, 1);
  if (lenient & kLenientKeepAlive) llhttp_set_lenient_keep_alive(&p->parser, 1);
  if (lenient & kLenientTransferEncoding) llhttp_set_lenient_transfer_encoding(&p->parser, 1);
  if (lenient & kLenientVersion) llhttp_set_lenient_version(&p->parser, 1);
  if (lenient & kLenientDataAfterClose) llhttp_set_lenient_data_after_close(&p->parser, 1);
  if (lenient & kLenientOptionalLFAfterCR) llhttp_set_lenient_optional_lf_after_cr(&p->parser, 1);
  if (lenient & kLenientOptionalCRLFAfterChunk)
    llhttp_set_lenient_optional_crlf_after_chunk(&p->parser, 1);
  if (lenient & kLenientOptionalCRBeforeLF) llhttp_set_lenient_optional_cr_before_lf(&p->parser, 1);
  if (lenient & kLenientSpacesAfterChunkSize)
    llhttp_set_lenient_spaces_after_chunk_size(&p->parser, 1);
  if (lenient & kLenientHeaderValueRelaxed)
    llhttp_set_lenient_header_value_relaxed(&p->parser, 1);

  p->header_nread = 0;
  p->url.len = p->status.len = 0;
  for (i = 0; i < kMaxHeaderFieldsCount; i++)
    p->fields[i].len = p->values[i].len = 0;
  p->num_fields = p->num_values = 0;
  p->have_flushed = p->got_exception = p->is_being_freed = p->headers_completed = false;
  p->max_http_header_size = (uint64_t)max;
  p->header_pairs = 0;
  p->max_header_pairs = -1;
  p->initialized = true;

  parser_detach_list(p);
  if (list) {
    p->conns = list;
    p->conns_obj = JS_DupValue(ctx, ARG(4));
    p->last_message_start = uv_hrtime();
    conns_push(list, p);
    conns_push_active(list, p);
  }
  return JS_UNDEFINED;
}

static JSValue parser_pause(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic) {
  Parser *p = parser_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  if (magic)
    llhttp_pause(&p->parser);
  else
    llhttp_resume(&p->parser);
  return JS_UNDEFINED;
}

/* consume(stream): the parser reads the native stream directly */
static void parser_stream_alloc(StreamWrap *s, size_t suggested, uv_buf_t *buf) {
  buf->base = malloc(65536);
  buf->len = buf->base ? 65536 : 0;
}

static void parser_stream_read(StreamWrap *s, ssize_t nread, const uv_buf_t *buf) {
  Parser *p = s->listener;
  JSContext *ctx;
  JSValue ret, cb;
  if (nread < 0) {
    /* PassReadErrorToPreviousListener */
    free(buf->base);
    if (p->prev_read)
      p->prev_read(s, nread, &(uv_buf_t){ NULL, 0 });
    else
      node_stream_emit_read_js(s, nread, NULL);
    return;
  }
  if (nread == 0) {
    free(buf->base);
    return;
  }
  ctx = p->aw.env->ctx;
  s->bytes_read += nread;
  JS_DupValue(ctx, p->aw.object);
  ret = parser_execute(p, buf->base, (size_t)nread);
  if (JS_IsException(ret)) {
    /* the callbacks threw at the outermost level */
    node_trigger_uncaught_exception(p->aw.env, JS_GetException(ctx), false);
    goto done;
  }
  cb = parser_cb(p, kOnExecute);
  if (JS_IsFunction(ctx, cb)) {
    JSValue r;
    p->cur_data = buf->base;
    p->cur_len = (size_t)nread;
    r = async_wrap_make_callback(&p->aw, cb, 1, (JSValueConst *)&ret);
    if (!cb_failed(r))
      JS_FreeValue(ctx, r);
    p->cur_data = NULL;
    p->cur_len = 0;
  }
  JS_FreeValue(ctx, cb);
  JS_FreeValue(ctx, ret);
done:
  free(buf->base);
  JS_FreeValue(ctx, p->aw.object);
}

static void parser_unconsume(Parser *p) {
  StreamWrap *s = p->stream;
  if (!s)
    return;
  s->alloc_override = p->prev_alloc;
  s->read_override = p->prev_read;
  s->listener = p->prev_listener;
  p->stream = NULL;
  JS_FreeValueRT(p->aw.env->rt, p->stream_obj);
  p->stream_obj = JS_UNDEFINED;
}

static JSValue parser_consume(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Parser *p = parser_of(ctx, this_val);
  StreamWrap *s;
  if (!p)
    return JS_EXCEPTION;
  s = node_stream_wrap_of(ARG(0));
  if (!s)
    return JS_UNDEFINED;
  parser_unconsume(p);
  p->prev_alloc = s->alloc_override;
  p->prev_read = s->read_override;
  p->prev_listener = s->listener;
  s->alloc_override = parser_stream_alloc;
  s->read_override = parser_stream_read;
  s->listener = p;
  p->stream = s;
  p->stream_obj = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue parser_unconsume_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Parser *p = parser_of(ctx, this_val);
  if (p)
    parser_unconsume(p);
  return JS_UNDEFINED;
}

static JSValue parser_get_current_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  Parser *p = parser_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  return nb_new_buffer(ctx, p->cur_data ? p->cur_data : "", p->cur_len);
}

static JSValue parser_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  return JS_NewFloat64(ctx, p ? p->aw.async_id : -1);
}

static JSValue parser_get_provider_type(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  return JS_NewInt32(ctx, p ? p->aw.provider : 0);
}

static JSValue parser_async_reset(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  Parser *p = JS_GetOpaque(this_val, parser_class_id);
  if (p)
    async_wrap_reset(&p->aw, ARG(0));
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry parser_proto[] = {
  JS_CFUNC_DEF("close", 0, parser_close),
  JS_CFUNC_DEF("free", 0, parser_free),
  JS_CFUNC_DEF("remove", 0, parser_remove),
  JS_CFUNC_DEF("execute", 1, parser_execute_fn),
  JS_CFUNC_DEF("finish", 0, parser_finish),
  JS_CFUNC_DEF("initialize", 5, parser_initialize),
  JS_CFUNC_MAGIC_DEF("pause", 0, parser_pause, 1),
  JS_CFUNC_MAGIC_DEF("resume", 0, parser_pause, 0),
  JS_CFUNC_DEF("consume", 1, parser_consume),
  JS_CFUNC_DEF("unconsume", 0, parser_unconsume_fn),
  JS_CFUNC_DEF("getCurrentBuffer", 0, parser_get_current_buffer),
  JS_CFUNC_DEF("getAsyncId", 0, parser_get_async_id),
  JS_CFUNC_DEF("getProviderType", 0, parser_get_provider_type),
  JS_CFUNC_DEF("asyncReset", 1, parser_async_reset),
};

/* ---- ConnectionsList ---- */

static void conns_finalizer(JSRuntime *rt, JSValueConst val) {
  ConnList *l = JS_GetOpaque(val, conns_class_id);
  size_t i;
  if (!l)
    return;
  /* parsers still pointing here hold a reference, so none are left */
  for (i = 0; i < l->nall; i++)
    l->all[i]->conns = NULL;
  free(l->all);
  free(l->active);
  free(l);
}

static JSValue conns_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, conns_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(ConnList)));
  return obj;
}

static int cmp_parsers(const void *a, const void *b) {
  const Parser *l = *(Parser *const *)a, *r = *(Parser *const *)b;
  if (l->last_message_start == 0 && r->last_message_start == 0)
    return l < r ? -1 : l > r;
  if (l->last_message_start == 0)
    return -1;
  if (r->last_message_start == 0)
    return 1;
  return l->last_message_start < r->last_message_start ? -1
         : l->last_message_start > r->last_message_start;
}

static JSValue parsers_array(JSContext *ctx, Parser **arr, size_t n, int filter) {
  JSValue out = JS_NewArray(ctx);
  Parser **copy = malloc((n ? n : 1) * sizeof(Parser *));
  size_t i;
  uint32_t k = 0;
  memcpy(copy, arr, n * sizeof(Parser *));
  qsort(copy, n, sizeof(Parser *), cmp_parsers);
  for (i = 0; i < n; i++)
    if (filter == 0 || copy[i]->last_message_start == 0)
      JS_SetPropertyUint32(ctx, out, k++, JS_DupValue(ctx, copy[i]->aw.object));
  free(copy);
  return out;
}

static JSValue conns_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ConnList *l = JS_GetOpaque2(ctx, this_val, conns_class_id);
  return l ? parsers_array(ctx, l->all, l->nall, 0) : JS_EXCEPTION;
}

static JSValue conns_idle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ConnList *l = JS_GetOpaque2(ctx, this_val, conns_class_id);
  return l ? parsers_array(ctx, l->all, l->nall, 1) : JS_EXCEPTION;
}

static JSValue conns_active(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ConnList *l = JS_GetOpaque2(ctx, this_val, conns_class_id);
  return l ? parsers_array(ctx, l->active, l->nactive, 0) : JS_EXCEPTION;
}

static JSValue conns_expired(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  ConnList *l = JS_GetOpaque2(ctx, this_val, conns_class_id);
  uint64_t headers_timeout, request_timeout, now, hd, rd, t;
  JSValue out;
  uint32_t k = 0;
  size_t i;
  if (!l)
    return JS_EXCEPTION;
  headers_timeout = (uint64_t)nb_uint32(ctx, ARG(0), 0) * 1000000;
  request_timeout = (uint64_t)nb_uint32(ctx, ARG(1), 0) * 1000000;
  out = JS_NewArray(ctx);
  if (headers_timeout == 0 && request_timeout == 0)
    return out;
  if (request_timeout > 0 && headers_timeout > request_timeout) {
    t = headers_timeout;
    headers_timeout = request_timeout;
    request_timeout = t;
  }
  now = uv_hrtime();
  hd = headers_timeout > 0 && now > headers_timeout ? now - headers_timeout : 0;
  rd = request_timeout > 0 && now > request_timeout ? now - request_timeout : 0;
  if (hd == 0 && rd == 0)
    return out;
  qsort(l->active, l->nactive, sizeof(Parser *), cmp_parsers);
  for (i = 0; i < l->nactive;) {
    Parser *p = l->active[i];
    if ((!p->headers_completed && hd > 0 && p->last_message_start < hd) ||
        (rd > 0 && p->last_message_start < rd)) {
      JS_SetPropertyUint32(ctx, out, k++, JS_DupValue(ctx, p->aw.object));
      conns_pop_active(l, p);
    } else {
      i++;
    }
  }
  return out;
}

static const JSCFunctionListEntry conns_proto[] = {
  JS_CFUNC_DEF("all", 0, conns_all),
  JS_CFUNC_DEF("idle", 0, conns_idle),
  JS_CFUNC_DEF("active", 0, conns_active),
  JS_CFUNC_DEF("expired", 2, conns_expired),
};

JSValue binding_init_http_parser(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ctor, methods, all;
  uint32_t n;
  NodeClassDef pdef = { .name = "HTTPParser", .class_id = &parser_class_id,
                        .ctor = parser_ctor, .finalizer = parser_finalizer,
                        .proto_funcs = parser_proto, .proto_funcs_count = countof(parser_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef cdef = { .name = "ConnectionsList", .class_id = &conns_class_id,
                        .ctor = conns_ctor, .finalizer = conns_finalizer,
                        .proto_funcs = conns_proto, .proto_funcs_count = countof(conns_proto),
                        .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &pdef);
#define C(name) nb_set_int(ctx, ctor, #name, name)
  nb_set_int(ctx, ctor, "REQUEST", HTTP_REQUEST);
  nb_set_int(ctx, ctor, "RESPONSE", HTTP_RESPONSE);
  C(kOnMessageBegin); C(kOnHeaders); C(kOnHeadersComplete); C(kOnBody);
  C(kOnMessageComplete); C(kOnExecute); C(kOnTimeout);
  C(kLenientNone); C(kLenientHeaders); C(kLenientChunkedLength); C(kLenientKeepAlive);
  C(kLenientTransferEncoding); C(kLenientVersion); C(kLenientDataAfterClose);
  C(kLenientOptionalLFAfterCR); C(kLenientOptionalCRLFAfterChunk);
  C(kLenientOptionalCRBeforeLF); C(kLenientSpacesAfterChunkSize);
  C(kLenientHeaderValueRelaxed); C(kLenientAll);
#undef C
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, nb_define_class(ctx, t, &cdef));

  methods = JS_NewArray(ctx);
  n = 0;
#define V(num, name, string) JS_SetPropertyUint32(ctx, methods, n++, JS_NewString(ctx, #string));
  HTTP_METHOD_MAP(V)
#undef V
  all = JS_NewArray(ctx);
  n = 0;
#define V(num, name, string) JS_SetPropertyUint32(ctx, all, n++, JS_NewString(ctx, #string));
  HTTP_ALL_METHOD_MAP(V)
#undef V
  nb_set(ctx, t, "methods", methods);
  nb_set(ctx, t, "allMethods", all);
  return t;
}
