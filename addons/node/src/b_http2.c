/* internalBinding('http2') (node_http2.cc) over nghttp2: Http2Session (a
 * listener of the socket's native stream, as in Node), Http2Stream (a
 * StreamBase for JS), pings, settings, and the shared state arrays.  The
 * structure and the rules follow node_http2.cc closely; the differences:
 * received DATA reaches JS as copies (not slices of the socket buffer),
 * session memory accounting leaves out nghttp2's own allocations, and
 * performance entries are not emitted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2/nghttp2.h>

#include "node.h"
#include "streams.h"

extern const char node_http2_constants_json[];

/* ---------------------------------------------------------------------- */
/* constants (node_http2.h, node_http2_state.h) */

enum { IDX_SETTINGS_HEADER_TABLE_SIZE, IDX_SETTINGS_ENABLE_PUSH, IDX_SETTINGS_INITIAL_WINDOW_SIZE,
       IDX_SETTINGS_MAX_FRAME_SIZE, IDX_SETTINGS_MAX_CONCURRENT_STREAMS,
       IDX_SETTINGS_MAX_HEADER_LIST_SIZE, IDX_SETTINGS_ENABLE_CONNECT_PROTOCOL,
       IDX_SETTINGS_COUNT };
#define MAX_ADDITIONAL_SETTINGS 10
#define SETTINGS_BUFFER_LEN (IDX_SETTINGS_COUNT + 1 + 1 + 2 * MAX_ADDITIONAL_SETTINGS)

enum { IDX_SESSION_STATE_EFFECTIVE_LOCAL_WINDOW_SIZE, IDX_SESSION_STATE_EFFECTIVE_RECV_DATA_LENGTH,
       IDX_SESSION_STATE_NEXT_STREAM_ID, IDX_SESSION_STATE_LOCAL_WINDOW_SIZE,
       IDX_SESSION_STATE_LAST_PROC_STREAM_ID, IDX_SESSION_STATE_REMOTE_WINDOW_SIZE,
       IDX_SESSION_STATE_OUTBOUND_QUEUE_SIZE, IDX_SESSION_STATE_HD_DEFLATE_DYNAMIC_TABLE_SIZE,
       IDX_SESSION_STATE_HD_INFLATE_DYNAMIC_TABLE_SIZE, IDX_SESSION_STATE_COUNT };

enum { IDX_STREAM_STATE, IDX_STREAM_STATE_WEIGHT, IDX_STREAM_STATE_SUM_DEPENDENCY_WEIGHT,
       IDX_STREAM_STATE_LOCAL_CLOSE, IDX_STREAM_STATE_REMOTE_CLOSE,
       IDX_STREAM_STATE_LOCAL_WINDOW_SIZE, IDX_STREAM_STATE_COUNT };

enum { IDX_OPTIONS_MAX_DEFLATE_DYNAMIC_TABLE_SIZE, IDX_OPTIONS_MAX_RESERVED_REMOTE_STREAMS,
       IDX_OPTIONS_MAX_SEND_HEADER_BLOCK_LENGTH, IDX_OPTIONS_PEER_MAX_CONCURRENT_STREAMS,
       IDX_OPTIONS_PADDING_STRATEGY, IDX_OPTIONS_MAX_HEADER_LIST_PAIRS,
       IDX_OPTIONS_MAX_OUTSTANDING_PINGS, IDX_OPTIONS_MAX_OUTSTANDING_SETTINGS,
       IDX_OPTIONS_MAX_SESSION_MEMORY, IDX_OPTIONS_MAX_SETTINGS, IDX_OPTIONS_STREAM_RESET_RATE,
       IDX_OPTIONS_STREAM_RESET_BURST, IDX_OPTIONS_STRICT_HTTP_FIELD_WHITESPACE_VALIDATION,
       IDX_OPTIONS_FLAGS };

#define IDX_STREAM_STATS_COUNT 6
#define IDX_SESSION_STATS_COUNT 9

#define DEFAULT_SETTINGS_HEADER_TABLE_SIZE 4096
#define DEFAULT_SETTINGS_ENABLE_PUSH 1
#define DEFAULT_SETTINGS_MAX_CONCURRENT_STREAMS 0xffffffffu
#define DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE 65535
#define DEFAULT_SETTINGS_MAX_FRAME_SIZE 16384
#define DEFAULT_SETTINGS_MAX_HEADER_LIST_SIZE 65535
#define DEFAULT_SETTINGS_ENABLE_CONNECT_PROTOCOL 0
#define MAX_MAX_HEADER_LIST_SIZE 16777215u
#define DEFAULT_MAX_HEADER_LIST_PAIRS 128u

#define kDefaultMaxPings 10
#define kDefaultMaxSettings 10
#define kDefaultMaxSessionMemory 10000000ull

#define STREAM_OPTION_EMPTY_PAYLOAD 0x1
#define STREAM_OPTION_GET_TRAILERS 0x2

enum { PADDING_STRATEGY_NONE, PADDING_STRATEGY_ALIGNED, PADDING_STRATEGY_MAX };
enum { SESSION_SERVER, SESSION_CLIENT };

/* Http2Stream states */
#define kStreamStateShut 0x1
#define kStreamStateReadStart 0x2
#define kStreamStateReadPaused 0x4
#define kStreamStateClosed 0x8
#define kStreamStateDestroyed 0x10
#define kStreamStateTrailers 0x20

/* Http2Session states */
#define kSessionStateHasScope 0x1
#define kSessionStateWriteScheduled 0x2
#define kSessionStateClosed 0x4
#define kSessionStateClosing 0x8
#define kSessionStateSending 0x10
#define kSessionStateWriteInProgress 0x20
#define kSessionStateReadingStopped 0x40
#define kSessionStateReceivePaused 0x80
#define kSessionStateReceiving 0x100
#define kSessionStateClosePending 0x200

/* SessionJSFields: the `fields` Uint8Array JS writes listener counts into */
enum { kBitfield = 0, kSessionPriorityListenerCount = 1, kSessionFrameErrorListenerCount = 2,
       kSessionMaxInvalidFrames = 4, kSessionMaxRejectedStreams = 8, kSessionUint8FieldCount = 12 };
enum { kSessionHasRemoteSettingsListeners, kSessionRemoteSettingsIsUpToDate,
       kSessionHasPingListeners, kSessionHasAltsvcListeners };

/* the JS functions setCallbackFunctions() gives */
enum { CB_ERROR, CB_PRIORITY, CB_SETTINGS, CB_PING, CB_HEADERS, CB_FRAME_ERROR, CB_GOAWAY_DATA,
       CB_ALTSVC, CB_ORIGIN, CB_STREAM_TRAILERS, CB_STREAM_CLOSE, CB_COUNT };

/* ---------------------------------------------------------------------- */
/* per-environment state (Http2State) and SetImmediate */

typedef struct H2Task {
  struct H2Task *next;
  void (*fn)(void *arg);
  void *arg;
  JSValue keep;
} H2Task;

typedef struct H2State {
  /* the arrays shared with JS; doubles first */
  double session_state[IDX_SESSION_STATE_COUNT];
  double stream_state[IDX_STREAM_STATE_COUNT];
  double stream_stats[IDX_STREAM_STATS_COUNT];
  double session_stats[IDX_SESSION_STATS_COUNT];
  uint32_t options[IDX_OPTIONS_FLAGS + 1];
  uint32_t settings[SETTINGS_BUFFER_LEN];
  Env *env;
  /* borrowed: the binding data object owns them */
  JSValue cb[CB_COUNT];
  bool cb_set;
  uv_idle_t idle;
  bool idle_inited;
  H2Task *tasks, *tasks_tail;
} H2State;

static H2State *state_of(Env *env) {
  H2State *st = env_scratch(env, "http2", sizeof(H2State));
  st->env = env;
  return st;
}

static void idle_cb(uv_idle_t *h) {
  H2State *st = h->data;
  Env *env = st->env;
  JSContext *ctx = env->ctx;
  H2Task *t = st->tasks, *next;
  st->tasks = st->tasks_tail = NULL;
  uv_idle_stop(h);
  env_internal_callback_scope_enter(env);
  for (; t; t = next) {
    next = t->next;
    if (env->can_call_into_js)
      t->fn(t->arg);
    JS_FreeValue(ctx, t->keep);
    free(t);
  }
  env_internal_callback_scope_exit(env, false);
}

/* runs fn(arg) on a later turn of the loop; keep (taken) stays alive till then */
static void set_immediate(H2State *st, void (*fn)(void *), void *arg, JSValue keep) {
  H2Task *t = calloc(1, sizeof(*t));
  t->fn = fn;
  t->arg = arg;
  t->keep = keep;
  if (st->tasks_tail)
    st->tasks_tail->next = t;
  else
    st->tasks = t;
  st->tasks_tail = t;
  if (!st->idle_inited) {
    uv_idle_init(st->env->loop, &st->idle);
    st->idle.data = st;
    st->idle_inited = true;
  }
  if (!uv_is_active((uv_handle_t *)&st->idle))
    uv_idle_start(&st->idle, idle_cb);
}

/* ---------------------------------------------------------------------- */
/* the objects */

typedef struct H2Session H2Session;
typedef struct H2Stream H2Stream;

typedef struct OutBuf {
  char *base;        /* NULL: in the session's storage, in order */
  size_t len;
  JSValue req;       /* a stream's WriteWrap to complete once written */
  H2Stream *owner;   /* the stream of req */
  void *free_ptr;    /* an allocation this buffer ends */
} OutBuf;

typedef struct H2Ping {
  struct H2Ping *next;
  JSValue cb;
  uint64_t start;
} H2Ping;

typedef struct H2SettingsReq {
  struct H2SettingsReq *next;
  JSValue cb;
  uint64_t start;
  size_t count;
  nghttp2_settings_entry entries[IDX_SETTINGS_COUNT + MAX_ADDITIONAL_SETTINGS];
} H2SettingsReq;

typedef struct {
  size_t number;
  nghttp2_settings_entry entries[MAX_ADDITIONAL_SETTINGS];
} CustomSettings;

struct H2Session {
  AsyncWrap aw;
  Env *env;
  H2State *st;
  nghttp2_session *session;
  int type;
  uint8_t *fields;
  JSValue fields_arr;
  uint32_t max_header_pairs;
  uint64_t max_session_memory, current_session_memory;
  /* the streams by id (each holds a reference to its object) */
  H2Stream **buckets;
  size_t nbuckets, nstreams;
  /* every stream object that still points here */
  H2Stream *all;
  int flags;
  int padding_strategy;
  uint32_t chunks_sent_since_last_write;
  /* input not yet given to nghttp2 */
  uint8_t *in_buf;
  size_t in_len, in_off;
  const char *custom_recv_error_code;
  size_t max_outstanding_pings, n_pings;
  H2Ping *pings, *pings_tail;
  size_t max_outstanding_settings, n_settings;
  H2SettingsReq *settings_head, *settings_tail;
  CustomSettings local_custom, remote_custom;
  /* output being written */
  OutBuf *out;
  size_t nout, cap_out;
  uint8_t *storage;
  size_t storage_len, storage_cap;
  size_t outgoing_length;
  int32_t *pending_rst;
  size_t n_pending_rst, cap_pending_rst;
  uint32_t pending_close_code;
  bool pending_close_socket_closed;
  uint32_t rejected_stream_count, invalid_frame_count;
  /* the socket's stream we listen to */
  StreamWrap *stream;
  JSValue stream_obj;
  void (*prev_alloc)(StreamWrap *, size_t, uv_buf_t *);
  void (*prev_read)(StreamWrap *, ssize_t, const uv_buf_t *);
  void *prev_listener;
  bool graceful_close_initiated, goaway_initiated, internal_goaway_sent;
  int32_t stream_count;
};

typedef struct QChunk {
  struct QChunk *next;
  JSValue req;
  uint8_t *alloc;
  char *base;
  size_t len;
} QChunk;

typedef struct {
  nghttp2_rcbuf *name, *value;
  uint8_t flags;
} HeaderPair;

struct H2Stream {
  StreamWrap s;            /* the JS-facing StreamBase; s.hw.aw its AsyncWrap */
  H2Session *session;      /* NULL once detached */
  H2Stream *hash_next;
  H2Stream *all_next, *all_prev;
  bool in_map;
  int32_t id, code;
  int flags;
  uint32_t max_header_pairs, max_header_length;
  int current_headers_category;
  uint32_t current_headers_length;
  uint64_t retained_headers_length;
  HeaderPair *headers;
  size_t nheaders, cap_headers;
  size_t inbound_consumed_data_while_paused;
  QChunk *q_head, *q_tail;
  size_t available_outbound_length;
  uint64_t first_byte_sent, first_header;
};

static JSClassID session_class_id, stream_class_id;

#define SF(se, f) (((se)->flags & (f)) != 0)
#define SET_SF(se, f, on) ((on) ? ((se)->flags |= (f)) : ((se)->flags &= ~(f)))
#define STF(st, f) (((st)->flags & (f)) != 0)

static bool session_destroyed(H2Session *se) {
  return SF(se, kSessionStateClosed) || !se->session;
}

static bool stream_destroyed(H2Stream *st) {
  return STF(st, kStreamStateDestroyed);
}

static bool stream_writable(H2Stream *st) {
  return !STF(st, kStreamStateShut);
}

static bool stream_reading(H2Stream *st) {
  return STF(st, kStreamStateReadStart) && !STF(st, kStreamStateReadPaused);
}

static uint32_t field_u32(H2Session *se, int off) {
  uint32_t v;
  memcpy(&v, se->fields + off, 4);
  return v;
}

static JSContext *ctx_of(H2Session *se) {
  return se->env->ctx;
}

/* a JS callback with the session (or a stream) as `this`; false when it failed */
static JSValue call_cb(AsyncWrap *aw, H2State *st, int which, int argc, JSValueConst *argv) {
  if (!JS_IsFunction(aw->env->ctx, st->cb[which]))
    return JS_UNDEFINED;
  return async_wrap_make_callback(aw, st->cb[which], argc, argv);
}

static void free_result(JSContext *ctx, JSValue r) {
  if (!JS_IsException(r) && !JS_IsUninitialized(r))
    JS_FreeValue(ctx, r);
}

static void call_cb_void(AsyncWrap *aw, H2State *st, int which, int argc, JSValueConst *argv) {
  free_result(aw->env->ctx, call_cb(aw, st, which, argc, argv));
}

static void call_named_void(AsyncWrap *aw, const char *name) {
  free_result(aw->env->ctx, async_wrap_make_callback_name(aw, name, 0, NULL));
}

/* ---------------------------------------------------------------------- */
/* the stream map */

static size_t bucket_of(H2Session *se, int32_t id) {
  return ((uint32_t)id * 2654435761u) & (se->nbuckets - 1);
}

static H2Stream *find_stream(H2Session *se, int32_t id) {
  H2Stream *st;
  if (!se->nbuckets)
    return NULL;
  for (st = se->buckets[bucket_of(se, id)]; st; st = st->hash_next)
    if (st->id == id)
      return st;
  return NULL;
}

static void map_grow(H2Session *se) {
  size_t n = se->nbuckets ? se->nbuckets * 2 : 64, i;
  H2Stream **b = calloc(n, sizeof(*b)), *st, *next;
  size_t old = se->nbuckets;
  H2Stream **ob = se->buckets;
  se->buckets = b;
  se->nbuckets = n;
  for (i = 0; i < old; i++) {
    for (st = ob[i]; st; st = next) {
      next = st->hash_next;
      st->hash_next = b[bucket_of(se, st->id)];
      b[bucket_of(se, st->id)] = st;
    }
  }
  free(ob);
}

static void add_stream(H2Session *se, H2Stream *st) {
  size_t b;
  if (se->nstreams + 1 > se->nbuckets / 2 || !se->nbuckets)
    map_grow(se);
  b = bucket_of(se, st->id);
  st->hash_next = se->buckets[b];
  se->buckets[b] = st;
  st->in_map = true;
  se->nstreams++;
  se->stream_count++;
  se->current_session_memory += sizeof(*st);
}

/* takes st out of the map: the caller gets the map's reference to its object */
static bool remove_stream(H2Session *se, H2Stream *st) {
  H2Stream **p;
  if (!st->in_map || !se->nbuckets)
    return false;
  for (p = &se->buckets[bucket_of(se, st->id)]; *p; p = &(*p)->hash_next) {
    if (*p == st) {
      *p = st->hash_next;
      st->hash_next = NULL;
      st->in_map = false;
      se->nstreams--;
      if (st->current_headers_length > 0) {
        se->current_session_memory -= st->current_headers_length;
        st->current_headers_length = 0;
      }
      if (st->retained_headers_length > 0) {
        se->current_session_memory -= st->retained_headers_length;
        st->retained_headers_length = 0;
      }
      se->current_session_memory -= sizeof(*st);
      return true;
    }
  }
  return false;
}

static bool can_add_stream(H2Session *se) {
  uint32_t max = nghttp2_session_get_local_settings(se->session,
                                                    NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  return se->nstreams < max &&
         se->current_session_memory + sizeof(H2Stream) <= se->max_session_memory;
}

static bool has_available_session_memory(H2Session *se, uint64_t amount) {
  return se->current_session_memory + sizeof(H2Session) + se->storage_len + amount <=
         se->max_session_memory;
}

/* ---------------------------------------------------------------------- */
/* scope, writes */

static void maybe_schedule_write(H2Session *se);
static int send_pending_data(H2Session *se);

static bool scope_enter(H2Session *se) {
  if (!se)
    return false;
  if (SF(se, kSessionStateHasScope) || SF(se, kSessionStateWriteScheduled))
    return false;
  se->flags |= kSessionStateHasScope;
  return true;
}

static void scope_exit(H2Session *se, bool owned) {
  if (!owned)
    return;
  se->flags &= ~kSessionStateHasScope;
  if (!SF(se, kSessionStateWriteScheduled))
    maybe_schedule_write(se);
}

static void write_task(void *arg) {
  H2Session *se = arg;
  if (!se->session || !SF(se, kSessionStateWriteScheduled))
    return;
  send_pending_data(se);
}

static void maybe_schedule_write(H2Session *se) {
  if (!se->session || SF(se, kSessionStateWriteScheduled))
    return;
  if (nghttp2_session_want_write(se->session)) {
    se->flags |= kSessionStateWriteScheduled;
    set_immediate(se->st, write_task, se, JS_DupValue(ctx_of(se), se->aw.object));
  }
}

static void maybe_stop_reading(H2Session *se) {
  if (SF(se, kSessionStateReadingStopped) || SF(se, kSessionStateClosing) || !se->stream)
    return;
  if (nghttp2_session_want_read(se->session) == 0 || SF(se, kSessionStateWriteInProgress)) {
    se->flags |= kSessionStateReadingStopped;
    node_stream_read_stop(se->stream);
  }
}

static void push_out(H2Session *se, OutBuf b) {
  if (se->nout == se->cap_out) {
    se->cap_out = se->cap_out ? se->cap_out * 2 : 32;
    se->out = realloc(se->out, se->cap_out * sizeof(OutBuf));
  }
  se->outgoing_length += b.len;
  se->out[se->nout++] = b;
}

static void copy_into_outgoing(H2Session *se, const uint8_t *src, size_t len) {
  OutBuf b = { NULL, len, JS_UNDEFINED, NULL, NULL };
  if (se->storage_len + len > se->storage_cap) {
    size_t cap = se->storage_cap ? se->storage_cap : 1024;
    while (cap < se->storage_len + len)
      cap *= 2;
    se->storage = realloc(se->storage, cap);
    se->storage_cap = cap;
  }
  memcpy(se->storage + se->storage_len, src, len);
  se->storage_len += len;
  push_out(se, b);
}

static void stream_write_done(H2Stream *st, JSValue req, int status) {
  Env *env = st->s.hw.aw.env;
  JSContext *ctx = env->ctx;
  JSValue args[3];
  args[0] = JS_NewInt32(ctx, status);
  args[1] = JS_DupValue(ctx, st->s.hw.aw.object);
  args[2] = JS_UNDEFINED;
  node_req_complete(env, req, 3, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
}

static void flush_rst_stream(H2Stream *st);

static void remove_pending_rst(H2Session *se, int32_t id) {
  size_t i, j = 0;
  for (i = 0; i < se->n_pending_rst; i++)
    if (se->pending_rst[i] != id)
      se->pending_rst[j++] = se->pending_rst[i];
  se->n_pending_rst = j;
}

static bool has_pending_rst(H2Session *se, int32_t id) {
  size_t i;
  for (i = 0; i < se->n_pending_rst; i++)
    if (se->pending_rst[i] == id)
      return true;
  return false;
}

static void add_pending_rst(H2Session *se, int32_t id) {
  if (se->n_pending_rst == se->cap_pending_rst) {
    se->cap_pending_rst = se->cap_pending_rst ? se->cap_pending_rst * 2 : 8;
    se->pending_rst = realloc(se->pending_rst, se->cap_pending_rst * sizeof(int32_t));
  }
  se->pending_rst[se->n_pending_rst++] = id;
}

/* the writes of the last batch are done: complete the streams' requests */
static void clear_outgoing(H2Session *se, int status) {
  JSContext *ctx = ctx_of(se);
  se->flags &= ~kSessionStateSending;
  if (se->nout) {
    OutBuf *out = se->out;
    size_t n = se->nout, i;
    se->out = NULL;
    se->nout = se->cap_out = 0;
    se->storage_len = 0;
    se->outgoing_length = 0;
    for (i = 0; i < n; i++) {
      if (!JS_IsUndefined(out[i].req)) {
        if (out[i].owner)
          stream_write_done(out[i].owner, out[i].req, 0);
        JS_FreeValue(ctx, out[i].req);
      }
      if (out[i].owner)
        JS_FreeValue(ctx, out[i].owner->s.hw.aw.object);
      free(out[i].free_ptr);
    }
    free(out);
  }
  if (se->n_pending_rst) {
    int32_t *ids = se->pending_rst;
    size_t n = se->n_pending_rst, i;
    se->pending_rst = NULL;
    se->n_pending_rst = se->cap_pending_rst = 0;
    send_pending_data(se);
    for (i = 0; i < n; i++) {
      H2Stream *st = find_stream(se, ids[i]);
      if (st)
        flush_rst_stream(st);
    }
    free(ids);
  }
}

static void maybe_notify_graceful_close_complete(H2Session *se) {
  if (!se->graceful_close_initiated || !se->session)
    return;
  if (nghttp2_session_want_write(se->session) == 0 &&
      nghttp2_session_want_read(se->session) == 0)
    call_named_void(&se->aw, "ongracefulclosecomplete");
}

static void consume_http2_data(H2Session *se);

/* OnStreamAfterWrite */
static void on_after_write(H2Session *se, int status) {
  maybe_notify_graceful_close_complete(se);
  se->flags &= ~kSessionStateWriteInProgress;
  clear_outgoing(se, status);
  if (SF(se, kSessionStateReadingStopped) && !SF(se, kSessionStateWriteInProgress) &&
      se->session && nghttp2_session_want_read(se->session) && se->stream) {
    se->flags &= ~kSessionStateReadingStopped;
    node_stream_read_start(se->stream);
  }
  if (session_destroyed(se)) {
    call_named_void(&se->aw, "ondone");
    if (se->stream) {
      se->flags &= ~kSessionStateReadingStopped;
      node_stream_read_start(se->stream);
    }
    return;
  }
  if (se->in_off > 0)
    consume_http2_data(se);
  if (!SF(se, kSessionStateWriteScheduled) && !session_destroyed(se))
    maybe_schedule_write(se);
}

static JSValue after_write_fn(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic, JSValue *data) {
  H2Session *se = JS_GetOpaque(data[0], session_class_id);
  if (se && SF(se, kSessionStateWriteInProgress))
    on_after_write(se, nb_int32(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0));
  return JS_UNDEFINED;
}

/* writes bufs to the socket's stream; *async when the completion comes later */
static int write_under(H2Session *se, uv_buf_t *bufs, unsigned n, bool *async) {
  Env *env = se->env;
  JSContext *ctx = env->ctx;
  JSValue req = JS_NewObject(ctx), fn;
  int err;
  fn = JS_NewCFunctionData(ctx, after_write_fn, 3, 0, 1, (JSValueConst *)&se->aw.object);
  JS_SetPropertyStr(ctx, req, "oncomplete", fn);
  env->stream_base_state[kLastWriteWasAsync] = 0;
  err = node_stream_write(se->stream, req, bufs, n, JS_UNDEFINED, true);
  *async = err == 0 && env->stream_base_state[kLastWriteWasAsync] != 0;
  JS_FreeValue(ctx, req);
  return err;
}

/* SendPendingData: nonzero when a write is already going on */
static int send_pending_data(H2Session *se) {
  ssize_t n;
  const uint8_t *src;
  uv_buf_t *bufs;
  size_t i, off = 0;
  bool async;
  int err;
  if (session_destroyed(se))
    return 0;
  se->flags &= ~kSessionStateWriteScheduled;
  if (SF(se, kSessionStateSending))
    return 1;
  if (SF(se, kSessionStateReceiving))
    return 1;
  se->flags |= kSessionStateSending;
  while ((n = nghttp2_session_mem_send(se->session, &src)) > 0)
    copy_into_outgoing(se, src, (size_t)n);
  if (!se->stream) {
    clear_outgoing(se, UV_ECANCELED);
    return 0;
  }
  if (se->nout == 0) {
    clear_outgoing(se, 0);
    return 0;
  }
  bufs = malloc(se->nout * sizeof(uv_buf_t));
  for (i = 0; i < se->nout; i++) {
    if (se->out[i].base == NULL) {
      bufs[i] = uv_buf_init((char *)se->storage + off, (unsigned)se->out[i].len);
      off += se->out[i].len;
    } else {
      bufs[i] = uv_buf_init(se->out[i].base, (unsigned)se->out[i].len);
    }
  }
  se->chunks_sent_since_last_write++;
  se->flags |= kSessionStateWriteInProgress;
  err = write_under(se, bufs, (unsigned)se->nout, &async);
  free(bufs);
  if (!async) {
    se->flags &= ~kSessionStateWriteInProgress;
    clear_outgoing(se, err);
    maybe_notify_graceful_close_complete(se);
  }
  maybe_stop_reading(se);
  if (se->internal_goaway_sent) {
    se->internal_goaway_sent = false;
    if (!SF(se, kSessionStateClosing) && !session_destroyed(se)) {
      JSValue arg = JS_NewInt32(ctx_of(se), NGHTTP2_ERR_PROTO);
      call_cb_void(&se->aw, se->st, CB_ERROR, 1, &arg);
    }
  }
  return 0;
}

/* ---------------------------------------------------------------------- */
/* streams */

static void stream_clear_headers(H2Stream *st) {
  size_t i;
  for (i = 0; i < st->nheaders; i++) {
    nghttp2_rcbuf_decref(st->headers[i].name);
    nghttp2_rcbuf_decref(st->headers[i].value);
  }
  st->nheaders = 0;
}

/* the stream stops pointing at its session */
static void stream_detach(H2Stream *st) {
  H2Session *se = st->session;
  if (!se)
    return;
  stream_clear_headers(st);
  if (st->all_prev)
    st->all_prev->all_next = st->all_next;
  else
    se->all = st->all_next;
  if (st->all_next)
    st->all_next->all_prev = st->all_prev;
  st->all_next = st->all_prev = NULL;
  st->session = NULL;
}

static void stream_free_queue(H2Stream *st, bool complete) {
  JSContext *ctx = st->s.hw.aw.env->ctx;
  while (st->q_head) {
    QChunk *c = st->q_head;
    st->q_head = c->next;
    if (complete && !JS_IsUndefined(c->req))
      stream_write_done(st, c->req, UV_ECANCELED);
    JS_FreeValue(ctx, c->req);
    free(c->alloc);
    free(c);
  }
  st->q_tail = NULL;
}

static int stream_do_write(StreamWrap *s, JSValueConst req, uv_buf_t *bufs, unsigned nbufs,
                           JSValueConst keep);
static int stream_do_shutdown(StreamWrap *s, JSValueConst req);
static int stream_read_start(StreamWrap *s);
static int stream_read_stop(StreamWrap *s);

static H2Stream *stream_new(H2Session *se, int32_t id, int category, int options) {
  JSContext *ctx = ctx_of(se);
  JSValue obj = JS_NewObjectClass(ctx, stream_class_id);
  H2Stream *st;
  if (JS_IsException(obj)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
  }
  st = calloc(1, sizeof(*st));
  async_wrap_init(&st->s.hw.aw, se->env, obj, PROVIDER_HTTP2STREAM, -1);
  st->s.hw.state = HW_INITIALIZED;
  st->s.hw.handle = NULL;
  st->s.stream = NULL;
  st->s.write_override = stream_do_write;
  st->s.shutdown_override = stream_do_shutdown;
  st->s.read_start_override = stream_read_start;
  st->s.read_stop_override = stream_read_stop;
  JS_SetOpaque(obj, st);
  st->session = se;
  st->all_next = se->all;
  if (se->all)
    se->all->all_prev = st;
  se->all = st;
  st->id = id;
  st->current_headers_category = category;
  st->max_header_pairs = se->max_header_pairs ? se->max_header_pairs
                                              : DEFAULT_MAX_HEADER_LIST_PAIRS;
  st->max_header_length = nghttp2_session_get_local_settings(se->session,
                                                             NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
  if (st->max_header_length > MAX_MAX_HEADER_LIST_SIZE)
    st->max_header_length = MAX_MAX_HEADER_LIST_SIZE;
  if (options & STREAM_OPTION_GET_TRAILERS)
    st->flags |= kStreamStateTrailers;
  if (options & STREAM_OPTION_EMPTY_PAYLOAD)
    st->flags |= kStreamStateShut;
  /* the map holds the reference JS_NewObjectClass gave */
  add_stream(se, st);
  return st;
}

static void stream_finalizer(JSRuntime *rt, JSValueConst val) {
  H2Stream *st = JS_GetOpaque(val, stream_class_id);
  if (!st)
    return;
  stream_detach(st);
  while (st->q_head) {
    QChunk *c = st->q_head;
    st->q_head = c->next;
    JS_FreeValueRT(rt, c->req);
    free(c->alloc);
    free(c);
  }
  free(st->headers);
  async_wrap_destroy(&st->s.hw.aw);
  free(st);
}

static void stream_start_headers(H2Stream *st, int category) {
  H2Session *se = st->session;
  se->current_session_memory -= st->current_headers_length;
  st->current_headers_length = 0;
  stream_clear_headers(st);
  st->current_headers_category = category;
}

static bool stream_add_header(H2Stream *st, nghttp2_rcbuf *name, nghttp2_rcbuf *value,
                              uint8_t flags) {
  H2Session *se = st->session;
  nghttp2_vec n = nghttp2_rcbuf_get_buf(name), v = nghttp2_rcbuf_get_buf(value);
  size_t length;
  if (n.len == 0)
    return true; /* empty names are ignored */
  length = n.len + v.len + 32;
  if (!has_available_session_memory(se, length) || st->nheaders == st->max_header_pairs ||
      st->current_headers_length + length > st->max_header_length)
    return false;
  if (st->first_header == 0)
    st->first_header = uv_hrtime();
  if (st->nheaders == st->cap_headers) {
    st->cap_headers = st->cap_headers ? st->cap_headers * 2 : 16;
    st->headers = realloc(st->headers, st->cap_headers * sizeof(HeaderPair));
  }
  nghttp2_rcbuf_incref(name);
  nghttp2_rcbuf_incref(value);
  st->headers[st->nheaders].name = name;
  st->headers[st->nheaders].value = value;
  st->headers[st->nheaders].flags = flags;
  st->nheaders++;
  st->current_headers_length += length;
  se->current_session_memory += length;
  return true;
}

static void flush_rst_stream(H2Stream *st) {
  H2Session *se = st->session;
  bool owned;
  if (!se)
    return;
  remove_pending_rst(se, st->id);
  owned = scope_enter(se);
  nghttp2_submit_rst_stream(se->session, NGHTTP2_FLAG_NONE, st->id, st->code);
  scope_exit(se, owned);
}

static void submit_rst_stream(H2Stream *st, uint32_t code) {
  H2Session *se = st->session;
  st->code = code;
  if (!se)
    return;
  if (SF(se, kSessionStateReceiving)) {
    if (code == NGHTTP2_ENHANCE_YOUR_CALM || code == NGHTTP2_REFUSED_STREAM)
      flush_rst_stream(st);
    else
      add_pending_rst(se, st->id);
    return;
  }
  if (SF(se, kSessionStateHasScope) && code == NGHTTP2_CANCEL) {
    add_pending_rst(se, st->id);
    return;
  }
  if (SF(se, kSessionStateHasScope) && code == NGHTTP2_REFUSED_STREAM) {
    flush_rst_stream(st);
    return;
  }
  if (send_pending_data(se) != 0) {
    add_pending_rst(se, st->id);
    return;
  }
  flush_rst_stream(st);
}

static bool has_writes_on_socket_for_stream(H2Session *se, H2Stream *st) {
  size_t i;
  for (i = 0; i < se->nout; i++)
    if (se->out[i].owner == st && !JS_IsUndefined(se->out[i].req))
      return true;
  return false;
}

/* the second half of Destroy(), on the next turn: the map's reference
   (taken over as the task's keep) is released after this */
static void destroy_task(void *arg) {
  H2Stream *st = arg;
  stream_free_queue(st, true);
  if (!st->session || !has_writes_on_socket_for_stream(st->session, st))
    stream_detach(st);
}

static void complete_destroy_cleanup(H2Stream *st) {
  H2Session *se = st->session;
  if (!se) {
    stream_detach(st);
    return;
  }
  if (has_pending_rst(se, st->id))
    flush_rst_stream(st);
  if (remove_stream(se, st))
    set_immediate(se->st, destroy_task, st, st->s.hw.aw.object);
}

static void complete_destroy_task(void *arg) {
  complete_destroy_cleanup(arg);
}

static void stream_destroy(H2Stream *st) {
  H2Session *se = st->session;
  if (stream_destroyed(st))
    return;
  st->flags |= kStreamStateDestroyed;
  if (!se) {
    stream_detach(st);
    return;
  }
  if (SF(se, kSessionStateReceiving)) {
    set_immediate(se->st, complete_destroy_task, st,
                  JS_DupValue(ctx_of(se), st->s.hw.aw.object));
    return;
  }
  if (has_pending_rst(se, st->id))
    flush_rst_stream(st);
  complete_destroy_cleanup(st);
}

static void stream_on_trailers(H2Stream *st) {
  st->flags &= ~kStreamStateTrailers;
  call_cb_void(&st->s.hw.aw, st->session->st, CB_STREAM_TRAILERS, 0, NULL);
}

/* the data provider: how much of the queue the next DATA frame takes */
static ssize_t on_read_data(nghttp2_session *handle, int32_t id, uint8_t *buf, size_t length,
                            uint32_t *flags, nghttp2_data_source *source, void *user_data) {
  H2Session *se = user_data;
  H2Stream *st = find_stream(se, id);
  size_t amount = 0;
  if (!st)
    return 0;
  if (st->first_byte_sent == 0)
    st->first_byte_sent = uv_hrtime();
  /* empty writes at the head are done now */
  while (st->q_head && st->q_head->len == 0) {
    QChunk *c = st->q_head;
    st->q_head = c->next;
    if (!st->q_head)
      st->q_tail = NULL;
    if (!JS_IsUndefined(c->req)) {
      stream_write_done(st, c->req, 0);
      JS_FreeValue(ctx_of(se), c->req);
    }
    free(c->alloc);
    free(c);
  }
  if (st->q_head) {
    amount = st->available_outbound_length < length ? st->available_outbound_length : length;
    if (amount > 0) {
      *flags |= NGHTTP2_DATA_FLAG_NO_COPY;
      st->available_outbound_length -= amount;
      se->current_session_memory -= amount;
    }
  }
  if (amount == 0 && stream_writable(st))
    return NGHTTP2_ERR_DEFERRED;
  if (st->available_outbound_length == 0 && !stream_writable(st)) {
    *flags |= NGHTTP2_DATA_FLAG_EOF;
    if (STF(st, kStreamStateTrailers) && !stream_destroyed(st)) {
      *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
      stream_on_trailers(st);
    }
  }
  return (ssize_t)amount;
}

static nghttp2_data_provider *provider(nghttp2_data_provider *p, H2Stream *st, int options) {
  if (options & STREAM_OPTION_EMPTY_PAYLOAD)
    return NULL;
  p->source.ptr = st;
  p->read_callback = on_read_data;
  return p;
}

/* nghttp2 sends a DATA frame's payload straight out of the stream's queue */
static int on_send_data(nghttp2_session *handle, nghttp2_frame *frame, const uint8_t *framehd,
                        size_t length, nghttp2_data_source *source, void *user_data) {
  static const char zero_bytes_256[256];
  H2Session *se = user_data;
  H2Stream *st = find_stream(se, frame->hd.stream_id);
  JSContext *ctx = ctx_of(se);
  if (!st)
    return 0;
  copy_into_outgoing(se, framehd, 9);
  if (frame->data.padlen > 0) {
    uint8_t padding_byte = (uint8_t)(frame->data.padlen - 1);
    copy_into_outgoing(se, &padding_byte, 1);
  }
  while (length > 0 && st->q_head) {
    QChunk *c = st->q_head;
    if (c->len <= length) {
      OutBuf b = { c->base, c->len, c->req, NULL, c->alloc };
      length -= c->len;
      if (!JS_IsUndefined(c->req)) {
        b.owner = st;
        JS_DupValue(ctx, st->s.hw.aw.object);
      }
      push_out(se, b);
      st->q_head = c->next;
      if (!st->q_head)
        st->q_tail = NULL;
      free(c);
      continue;
    }
    {
      OutBuf b = { c->base, length, JS_UNDEFINED, NULL, NULL };
      push_out(se, b);
      c->base += length;
      c->len -= length;
      length = 0;
    }
  }
  if (frame->data.padlen > 0) {
    OutBuf b = { (char *)zero_bytes_256, frame->data.padlen - 1, JS_UNDEFINED, NULL, NULL };
    push_out(se, b);
  }
  return 0;
}

/* ---- StreamBase overrides ---- */

static int stream_do_write(StreamWrap *s, JSValueConst req, uv_buf_t *bufs, unsigned nbufs,
                           JSValueConst keep) {
  H2Stream *st = (H2Stream *)s;
  H2Session *se = st->session;
  Env *env = s->hw.aw.env;
  JSContext *ctx = env->ctx;
  size_t total = 0, off = 0;
  unsigned i;
  QChunk *c;
  bool owned;
  for (i = 0; i < nbufs; i++)
    total += bufs[i].len;
  env->stream_base_state[kBytesWritten] = (int32_t)total;
  env->stream_base_state[kLastWriteWasAsync] = 0;
  if (!se || !stream_writable(st) || stream_destroyed(st))
    return UV_EOF;
  owned = scope_enter(se);
  c = calloc(1, sizeof(*c));
  c->req = JS_DupValue(ctx, req);
  c->alloc = total ? malloc(total) : NULL;
  for (i = 0; i < nbufs; i++) {
    memcpy(c->alloc + off, bufs[i].base, bufs[i].len);
    off += bufs[i].len;
  }
  c->base = (char *)c->alloc;
  c->len = total;
  if (st->q_tail)
    st->q_tail->next = c;
  else
    st->q_head = c;
  st->q_tail = c;
  st->available_outbound_length += total;
  se->current_session_memory += total;
  nghttp2_session_resume_data(se->session, st->id);
  env->stream_base_state[kLastWriteWasAsync] = 1;
  scope_exit(se, owned);
  return 0;
}

static int stream_do_shutdown(StreamWrap *s, JSValueConst req) {
  H2Stream *st = (H2Stream *)s;
  H2Session *se = st->session;
  bool owned;
  if (stream_destroyed(st) || !se)
    return UV_EPIPE;
  owned = scope_enter(se);
  st->flags |= kStreamStateShut;
  nghttp2_session_resume_data(se->session, st->id);
  scope_exit(se, owned);
  return 1; /* finished synchronously */
}

static int stream_read_start(StreamWrap *s) {
  H2Stream *st = (H2Stream *)s;
  H2Session *se = st->session;
  bool owned;
  if (stream_destroyed(st) || !se)
    return 0;
  owned = scope_enter(se);
  st->flags |= kStreamStateReadStart;
  st->flags &= ~kStreamStateReadPaused;
  nghttp2_session_consume_stream(se->session, st->id, st->inbound_consumed_data_while_paused);
  st->inbound_consumed_data_while_paused = 0;
  scope_exit(se, owned);
  return 0;
}

static int stream_read_stop(StreamWrap *s) {
  H2Stream *st = (H2Stream *)s;
  if (stream_destroyed(st) || !stream_reading(st))
    return 0;
  st->flags |= kStreamStateReadPaused;
  return 0;
}

/* ---------------------------------------------------------------------- */
/* nghttp2 callbacks */

static int32_t frame_id(const nghttp2_frame *frame) {
  return frame->hd.type == NGHTTP2_PUSH_PROMISE ? frame->push_promise.promised_stream_id
                                                : frame->hd.stream_id;
}

static int on_begin_headers(nghttp2_session *handle, const nghttp2_frame *frame,
                            void *user_data) {
  H2Session *se = user_data;
  int32_t id = frame_id(frame);
  H2Stream *st;
  if (SF(se, kSessionStateClosePending))
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  st = find_stream(se, id);
  if (!st) {
    if (!can_add_stream(se) || stream_new(se, id, frame->headers.cat, 0) == NULL) {
      if (se->rejected_stream_count++ > field_u32(se, kSessionMaxRejectedStreams))
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      nghttp2_submit_rst_stream(handle, NGHTTP2_FLAG_NONE, id, NGHTTP2_ENHANCE_YOUR_CALM);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    se->rejected_stream_count = 0;
  } else if (!stream_destroyed(st)) {
    stream_start_headers(st, frame->headers.cat);
  }
  return 0;
}

static int on_header(nghttp2_session *handle, const nghttp2_frame *frame, nghttp2_rcbuf *name,
                     nghttp2_rcbuf *value, uint8_t flags, void *user_data) {
  H2Session *se = user_data;
  H2Stream *st = find_stream(se, frame_id(frame));
  if (!st)
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  if (!stream_destroyed(st) && !stream_add_header(st, name, value, flags)) {
    submit_rst_stream(st, NGHTTP2_ENHANCE_YOUR_CALM);
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  return 0;
}

static JSValue latin1(JSContext *ctx, const uint8_t *p, size_t len) {
  return node_string_encode(ctx, p, len, ENC_LATIN1);
}

static void handle_headers_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  int32_t id = frame_id(frame);
  H2Stream *st = find_stream(se, id);
  JSValue headers, sensitive, args[6];
  size_t i, nsens = 0;
  if (!st || stream_destroyed(st))
    return;
  headers = JS_NewArray(ctx);
  sensitive = JS_NewArray(ctx);
  for (i = 0; i < st->nheaders; i++) {
    nghttp2_vec n = nghttp2_rcbuf_get_buf(st->headers[i].name);
    nghttp2_vec v = nghttp2_rcbuf_get_buf(st->headers[i].value);
    JSValue name = latin1(ctx, n.base, n.len);
    JS_SetPropertyUint32(ctx, headers, (uint32_t)(i * 2), JS_DupValue(ctx, name));
    JS_SetPropertyUint32(ctx, headers, (uint32_t)(i * 2 + 1), latin1(ctx, v.base, v.len));
    if (st->headers[i].flags & NGHTTP2_NV_FLAG_NO_INDEX)
      JS_SetPropertyUint32(ctx, sensitive, (uint32_t)nsens++, JS_DupValue(ctx, name));
    JS_FreeValue(ctx, name);
  }
  stream_clear_headers(st);
  st->retained_headers_length += st->current_headers_length;
  st->current_headers_length = 0;
  args[0] = JS_DupValue(ctx, st->s.hw.aw.object);
  args[1] = JS_NewInt32(ctx, id);
  args[2] = JS_NewInt32(ctx, st->current_headers_category);
  args[3] = JS_NewInt32(ctx, frame->hd.flags);
  args[4] = headers;
  args[5] = sensitive;
  call_cb_void(&se->aw, se->st, CB_HEADERS, 6, args);
  for (i = 0; i < 6; i++)
    JS_FreeValue(ctx, args[i]);
}

static void handle_priority_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  nghttp2_priority_spec spec = frame->priority.pri_spec;
  JSValue argv[4];
  if (se->fields[kSessionPriorityListenerCount] == 0)
    return;
  argv[0] = JS_NewInt32(ctx, frame_id(frame));
  argv[1] = JS_NewInt32(ctx, spec.stream_id);
  argv[2] = JS_NewInt32(ctx, spec.weight);
  argv[3] = JS_NewBool(ctx, spec.exclusive);
  call_cb_void(&se->aw, se->st, CB_PRIORITY, 4, argv);
}

static int handle_data_frame(H2Session *se, const nghttp2_frame *frame) {
  H2Stream *st = find_stream(se, frame_id(frame));
  if (st && !stream_destroyed(st) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    node_stream_emit_read(&st->s, UV_EOF, NULL);
  } else if (frame->hd.length == 0) {
    if (se->invalid_frame_count++ > field_u32(se, kSessionMaxInvalidFrames)) {
      se->custom_recv_error_code = "ERR_HTTP2_TOO_MANY_INVALID_FRAMES";
      return 1;
    }
  }
  return 0;
}

static void handle_goaway_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  nghttp2_goaway g = frame->goaway;
  JSValue argv[3];
  argv[0] = JS_NewUint32(ctx, g.error_code);
  argv[1] = JS_NewInt32(ctx, g.last_stream_id);
  argv[2] = g.opaque_data_len > 0 ? nb_new_buffer(ctx, g.opaque_data, g.opaque_data_len)
                                  : JS_UNDEFINED;
  call_cb_void(&se->aw, se->st, CB_GOAWAY_DATA, 3, argv);
  JS_FreeValue(ctx, argv[2]);
}

static void handle_altsvc_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  nghttp2_ext_altsvc *a = frame->ext.payload;
  JSValue argv[3];
  if (!(se->fields[kBitfield] & (1 << kSessionHasAltsvcListeners)))
    return;
  argv[0] = JS_NewInt32(ctx, frame_id(frame));
  argv[1] = latin1(ctx, a->origin, a->origin_len);
  argv[2] = latin1(ctx, a->field_value, a->field_value_len);
  call_cb_void(&se->aw, se->st, CB_ALTSVC, 3, argv);
  JS_FreeValue(ctx, argv[1]);
  JS_FreeValue(ctx, argv[2]);
}

static void handle_origin_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  nghttp2_ext_origin *o = frame->ext.payload;
  JSValue arr = JS_NewArray(ctx);
  size_t i;
  for (i = 0; i < o->nov; i++)
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, latin1(ctx, o->ov[i].origin, o->ov[i].origin_len));
  call_cb_void(&se->aw, se->st, CB_ORIGIN, 1, &arr);
  JS_FreeValue(ctx, arr);
}

static void ping_done(H2Session *se, JSValue cb, uint64_t start, bool ack, const uint8_t *payload);

static void handle_ping_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  JSValue arg;
  if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
    H2Ping *p = se->pings;
    if (!p) {
      arg = JS_NewInt32(ctx, NGHTTP2_ERR_PROTO);
      call_cb_void(&se->aw, se->st, CB_ERROR, 1, &arg);
      return;
    }
    se->pings = p->next;
    if (!se->pings)
      se->pings_tail = NULL;
    se->n_pings--;
    se->current_session_memory -= sizeof(*p);
    ping_done(se, p->cb, p->start, true, frame->ping.opaque_data);
    JS_FreeValue(ctx, p->cb);
    free(p);
    return;
  }
  if (!(se->fields[kBitfield] & (1 << kSessionHasPingListeners)))
    return;
  arg = nb_new_buffer(ctx, frame->ping.opaque_data, 8);
  call_cb_void(&se->aw, se->st, CB_PING, 1, &arg);
  JS_FreeValue(ctx, arg);
}

static void settings_done(H2Session *se, H2SettingsReq *r, bool ack) {
  JSContext *ctx = ctx_of(se);
  JSValue argv[2];
  argv[0] = JS_NewBool(ctx, ack);
  argv[1] = JS_NewFloat64(ctx, (double)(uv_hrtime() - r->start) / 1e6);
  free_result(ctx, async_wrap_make_callback(&se->aw, r->cb, 2, argv));
}

static void handle_settings_frame(H2Session *se, const nghttp2_frame *frame) {
  JSContext *ctx = ctx_of(se);
  if (!(frame->hd.flags & NGHTTP2_FLAG_ACK)) {
    se->fields[kBitfield] &= ~(1 << kSessionRemoteSettingsIsUpToDate);
    if (se->remote_custom.number > 0) {
      size_t i, j;
      for (i = 0; i < frame->settings.niv; i++) {
        int32_t id = frame->settings.iv[i].settings_id;
        if (id >= IDX_SETTINGS_COUNT) {
          for (j = 0; j < se->remote_custom.number; j++) {
            if ((se->remote_custom.entries[j].settings_id & 0xffff) == id) {
              se->remote_custom.entries[j].settings_id = id;
              se->remote_custom.entries[j].value = frame->settings.iv[i].value;
              break;
            }
          }
        }
      }
    }
    if (!(se->fields[kBitfield] & (1 << kSessionHasRemoteSettingsListeners)))
      return;
    call_cb_void(&se->aw, se->st, CB_SETTINGS, 0, NULL);
    return;
  }
  if (se->settings_head) {
    H2SettingsReq *r = se->settings_head;
    se->settings_head = r->next;
    if (!se->settings_head)
      se->settings_tail = NULL;
    se->n_settings--;
    se->current_session_memory -= sizeof(*r);
    settings_done(se, r, true);
    JS_FreeValue(ctx, r->cb);
    free(r);
    return;
  }
  {
    JSValue arg = JS_NewInt32(ctx, NGHTTP2_ERR_PROTO);
    call_cb_void(&se->aw, se->st, CB_ERROR, 1, &arg);
  }
}

static int on_frame_recv(nghttp2_session *handle, const nghttp2_frame *frame, void *user_data) {
  H2Session *se = user_data;
  if (SF(se, kSessionStateClosePending))
    return 0;
  switch (frame->hd.type) {
  case NGHTTP2_DATA:
    return handle_data_frame(se, frame);
  case NGHTTP2_PUSH_PROMISE:
  case NGHTTP2_HEADERS:
    handle_headers_frame(se, frame);
    break;
  case NGHTTP2_SETTINGS:
    handle_settings_frame(se, frame);
    break;
  case NGHTTP2_PRIORITY:
    handle_priority_frame(se, frame);
    break;
  case NGHTTP2_GOAWAY:
    handle_goaway_frame(se, frame);
    break;
  case NGHTTP2_PING:
    handle_ping_frame(se, frame);
    break;
  case NGHTTP2_ALTSVC:
    handle_altsvc_frame(se, frame);
    break;
  case NGHTTP2_ORIGIN:
    handle_origin_frame(se, frame);
    break;
  default:
    break;
  }
  return 0;
}

static int on_invalid_frame(nghttp2_session *handle, const nghttp2_frame *frame,
                            int lib_error_code, void *user_data) {
  H2Session *se = user_data;
  if (se->invalid_frame_count++ > field_u32(se, kSessionMaxInvalidFrames)) {
    se->custom_recv_error_code = "ERR_HTTP2_TOO_MANY_INVALID_FRAMES";
    return 1;
  }
  if (nghttp2_is_fatal(lib_error_code) || lib_error_code == NGHTTP2_ERR_STREAM_CLOSED ||
      lib_error_code == NGHTTP2_ERR_FLOW_CONTROL || lib_error_code == NGHTTP2_ERR_PROTO) {
    JSValue arg = JS_NewInt32(ctx_of(se), lib_error_code);
    call_cb_void(&se->aw, se->st, CB_ERROR, 1, &arg);
  }
  return 0;
}

static void decref_headers(H2Session *se, const nghttp2_frame *frame) {
  H2Stream *st = find_stream(se, frame_id(frame));
  if (st && !stream_destroyed(st) && st->nheaders > 0) {
    stream_clear_headers(st);
    se->current_session_memory -= st->current_headers_length;
    st->current_headers_length = 0;
  }
}

static uint32_t translate_error(int lib_error_code) {
  switch (lib_error_code) {
  case NGHTTP2_ERR_STREAM_CLOSED: return NGHTTP2_STREAM_CLOSED;
  case NGHTTP2_ERR_HEADER_COMP: return NGHTTP2_COMPRESSION_ERROR;
  case NGHTTP2_ERR_FRAME_SIZE_ERROR: return NGHTTP2_FRAME_SIZE_ERROR;
  case NGHTTP2_ERR_FLOW_CONTROL: return NGHTTP2_FLOW_CONTROL_ERROR;
  case NGHTTP2_ERR_REFUSED_STREAM: return NGHTTP2_REFUSED_STREAM;
  case NGHTTP2_ERR_PROTO:
  case NGHTTP2_ERR_HTTP_HEADER:
  case NGHTTP2_ERR_HTTP_MESSAGING: return NGHTTP2_PROTOCOL_ERROR;
  default: return NGHTTP2_INTERNAL_ERROR;
  }
}

static int on_frame_not_send(nghttp2_session *handle, const nghttp2_frame *frame,
                             int error_code, void *user_data) {
  H2Session *se = user_data;
  JSContext *ctx = ctx_of(se);
  JSValue argv[3];
  if (error_code == NGHTTP2_ERR_SESSION_CLOSING || error_code == NGHTTP2_ERR_STREAM_CLOSED ||
      error_code == NGHTTP2_ERR_STREAM_CLOSING) {
    decref_headers(se, frame);
    if (frame->hd.type != NGHTTP2_GOAWAY)
      return 0;
  }
  argv[0] = JS_NewInt32(ctx, frame->hd.stream_id);
  argv[1] = JS_NewInt32(ctx, frame->hd.type);
  argv[2] = JS_NewInt32(ctx, (int32_t)translate_error(error_code));
  call_cb_void(&se->aw, se->st, CB_FRAME_ERROR, 3, argv);
  return 0;
}

static int on_frame_send(nghttp2_session *handle, const nghttp2_frame *frame, void *user_data) {
  H2Session *se = user_data;
  if (frame->hd.type == NGHTTP2_GOAWAY && !SF(se, kSessionStateClosing) &&
      !session_destroyed(se) && !se->graceful_close_initiated && !se->goaway_initiated)
    se->internal_goaway_sent = true;
  return 0;
}

static int on_stream_close(nghttp2_session *handle, int32_t id, uint32_t code, void *user_data) {
  H2Session *se = user_data;
  Env *env = se->env;
  JSContext *ctx = env->ctx;
  H2Stream *st = find_stream(se, id);
  if (!st || stream_destroyed(st))
    return 0;
  st->flags |= kStreamStateClosed;
  st->code = (int32_t)code;
  if (env->can_call_into_js) {
    JSValue arg = JS_NewUint32(ctx, code);
    JSValue answer = call_cb(&st->s.hw.aw, se->st, CB_STREAM_CLOSE, 1, &arg);
    if (JS_IsException(answer) || JS_IsUninitialized(answer) ||
        (JS_IsBool(answer) && !JS_ToBool(ctx, answer)))
      stream_destroy(st);
    free_result(ctx, answer);
  }
  return 0;
}

static int on_invalid_header(nghttp2_session *handle, const nghttp2_frame *frame,
                             nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
                             void *user_data) {
  return 0; /* ignored, as Node does */
}

static int on_data_chunk_recv(nghttp2_session *handle, uint8_t flags, int32_t id,
                              const uint8_t *data, size_t len, void *user_data) {
  H2Session *se = user_data;
  H2Stream *st;
  char *copy;
  if (len == 0)
    return 0;
  if (SF(se, kSessionStateClosePending))
    return 0;
  nghttp2_session_consume_connection(handle, len);
  st = find_stream(se, id);
  if (!st || stream_destroyed(st))
    return 0;
  copy = malloc(len);
  memcpy(copy, data, len);
  node_stream_emit_read(&st->s, (ssize_t)len, copy);
  if (!stream_destroyed(st)) {
    if (stream_reading(st))
      nghttp2_session_consume_stream(handle, id, len);
    else
      st->inbound_consumed_data_while_paused += len;
    if (se->outgoing_length > 4096 || st->available_outbound_length > 4096)
      send_pending_data(se);
  }
  if (SF(se, kSessionStateWriteInProgress)) {
    se->flags |= kSessionStateReceivePaused;
    return NGHTTP2_ERR_PAUSE;
  }
  return 0;
}

static ssize_t on_select_padding(nghttp2_session *handle, const nghttp2_frame *frame,
                                 size_t max_payload_len, void *user_data) {
  H2Session *se = user_data;
  ssize_t padding = frame->hd.length;
  if (se->padding_strategy == PADDING_STRATEGY_MAX) {
    padding = (ssize_t)max_payload_len;
  } else if (se->padding_strategy == PADDING_STRATEGY_ALIGNED) {
    size_t r = (frame->hd.length + 9) % 8;
    if (r != 0) {
      size_t pad = frame->hd.length + (8 - r);
      padding = (ssize_t)(pad < max_payload_len ? pad : max_payload_len);
    }
  }
  return padding;
}

static int on_nghttp_error(nghttp2_session *handle, int lib_error_code, const char *msg,
                           size_t len, void *user_data) {
  H2Session *se = user_data;
  if (lib_error_code == NGHTTP2_ERR_SETTINGS_EXPECTED) {
    JSValue arg = JS_NewInt32(ctx_of(se), NGHTTP2_ERR_PROTO);
    call_cb_void(&se->aw, se->st, CB_ERROR, 1, &arg);
  }
  return 0;
}

/* ---------------------------------------------------------------------- */
/* reading the socket */

static void finish_close(H2Session *se, uint32_t code, bool socket_closed);

static void maybe_finish_pending_close(H2Session *se) {
  if (!SF(se, kSessionStateClosePending) || session_destroyed(se))
    return;
  se->flags &= ~kSessionStateClosePending;
  finish_close(se, se->pending_close_code, se->pending_close_socket_closed);
}

static void consume_http2_data(H2Session *se) {
  size_t read_len = se->in_len - se->in_off;
  ssize_t ret;
  se->flags &= ~kSessionStateReceivePaused;
  se->custom_recv_error_code = NULL;
  se->flags |= kSessionStateReceiving;
  ret = nghttp2_session_mem_recv(se->session, se->in_buf + se->in_off, read_len);
  se->flags &= ~kSessionStateReceiving;
  if (SF(se, kSessionStateReceivePaused)) {
    if (ret > 0)
      se->in_off += (size_t)ret;
    maybe_finish_pending_close(se);
    goto done;
  }
  se->current_session_memory -= se->in_len;
  free(se->in_buf);
  se->in_buf = NULL;
  se->in_len = se->in_off = 0;
  maybe_finish_pending_close(se);
done:
  if (SF(se, kSessionStateClosePending) && !session_destroyed(se)) {
    se->flags &= ~kSessionStateClosePending;
    finish_close(se, se->pending_close_code, se->pending_close_socket_closed);
  }
  if (ret >= 0 && !session_destroyed(se))
    send_pending_data(se);
  if (ret < 0) {
    JSContext *ctx = ctx_of(se);
    JSValue args[2];
    args[0] = JS_NewInt32(ctx, (int32_t)ret);
    args[1] = se->custom_recv_error_code ? JS_NewString(ctx, se->custom_recv_error_code) : JS_NULL;
    call_cb_void(&se->aw, se->st, CB_ERROR, 2, args);
    JS_FreeValue(ctx, args[1]);
  }
}

static void pass_read_error(H2Session *se, ssize_t nread) {
  StreamWrap *s = se->stream;
  if (!s)
    return;
  if (se->prev_read) {
    uv_buf_t b = uv_buf_init(NULL, 0);
    void *self = s->listener;
    s->listener = se->prev_listener;
    se->prev_read(s, nread, &b);
    if (s->listener == se->prev_listener)
      s->listener = self;
  } else {
    node_stream_emit_read_js(s, nread, NULL);
  }
}

static void session_alloc(StreamWrap *s, size_t suggested, uv_buf_t *buf) {
  buf->base = malloc(65536);
  buf->len = buf->base ? 65536 : 0;
}

/* data from the socket (data: malloc'ed, taken) */
static void session_receive(H2Session *se, ssize_t nread, char *data) {
  JSContext *ctx = ctx_of(se);
  bool owned;
  if (nread <= 0) {
    free(data);
    if (nread < 0)
      pass_read_error(se, nread);
    return;
  }
  owned = scope_enter(se);
  if (se->in_off == 0 && !se->in_buf) {
    char *p = realloc(data, (size_t)nread);
    se->in_buf = (uint8_t *)(p ? p : data);
    se->in_len = (size_t)nread;
  } else {
    size_t pending = se->in_len - se->in_off;
    uint8_t *nb = malloc(pending + (size_t)nread);
    memcpy(nb, se->in_buf + se->in_off, pending);
    memcpy(nb + pending, data, (size_t)nread);
    free(data);
    se->current_session_memory -= se->in_len;
    free(se->in_buf);
    se->in_buf = nb;
    se->in_len = pending + (size_t)nread;
    se->in_off = 0;
  }
  se->current_session_memory += (size_t)nread;
  if (se->session && !session_destroyed(se)) {
    consume_http2_data(se);
  } else if (se->session) {
    /* closed: let nghttp2 see the input anyway (it ignores it) */
    consume_http2_data(se);
  }
  maybe_stop_reading(se);
  scope_exit(se, owned);
  (void)ctx;
}

/* as in Node, no callback scope around this: each callback into JS is the
   outermost one and runs the ticks it queued (the 'response' event before
   the DATA that follows) */
static void session_read(StreamWrap *s, ssize_t nread, const uv_buf_t *buf) {
  H2Session *se = s->listener;
  Env *env = se->env;
  JSValue keep = JS_DupValue(env->ctx, se->aw.object);
  session_receive(se, nread, buf->base);
  JS_FreeValue(env->ctx, keep);
}

static void session_unconsume(H2Session *se) {
  StreamWrap *s = se->stream;
  if (!s)
    return;
  if (s->listener == se) {
    s->alloc_override = se->prev_alloc;
    s->read_override = se->prev_read;
    s->listener = se->prev_listener;
  }
  se->stream = NULL;
}

/* ---------------------------------------------------------------------- */
/* closing */

static void ping_done(H2Session *se, JSValue cb, uint64_t start, bool ack, const uint8_t *payload) {
  JSContext *ctx = ctx_of(se);
  JSValue argv[3];
  argv[0] = JS_NewBool(ctx, ack);
  argv[1] = JS_NewFloat64(ctx, (double)(uv_hrtime() - start) / 1e6);
  argv[2] = payload ? nb_new_buffer(ctx, payload, 8) : JS_UNDEFINED;
  free_result(ctx, async_wrap_make_callback(&se->aw, cb, 3, argv));
  JS_FreeValue(ctx, argv[2]);
}

typedef struct {
  H2Session *se;
  JSValue cb;
  uint64_t start;
} PingCancel;

static void ping_cancel_task(void *arg) {
  PingCancel *pc = arg;
  ping_done(pc->se, pc->cb, pc->start, false, NULL);
  JS_FreeValue(ctx_of(pc->se), pc->cb);
  free(pc);
}

static void finish_close(H2Session *se, uint32_t code, bool socket_closed) {
  JSContext *ctx = ctx_of(se);
  if (se->stream) {
    se->flags |= kSessionStateReadingStopped;
    node_stream_read_stop(se->stream);
  }
  if (!socket_closed) {
    nghttp2_session_terminate_session(se->session, code);
    send_pending_data(se);
  } else if (se->stream) {
    session_unconsume(se);
  }
  se->flags |= kSessionStateClosed;
  if (!SF(se, kSessionStateWriteInProgress) || !se->stream) {
    call_named_void(&se->aw, "ondone");
    if (se->stream) {
      se->flags &= ~kSessionStateReadingStopped;
      node_stream_read_start(se->stream);
    }
  }
  while (se->pings) {
    H2Ping *p = se->pings;
    PingCancel *pc = calloc(1, sizeof(*pc));
    se->pings = p->next;
    se->n_pings--;
    se->current_session_memory -= sizeof(*p);
    pc->se = se;
    pc->cb = p->cb;
    pc->start = p->start;
    set_immediate(se->st, ping_cancel_task, pc, JS_DupValue(ctx, se->aw.object));
    free(p);
  }
  se->pings_tail = NULL;
}

static void session_close(H2Session *se, uint32_t code, bool socket_closed) {
  if (SF(se, kSessionStateClosing))
    return;
  se->flags |= kSessionStateClosing;
  if (SF(se, kSessionStateReceiving)) {
    se->flags |= kSessionStateClosePending;
    se->pending_close_code = code;
    se->pending_close_socket_closed = socket_closed;
    return;
  }
  finish_close(se, code, socket_closed);
}

static void session_finalizer(JSRuntime *rt, JSValueConst val) {
  H2Session *se = JS_GetOpaque(val, session_class_id);
  size_t i;
  H2Stream *st, *next;
  if (!se)
    return;
  /* the streams first: they hold nghttp2's header buffers */
  for (i = 0; i < se->nbuckets; i++) {
    for (st = se->buckets[i]; st; st = next) {
      next = st->hash_next;
      st->in_map = false;
      st->hash_next = NULL;
      stream_clear_headers(st);
    }
  }
  for (st = se->all; st; st = next) {
    next = st->all_next;
    stream_clear_headers(st);
    st->session = NULL;
    st->all_next = st->all_prev = NULL;
  }
  se->all = NULL;
  /* the map's references go too (the objects may live on) */
  for (i = 0; i < se->nbuckets; i++)
    se->buckets[i] = NULL;
  if (se->session)
    nghttp2_session_del(se->session);
  se->session = NULL;
  free(se->buckets);
  free(se->in_buf);
  while (se->pings) {
    H2Ping *p = se->pings;
    se->pings = p->next;
    JS_FreeValueRT(rt, p->cb);
    free(p);
  }
  while (se->settings_head) {
    H2SettingsReq *r = se->settings_head;
    se->settings_head = r->next;
    JS_FreeValueRT(rt, r->cb);
    free(r);
  }
  for (i = 0; i < se->nout; i++) {
    JS_FreeValueRT(rt, se->out[i].req);
    if (se->out[i].owner)
      JS_FreeValueRT(rt, se->out[i].owner->s.hw.aw.object);
    free(se->out[i].free_ptr);
  }
  free(se->out);
  free(se->storage);
  free(se->pending_rst);
  session_unconsume(se);
  JS_FreeValueRT(rt, se->stream_obj);
  JS_FreeValueRT(rt, se->fields_arr);
  async_wrap_destroy(&se->aw);
  free(se);
}

/* the streams the map still references: their objects are released when the
   session is collected (the map holds them while the session lives) */
static void session_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  H2Session *se = JS_GetOpaque(val, session_class_id);
  size_t i;
  H2Stream *st;
  if (!se)
    return;
  for (i = 0; i < se->nbuckets; i++)
    for (st = se->buckets[i]; st; st = st->hash_next)
      JS_MarkValue(rt, st->s.hw.aw.object, mark);
  JS_MarkValue(rt, se->stream_obj, mark);
  JS_MarkValue(rt, se->fields_arr, mark);
}

/* ---------------------------------------------------------------------- */
/* settings */

static size_t settings_init(H2State *st, nghttp2_settings_entry *entries) {
  uint32_t *buf = st->settings, flags = buf[IDX_SETTINGS_COUNT], n, i;
  size_t count = 0;
  static const int32_t ids[IDX_SETTINGS_COUNT] = {
    NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, NGHTTP2_SETTINGS_ENABLE_PUSH,
    NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, NGHTTP2_SETTINGS_MAX_FRAME_SIZE,
    NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
    NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL,
  };
  for (i = 0; i < IDX_SETTINGS_COUNT; i++) {
    if (flags & (1u << i)) {
      entries[count].settings_id = ids[i];
      entries[count].value = buf[i];
      count++;
    }
  }
  n = buf[IDX_SETTINGS_COUNT + 1];
  if (n > MAX_ADDITIONAL_SETTINGS)
    n = MAX_ADDITIONAL_SETTINGS;
  for (i = 0; i < n; i++) {
    entries[count].settings_id = (int32_t)buf[IDX_SETTINGS_COUNT + 2 + i * 2];
    entries[count].value = buf[IDX_SETTINGS_COUNT + 2 + i * 2 + 1];
    count++;
  }
  return count;
}

static JSValue pack_settings(JSContext *ctx, size_t count, const nghttp2_settings_entry *e) {
  uint8_t buf[6 * (IDX_SETTINGS_COUNT + MAX_ADDITIONAL_SETTINGS)];
  ssize_t n = nghttp2_pack_settings_payload(buf, sizeof(buf), e, count);
  if (n < 0)
    return JS_UNDEFINED;
  return nb_new_buffer(ctx, buf, (size_t)n);
}

static void update_local_custom_settings(H2Session *se, size_t count,
                                         const nghttp2_settings_entry *entries) {
  size_t number = se->local_custom.number, i, j;
  for (i = 0; i < count; i++) {
    if (entries[i].settings_id >= IDX_SETTINGS_COUNT) {
      for (j = 0; j < number; j++) {
        if (se->local_custom.entries[j].settings_id == entries[i].settings_id) {
          se->local_custom.entries[j].value = entries[i].value;
          break;
        }
      }
      if (j == number && number < MAX_ADDITIONAL_SETTINGS) {
        se->local_custom.entries[number] = entries[i];
        number++;
      }
    }
  }
  se->local_custom.number = number;
}

/* local/remoteSettings(): the settings into settingsBuffer */
static void settings_update(H2Session *se, bool local) {
  uint32_t *buf = se->st->settings, count = 0;
  CustomSettings *cs = local ? &se->local_custom : &se->remote_custom;
  uint32_t (*fn)(nghttp2_session *, nghttp2_settings_id) =
      local ? nghttp2_session_get_local_settings : nghttp2_session_get_remote_settings;
  size_t i, j, imax;
  buf[IDX_SETTINGS_HEADER_TABLE_SIZE] = fn(se->session, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE);
  buf[IDX_SETTINGS_ENABLE_PUSH] = fn(se->session, NGHTTP2_SETTINGS_ENABLE_PUSH);
  buf[IDX_SETTINGS_INITIAL_WINDOW_SIZE] = fn(se->session, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE);
  buf[IDX_SETTINGS_MAX_FRAME_SIZE] = fn(se->session, NGHTTP2_SETTINGS_MAX_FRAME_SIZE);
  buf[IDX_SETTINGS_MAX_CONCURRENT_STREAMS] =
      fn(se->session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  buf[IDX_SETTINGS_MAX_HEADER_LIST_SIZE] = fn(se->session, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
  buf[IDX_SETTINGS_ENABLE_CONNECT_PROTOCOL] =
      fn(se->session, NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL);
  imax = cs->number < MAX_ADDITIONAL_SETTINGS ? cs->number : MAX_ADDITIONAL_SETTINGS;
  for (i = 0; i < imax; i++) {
    if (!(cs->entries[i].settings_id & ~0xffff)) {
      uint32_t id = (uint32_t)(cs->entries[i].settings_id & 0xffff);
      for (j = 0; j < count; j++) {
        if ((buf[IDX_SETTINGS_COUNT + 1 + j * 2 + 1] & 0xffff) == id) {
          buf[IDX_SETTINGS_COUNT + 1 + j * 2 + 1] = id;
          buf[IDX_SETTINGS_COUNT + 1 + j * 2 + 2] = cs->entries[i].value;
          break;
        }
      }
      if (j == count && count < MAX_ADDITIONAL_SETTINGS) {
        buf[IDX_SETTINGS_COUNT + 1 + count * 2 + 1] = id;
        buf[IDX_SETTINGS_COUNT + 1 + count * 2 + 2] = cs->entries[i].value;
        count++;
      }
    }
  }
  buf[IDX_SETTINGS_COUNT + 1] = count;
}

/* ---------------------------------------------------------------------- */
/* headers from JS: [string of "name\0value\0<flags>"..., count] */

typedef struct {
  nghttp2_nv *nva;
  size_t count;
  uint8_t *buf;
} H2Headers;

static void headers_free(H2Headers *h) {
  free(h->nva);
  free(h->buf);
}

static int headers_from_js(JSContext *ctx, JSValueConst arr, H2Headers *h) {
  JSValue str = JS_GetPropertyUint32(ctx, arr, 0), cnt = JS_GetPropertyUint32(ctx, arr, 1);
  uint32_t count = 0;
  size_t len, n = 0;
  uint8_t *p, *end;
  memset(h, 0, sizeof(*h));
  JS_ToUint32(ctx, &count, cnt);
  JS_FreeValue(ctx, cnt);
  if (!JS_IsString(str)) {
    JS_FreeValue(ctx, str);
    return -1;
  }
  len = node_string_bytes_size(ctx, str, ENC_LATIN1);
  h->buf = malloc(len + 1);
  len = node_string_write(ctx, h->buf, len, str, ENC_LATIN1, NULL);
  JS_FreeValue(ctx, str);
  h->nva = calloc(count ? count : 1, sizeof(nghttp2_nv));
  h->count = count;
  if (count == 0)
    return 0;
  p = h->buf;
  end = h->buf + len;
  while (p < end) {
    uint8_t *z;
    if (n >= count) {
      static uint8_t zero = 0;
      h->nva[0].name = h->nva[0].value = &zero;
      h->nva[0].namelen = h->nva[0].valuelen = 1;
      h->count = 1;
      return 0;
    }
    z = memchr(p, 0, end - p);
    if (!z)
      break;
    h->nva[n].name = p;
    h->nva[n].namelen = z - p;
    p = z + 1;
    z = memchr(p, 0, end - p);
    if (!z)
      break;
    h->nva[n].value = p;
    h->nva[n].valuelen = z - p;
    p = z + 1;
    h->nva[n].flags = p < end ? *p : 0;
    p++;
    n++;
  }
  h->count = n;
  return 0;
}

static void priority_from_js(JSContext *ctx, JSValueConst parent, JSValueConst weight,
                             JSValueConst exclusive, nghttp2_priority_spec *spec) {
  int32_t p, w;
  if (JS_ToInt32(ctx, &p, parent) || JS_ToInt32(ctx, &w, weight)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    nghttp2_priority_spec_init(spec, 0, 0, 0);
    return;
  }
  nghttp2_priority_spec_init(spec, p, w, JS_ToBool(ctx, exclusive) ? 1 : 0);
}

/* ---------------------------------------------------------------------- */
/* Http2Session's JS API */

static H2Session *session_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, session_class_id);
}

static H2Stream *stream_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, stream_class_id);
}

/* what the JS call left behind: an exception thrown by a callback */
static JSValue ret_or_exc(JSContext *ctx, JSValue r) {
  if (JS_HasException(ctx)) {
    JS_FreeValue(ctx, r);
    return JS_EXCEPTION;
  }
  return r;
}

static void *ab_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

static nghttp2_session_callbacks *make_callbacks(bool padding) {
  nghttp2_session_callbacks *cb;
  nghttp2_session_callbacks_new(&cb);
  nghttp2_session_callbacks_set_on_begin_headers_callback(cb, on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback2(cb, on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cb, on_frame_recv);
  nghttp2_session_callbacks_set_on_stream_close_callback(cb, on_stream_close);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, on_data_chunk_recv);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(cb, on_frame_not_send);
  nghttp2_session_callbacks_set_on_invalid_header_callback2(cb, on_invalid_header);
  nghttp2_session_callbacks_set_error_callback2(cb, on_nghttp_error);
  nghttp2_session_callbacks_set_send_data_callback(cb, on_send_data);
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(cb, on_invalid_frame);
  nghttp2_session_callbacks_set_on_frame_send_callback(cb, on_frame_send);
  if (padding)
    nghttp2_session_callbacks_set_select_padding_callback(cb, on_select_padding);
  return cb;
}

/* new Http2Session(type) */
static JSValue session_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  H2State *st = state_of(env);
  int32_t type = nb_int32(ctx, ARG(0), SESSION_SERVER);
  JSValue obj = nb_new_instance(ctx, nt, session_class_id);
  H2Session *se;
  nghttp2_option *opt;
  nghttp2_session_callbacks *cbs;
  uint32_t *buf = st->options, flags = buf[IDX_OPTIONS_FLAGS], max_pairs;
  uint8_t *fields;
  int r;
  if (JS_IsException(obj))
    return obj;
  se = calloc(1, sizeof(*se));
  se->env = env;
  se->st = st;
  se->type = type;
  se->stream_obj = JS_UNDEFINED;
  se->fields_arr = JS_UNDEFINED;
  async_wrap_init(&se->aw, env, obj, PROVIDER_HTTP2SESSION, -1);
  JS_SetOpaque(obj, se);

  nghttp2_option_new(&opt);
  nghttp2_option_set_no_closed_streams(opt, 1);
  nghttp2_option_set_no_auto_window_update(opt, 1);
  if (type == SESSION_CLIENT) {
    nghttp2_option_set_builtin_recv_extension_type(opt, NGHTTP2_ALTSVC);
    nghttp2_option_set_builtin_recv_extension_type(opt, NGHTTP2_ORIGIN);
  }
  if (flags & (1 << IDX_OPTIONS_MAX_DEFLATE_DYNAMIC_TABLE_SIZE))
    nghttp2_option_set_max_deflate_dynamic_table_size(
        opt, buf[IDX_OPTIONS_MAX_DEFLATE_DYNAMIC_TABLE_SIZE]);
  if (flags & (1 << IDX_OPTIONS_MAX_RESERVED_REMOTE_STREAMS))
    nghttp2_option_set_max_reserved_remote_streams(opt,
                                                   buf[IDX_OPTIONS_MAX_RESERVED_REMOTE_STREAMS]);
  if (flags & (1 << IDX_OPTIONS_MAX_SEND_HEADER_BLOCK_LENGTH))
    nghttp2_option_set_max_send_header_block_length(
        opt, buf[IDX_OPTIONS_MAX_SEND_HEADER_BLOCK_LENGTH]);
  nghttp2_option_set_peer_max_concurrent_streams(opt, 100);
  if (flags & (1 << IDX_OPTIONS_PEER_MAX_CONCURRENT_STREAMS))
    nghttp2_option_set_peer_max_concurrent_streams(opt,
                                                   buf[IDX_OPTIONS_PEER_MAX_CONCURRENT_STREAMS]);
  if (flags & (1 << IDX_OPTIONS_STRICT_HTTP_FIELD_WHITESPACE_VALIDATION))
    nghttp2_option_set_no_rfc9113_leading_and_trailing_ws_validation(
        opt, buf[IDX_OPTIONS_STRICT_HTTP_FIELD_WHITESPACE_VALIDATION]);
  se->padding_strategy = PADDING_STRATEGY_NONE;
  if (flags & (1 << IDX_OPTIONS_PADDING_STRATEGY))
    se->padding_strategy = (int)buf[IDX_OPTIONS_PADDING_STRATEGY];
  max_pairs = DEFAULT_MAX_HEADER_LIST_PAIRS;
  if (flags & (1 << IDX_OPTIONS_MAX_HEADER_LIST_PAIRS))
    max_pairs = buf[IDX_OPTIONS_MAX_HEADER_LIST_PAIRS];
  if (type == SESSION_SERVER)
    se->max_header_pairs = max_pairs < 4 ? 4 : max_pairs;
  else
    se->max_header_pairs = max_pairs < 1 ? 1 : max_pairs;
  se->max_outstanding_pings = kDefaultMaxPings;
  if (flags & (1 << IDX_OPTIONS_MAX_OUTSTANDING_PINGS))
    se->max_outstanding_pings = buf[IDX_OPTIONS_MAX_OUTSTANDING_PINGS];
  se->max_outstanding_settings = kDefaultMaxSettings;
  if (flags & (1 << IDX_OPTIONS_MAX_OUTSTANDING_SETTINGS))
    se->max_outstanding_settings = buf[IDX_OPTIONS_MAX_OUTSTANDING_SETTINGS];
  se->max_session_memory = kDefaultMaxSessionMemory;
  if (flags & (1 << IDX_OPTIONS_MAX_SESSION_MEMORY))
    se->max_session_memory = (uint64_t)buf[IDX_OPTIONS_MAX_SESSION_MEMORY] * 1000000ull;
  if (flags & (1 << IDX_OPTIONS_MAX_SETTINGS))
    nghttp2_option_set_max_settings(opt, buf[IDX_OPTIONS_MAX_SETTINGS]);
  if ((flags & (1 << IDX_OPTIONS_STREAM_RESET_BURST)) &&
      (flags & (1 << IDX_OPTIONS_STREAM_RESET_RATE)))
    nghttp2_option_set_stream_reset_rate_limit(opt, buf[IDX_OPTIONS_STREAM_RESET_BURST],
                                               buf[IDX_OPTIONS_STREAM_RESET_RATE]);

  /* the custom settings JS allows the peer to send */
  {
    uint32_t *sb = st->settings, n = sb[IDX_SETTINGS_COUNT + 1], i;
    if (n > MAX_ADDITIONAL_SETTINGS)
      n = MAX_ADDITIONAL_SETTINGS;
    for (i = 0; i < n; i++) {
      se->remote_custom.entries[i].settings_id =
          (int32_t)((sb[IDX_SETTINGS_COUNT + 2 + i * 2] & 0xffff) | (1 << 16));
      se->remote_custom.entries[i].value = 0;
    }
    se->remote_custom.number = n;
  }

  cbs = make_callbacks(se->padding_strategy != PADDING_STRATEGY_NONE);
  r = type == SESSION_SERVER ? nghttp2_session_server_new2(&se->session, cbs, se, opt)
                             : nghttp2_session_client_new2(&se->session, cbs, se, opt);
  nghttp2_session_callbacks_del(cbs);
  nghttp2_option_del(opt);
  if (r != 0) {
    JS_FreeValue(ctx, obj);
    return JS_ThrowOutOfMemory(ctx);
  }

  fields = calloc(1, kSessionUint8FieldCount);
  {
    uint32_t mif = 1000, mrs = 100;
    memcpy(fields + kSessionMaxInvalidFrames, &mif, 4);
    memcpy(fields + kSessionMaxRejectedStreams, &mrs, 4);
  }
  se->fields = fields;
  se->fields_arr = JS_NewUint8Array(ctx, fields, kSessionUint8FieldCount, ab_realloc, NULL, false);
  JS_SetPropertyStr(ctx, obj, "fields", JS_DupValue(ctx, se->fields_arr));
  return obj;
}

static JSValue session_consume(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  StreamWrap *s;
  if (!se)
    return JS_EXCEPTION;
  s = node_stream_wrap_of(ARG(0));
  if (!s)
    return JS_ThrowTypeError(ctx, "Http2Session.consume: not a native stream");
  session_unconsume(se);
  JS_FreeValue(ctx, se->stream_obj);
  se->stream_obj = JS_DupValue(ctx, ARG(0));
  se->stream = s;
  se->prev_alloc = s->alloc_override;
  se->prev_read = s->read_override;
  se->prev_listener = s->listener;
  s->alloc_override = session_alloc;
  s->read_override = session_read;
  s->listener = se;
  return JS_UNDEFINED;
}

static JSValue session_receive_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  size_t len;
  uint8_t *d;
  char *copy;
  if (!se)
    return JS_EXCEPTION;
  d = nb_buffer_data(ctx, ARG(0), &len);
  if (!d || !len)
    return JS_UNDEFINED;
  copy = malloc(len);
  memcpy(copy, d, len);
  session_receive(se, (ssize_t)len, copy);
  return ret_or_exc(ctx, JS_UNDEFINED);
}

static JSValue session_destroy_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  uint32_t code = 0;
  if (!se)
    return JS_EXCEPTION;
  if (JS_ToUint32(ctx, &code, ARG(0)))
    return JS_EXCEPTION;
  session_close(se, code, JS_ToBool(ctx, ARG(1)));
  return ret_or_exc(ctx, JS_UNDEFINED);
}

static JSValue session_has_pending_data(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  if (!se || !se->session)
    return JS_FALSE;
  return JS_NewBool(ctx, nghttp2_session_want_write(se->session) != 0 ||
                             nghttp2_session_want_read(se->session) != 0);
}

static JSValue session_settings_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  H2SettingsReq *r;
  bool owned;
  if (!se)
    return JS_EXCEPTION;
  if (!JS_IsFunction(ctx, ARG(0)) || session_destroyed(se))
    return JS_FALSE;
  r = calloc(1, sizeof(*r));
  r->cb = JS_DupValue(ctx, ARG(0));
  r->start = 0;
  r->count = settings_init(se->st, r->entries);
  if (se->n_settings == se->max_outstanding_settings) {
    settings_done(se, r, false);
    JS_FreeValue(ctx, r->cb);
    free(r);
    return ret_or_exc(ctx, JS_FALSE);
  }
  se->current_session_memory += sizeof(*r);
  owned = scope_enter(se);
  update_local_custom_settings(se, r->count, r->entries);
  nghttp2_submit_settings(se->session, NGHTTP2_FLAG_NONE, r->entries, r->count);
  scope_exit(se, owned);
  if (se->settings_tail)
    se->settings_tail->next = r;
  else
    se->settings_head = r;
  se->settings_tail = r;
  se->n_settings++;
  return JS_TRUE;
}

/* request(headers, options, parent, weight, exclusive) */
static JSValue session_request(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  H2Headers h;
  nghttp2_priority_spec pri;
  nghttp2_data_provider prov;
  int32_t options = 0, ret;
  H2Stream *st = NULL;
  bool owned;
  if (!se)
    return JS_EXCEPTION;
  if (session_destroyed(se))
    return JS_NewInt32(ctx, NGHTTP2_ERR_INVALID_STATE);
  if (JS_ToInt32(ctx, &options, ARG(1)))
    return JS_EXCEPTION;
  if (headers_from_js(ctx, ARG(0), &h) < 0)
    return JS_ThrowTypeError(ctx, "invalid headers");
  priority_from_js(ctx, ARG(2), ARG(3), ARG(4), &pri);
  owned = scope_enter(se);
  memset(&prov, 0, sizeof(prov));
  ret = nghttp2_submit_request(se->session, &pri, h.nva, h.count, provider(&prov, NULL, options),
                               NULL);
  if (ret > 0)
    st = stream_new(se, ret, NGHTTP2_HCAT_HEADERS, options);
  scope_exit(se, owned);
  headers_free(&h);
  if (ret <= 0 || !st)
    return JS_NewInt32(ctx, ret);
  return JS_DupValue(ctx, st->s.hw.aw.object);
}

static JSValue session_set_next_stream_id(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  int32_t id;
  if (!se || JS_ToInt32(ctx, &id, ARG(0)))
    return JS_EXCEPTION;
  return JS_NewBool(ctx, nghttp2_session_set_next_stream_id(se->session, id) >= 0);
}

static JSValue session_set_local_window_size(JSContext *ctx, JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  int32_t size;
  bool owned;
  int r;
  if (!se || JS_ToInt32(ctx, &size, ARG(0)))
    return JS_EXCEPTION;
  owned = scope_enter(se);
  r = nghttp2_session_set_local_window_size(se->session, NGHTTP2_FLAG_NONE, 0, size);
  scope_exit(se, owned);
  return JS_NewInt32(ctx, r);
}

static JSValue session_goaway(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  uint32_t code;
  int32_t last;
  size_t len = 0;
  uint8_t *data = NULL;
  bool owned;
  if (!se)
    return JS_EXCEPTION;
  if (JS_ToUint32(ctx, &code, ARG(0)) || JS_ToInt32(ctx, &last, ARG(1)))
    return JS_EXCEPTION;
  if (JS_IsObject(ARG(2)))
    data = nb_buffer_data(ctx, ARG(2), &len);
  if (session_destroyed(se))
    return JS_UNDEFINED;
  se->goaway_initiated = true;
  owned = scope_enter(se);
  if (last <= 0)
    last = nghttp2_session_get_last_proc_stream_id(se->session);
  nghttp2_submit_goaway(se->session, NGHTTP2_FLAG_NONE, last, code, data, len);
  scope_exit(se, owned);
  return JS_UNDEFINED;
}

static JSValue session_update_chunks_sent(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  if (!se)
    return JS_EXCEPTION;
  JS_SetPropertyStr(ctx, this_val, "chunksSentSinceLastWrite",
                    JS_NewUint32(ctx, se->chunks_sent_since_last_write));
  return JS_NewUint32(ctx, se->chunks_sent_since_last_write);
}

static JSValue session_refresh_state(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  double *b;
  nghttp2_session *s;
  if (!se)
    return JS_EXCEPTION;
  if (!se->session)
    return JS_UNDEFINED;
  b = se->st->session_state;
  s = se->session;
  b[IDX_SESSION_STATE_EFFECTIVE_LOCAL_WINDOW_SIZE] =
      nghttp2_session_get_effective_local_window_size(s);
  b[IDX_SESSION_STATE_EFFECTIVE_RECV_DATA_LENGTH] =
      nghttp2_session_get_effective_recv_data_length(s);
  b[IDX_SESSION_STATE_NEXT_STREAM_ID] = nghttp2_session_get_next_stream_id(s);
  b[IDX_SESSION_STATE_LOCAL_WINDOW_SIZE] = nghttp2_session_get_local_window_size(s);
  b[IDX_SESSION_STATE_LAST_PROC_STREAM_ID] = nghttp2_session_get_last_proc_stream_id(s);
  b[IDX_SESSION_STATE_REMOTE_WINDOW_SIZE] = nghttp2_session_get_remote_window_size(s);
  b[IDX_SESSION_STATE_OUTBOUND_QUEUE_SIZE] = (double)nghttp2_session_get_outbound_queue_size(s);
  b[IDX_SESSION_STATE_HD_DEFLATE_DYNAMIC_TABLE_SIZE] =
      (double)nghttp2_session_get_hd_deflate_dynamic_table_size(s);
  b[IDX_SESSION_STATE_HD_INFLATE_DYNAMIC_TABLE_SIZE] =
      (double)nghttp2_session_get_hd_inflate_dynamic_table_size(s);
  return JS_UNDEFINED;
}

static JSValue session_ping(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  H2Ping *p;
  uint8_t data[8];
  const uint8_t *payload = NULL;
  size_t len = 0;
  bool owned;
  if (!se)
    return JS_EXCEPTION;
  if (JS_IsObject(ARG(0))) {
    payload = nb_buffer_data(ctx, ARG(0), &len);
    if (payload && len != 8)
      return JS_ThrowRangeError(ctx, "ping payload must be 8 bytes");
  }
  if (!JS_IsFunction(ctx, ARG(1)))
    return JS_ThrowTypeError(ctx, "callback must be a function");
  if (session_destroyed(se))
    return JS_FALSE;
  p = calloc(1, sizeof(*p));
  p->cb = JS_DupValue(ctx, ARG(1));
  p->start = uv_hrtime();
  if (se->n_pings == se->max_outstanding_pings) {
    ping_done(se, p->cb, p->start, false, NULL);
    JS_FreeValue(ctx, p->cb);
    free(p);
    return ret_or_exc(ctx, JS_FALSE);
  }
  se->current_session_memory += sizeof(*p);
  if (!payload) {
    memcpy(data, &p->start, 8);
    payload = data;
  }
  owned = scope_enter(se);
  nghttp2_submit_ping(se->session, NGHTTP2_FLAG_NONE, payload);
  scope_exit(se, owned);
  if (se->pings_tail)
    se->pings_tail->next = p;
  else
    se->pings = p;
  se->pings_tail = p;
  se->n_pings++;
  return JS_TRUE;
}

static JSValue session_altsvc(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  int32_t id;
  JSValue os, vs;
  size_t olen, vlen;
  uint8_t *o, *v;
  bool owned;
  if (!se || JS_ToInt32(ctx, &id, ARG(0)))
    return JS_EXCEPTION;
  os = JS_ToString(ctx, ARG(1));
  vs = JS_ToString(ctx, ARG(2));
  if (JS_IsException(os) || JS_IsException(vs)) {
    JS_FreeValue(ctx, os);
    JS_FreeValue(ctx, vs);
    return JS_EXCEPTION;
  }
  olen = node_string_bytes_size(ctx, os, ENC_LATIN1);
  vlen = node_string_bytes_size(ctx, vs, ENC_LATIN1);
  o = malloc(olen + 1);
  v = malloc(vlen + 1);
  olen = node_string_write(ctx, o, olen, os, ENC_LATIN1, NULL);
  vlen = node_string_write(ctx, v, vlen, vs, ENC_LATIN1, NULL);
  owned = scope_enter(se);
  nghttp2_submit_altsvc(se->session, NGHTTP2_FLAG_NONE, id, o, olen, v, vlen);
  scope_exit(se, owned);
  free(o);
  free(v);
  JS_FreeValue(ctx, os);
  JS_FreeValue(ctx, vs);
  return JS_UNDEFINED;
}

static JSValue session_origin(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  int32_t count;
  size_t len, n = 0;
  uint8_t *buf, *p, *end;
  nghttp2_origin_entry *ov;
  bool owned;
  if (!se || JS_ToInt32(ctx, &count, ARG(1)) || !JS_IsString(ARG(0)))
    return JS_EXCEPTION;
  len = node_string_bytes_size(ctx, ARG(0), ENC_LATIN1);
  buf = malloc(len + 1);
  len = node_string_write(ctx, buf, len, ARG(0), ENC_LATIN1, NULL);
  ov = calloc(count > 0 ? count : 1, sizeof(*ov));
  p = buf;
  end = buf + len;
  while (p < end && n < (size_t)count) {
    uint8_t *z = memchr(p, 0, end - p);
    size_t l = z ? (size_t)(z - p) : (size_t)(end - p);
    ov[n].origin = p;
    ov[n].origin_len = l;
    n++;
    p += l + 1;
  }
  owned = scope_enter(se);
  nghttp2_submit_origin(se->session, NGHTTP2_FLAG_NONE, n ? ov : NULL, n);
  scope_exit(se, owned);
  free(ov);
  free(buf);
  return JS_UNDEFINED;
}

static JSValue session_settings_refresh(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv, int local) {
  H2Session *se = session_of(ctx, this_val);
  if (!se)
    return JS_EXCEPTION;
  if (se->session)
    settings_update(se, local != 0);
  return JS_UNDEFINED;
}

static JSValue session_set_graceful_close(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  H2Session *se = session_of(ctx, this_val);
  if (!se)
    return JS_EXCEPTION;
  se->graceful_close_initiated = true;
  return JS_UNDEFINED;
}

/* AsyncWrap methods for both classes */
static AsyncWrap *aw_of(JSValueConst v) {
  JSClassID id;
  void *p = JS_GetAnyOpaque(v, &id);
  if (!p)
    return NULL;
  if (id == session_class_id)
    return &((H2Session *)p)->aw;
  if (id == stream_class_id)
    return &((H2Stream *)p)->s.hw.aw;
  return NULL;
}

static JSValue aw_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  AsyncWrap *w = aw_of(this_val);
  return JS_NewFloat64(ctx, w ? w->async_id : -1);
}

static JSValue aw_get_provider_type(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  AsyncWrap *w = aw_of(this_val);
  return JS_NewInt32(ctx, w ? w->provider : 0);
}

static JSValue aw_async_reset(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  AsyncWrap *w = aw_of(this_val);
  if (w)
    async_wrap_reset(w, ARG(0));
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry session_proto[] = {
  JS_CFUNC_DEF("origin", 2, session_origin),
  JS_CFUNC_DEF("altsvc", 3, session_altsvc),
  JS_CFUNC_DEF("ping", 2, session_ping),
  JS_CFUNC_DEF("consume", 1, session_consume),
  JS_CFUNC_DEF("receive", 1, session_receive_fn),
  JS_CFUNC_DEF("destroy", 2, session_destroy_fn),
  JS_CFUNC_DEF("goaway", 3, session_goaway),
  JS_CFUNC_DEF("hasPendingData", 0, session_has_pending_data),
  JS_CFUNC_DEF("settings", 1, session_settings_fn),
  JS_CFUNC_DEF("request", 5, session_request),
  JS_CFUNC_DEF("setNextStreamID", 1, session_set_next_stream_id),
  JS_CFUNC_DEF("setLocalWindowSize", 1, session_set_local_window_size),
  JS_CFUNC_DEF("updateChunksSent", 0, session_update_chunks_sent),
  JS_CFUNC_DEF("refreshState", 0, session_refresh_state),
  JS_CFUNC_MAGIC_DEF("localSettings", 0, session_settings_refresh, 1),
  JS_CFUNC_MAGIC_DEF("remoteSettings", 0, session_settings_refresh, 0),
  JS_CFUNC_DEF("setGracefulClose", 0, session_set_graceful_close),
  JS_CFUNC_DEF("getAsyncId", 0, aw_get_async_id),
  JS_CFUNC_DEF("getProviderType", 0, aw_get_provider_type),
  JS_CFUNC_DEF("asyncReset", 1, aw_async_reset),
};

/* ---------------------------------------------------------------------- */
/* Http2Stream's JS API */

static JSValue stream_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static JSValue stream_get_id(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  if (!st)
    return JS_EXCEPTION;
  return JS_NewInt32(ctx, st->id);
}

static JSValue stream_destroy_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  if (!st)
    return JS_EXCEPTION;
  stream_destroy(st);
  return ret_or_exc(ctx, JS_UNDEFINED);
}

static JSValue stream_priority(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  nghttp2_priority_spec pri;
  bool owned;
  if (!st)
    return JS_EXCEPTION;
  if (!st->session || stream_destroyed(st))
    return JS_UNDEFINED;
  priority_from_js(ctx, ARG(0), ARG(1), ARG(2), &pri);
  owned = scope_enter(st->session);
  if (JS_ToBool(ctx, ARG(3)))
    nghttp2_session_change_stream_priority(st->session->session, st->id, &pri);
  else
    nghttp2_submit_priority(st->session->session, NGHTTP2_FLAG_NONE, st->id, &pri);
  scope_exit(st->session, owned);
  return JS_UNDEFINED;
}

static JSValue stream_push_promise(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  H2Stream *parent = stream_of(ctx, this_val), *st = NULL;
  H2Session *se;
  H2Headers h;
  int32_t options = 0, ret;
  bool owned;
  if (!parent)
    return JS_EXCEPTION;
  se = parent->session;
  if (!se || stream_destroyed(parent))
    return JS_NewInt32(ctx, NGHTTP2_ERR_INVALID_STATE);
  if (JS_ToInt32(ctx, &options, ARG(1)))
    return JS_EXCEPTION;
  if (headers_from_js(ctx, ARG(0), &h) < 0)
    return JS_ThrowTypeError(ctx, "invalid headers");
  owned = scope_enter(se);
  ret = nghttp2_submit_push_promise(se->session, NGHTTP2_FLAG_NONE, parent->id, h.nva, h.count,
                                    NULL);
  if (ret > 0)
    st = stream_new(se, ret, NGHTTP2_HCAT_HEADERS, options);
  scope_exit(se, owned);
  headers_free(&h);
  if (ret <= 0 || !st)
    return JS_NewInt32(ctx, ret);
  return JS_DupValue(ctx, st->s.hw.aw.object);
}

static JSValue stream_info(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  H2Headers h;
  bool owned;
  int r;
  if (!st)
    return JS_EXCEPTION;
  if (!st->session || stream_destroyed(st))
    return JS_NewInt32(ctx, NGHTTP2_ERR_INVALID_STATE);
  if (headers_from_js(ctx, ARG(0), &h) < 0)
    return JS_ThrowTypeError(ctx, "invalid headers");
  owned = scope_enter(st->session);
  r = nghttp2_submit_headers(st->session->session, NGHTTP2_FLAG_NONE, st->id, NULL, h.nva,
                             h.count, NULL);
  scope_exit(st->session, owned);
  headers_free(&h);
  return JS_NewInt32(ctx, r);
}

static JSValue stream_trailers(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  H2Headers h;
  bool owned;
  int r;
  if (!st)
    return JS_EXCEPTION;
  if (!st->session || stream_destroyed(st))
    return JS_NewInt32(ctx, NGHTTP2_ERR_INVALID_STATE);
  if (headers_from_js(ctx, ARG(0), &h) < 0)
    return JS_ThrowTypeError(ctx, "invalid headers");
  owned = scope_enter(st->session);
  if (h.count == 0) {
    /* empty trailers: an empty DATA frame with END_STREAM instead */
    nghttp2_data_provider prov;
    memset(&prov, 0, sizeof(prov));
    r = nghttp2_submit_data(st->session->session, NGHTTP2_FLAG_END_STREAM, st->id,
                            provider(&prov, st, 0));
  } else {
    r = nghttp2_submit_trailer(st->session->session, st->id, h.nva, h.count);
  }
  scope_exit(st->session, owned);
  headers_free(&h);
  return JS_NewInt32(ctx, r);
}

static JSValue stream_respond(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  H2Headers h;
  nghttp2_data_provider prov;
  int32_t options = 0;
  bool owned;
  int r;
  if (!st)
    return JS_EXCEPTION;
  if (!st->session || stream_destroyed(st))
    return JS_NewInt32(ctx, NGHTTP2_ERR_INVALID_STATE);
  if (JS_ToInt32(ctx, &options, ARG(1)))
    return JS_EXCEPTION;
  if (headers_from_js(ctx, ARG(0), &h) < 0)
    return JS_ThrowTypeError(ctx, "invalid headers");
  owned = scope_enter(st->session);
  if (options & STREAM_OPTION_GET_TRAILERS)
    st->flags |= kStreamStateTrailers;
  if (!stream_writable(st))
    options |= STREAM_OPTION_EMPTY_PAYLOAD;
  memset(&prov, 0, sizeof(prov));
  r = nghttp2_submit_response(st->session->session, st->id, h.nva, h.count,
                              provider(&prov, st, options));
  scope_exit(st->session, owned);
  headers_free(&h);
  return JS_NewInt32(ctx, r);
}

static JSValue stream_rst_stream(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  uint32_t code;
  if (!st)
    return JS_EXCEPTION;
  if (JS_ToUint32(ctx, &code, ARG(0)))
    return JS_EXCEPTION;
  if (!stream_destroyed(st))
    submit_rst_stream(st, code);
  return ret_or_exc(ctx, JS_UNDEFINED);
}

static JSValue stream_refresh_state(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  H2Stream *st = stream_of(ctx, this_val);
  double *b;
  nghttp2_stream *ns;
  nghttp2_session *s;
  if (!st)
    return JS_EXCEPTION;
  if (!st->session || !st->session->session)
    return JS_UNDEFINED;
  b = st->session->st->stream_state;
  s = st->session->session;
  ns = nghttp2_session_find_stream(s, st->id);
  if (!ns) {
    b[IDX_STREAM_STATE] = NGHTTP2_STREAM_STATE_IDLE;
    b[IDX_STREAM_STATE_WEIGHT] = b[IDX_STREAM_STATE_SUM_DEPENDENCY_WEIGHT] =
        b[IDX_STREAM_STATE_LOCAL_CLOSE] = b[IDX_STREAM_STATE_REMOTE_CLOSE] =
            b[IDX_STREAM_STATE_LOCAL_WINDOW_SIZE] = 0;
  } else {
    b[IDX_STREAM_STATE] = nghttp2_stream_get_state(ns);
    b[IDX_STREAM_STATE_WEIGHT] = nghttp2_stream_get_weight(ns);
    b[IDX_STREAM_STATE_SUM_DEPENDENCY_WEIGHT] = nghttp2_stream_get_sum_dependency_weight(ns);
    b[IDX_STREAM_STATE_LOCAL_CLOSE] = nghttp2_session_get_stream_local_close(s, st->id);
    b[IDX_STREAM_STATE_REMOTE_CLOSE] = nghttp2_session_get_stream_remote_close(s, st->id);
    b[IDX_STREAM_STATE_LOCAL_WINDOW_SIZE] =
        nghttp2_session_get_stream_local_window_size(s, st->id);
  }
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry stream_proto[] = {
  JS_CFUNC_DEF("id", 0, stream_get_id),
  JS_CFUNC_DEF("destroy", 0, stream_destroy_fn),
  JS_CFUNC_DEF("priority", 4, stream_priority),
  JS_CFUNC_DEF("pushPromise", 2, stream_push_promise),
  JS_CFUNC_DEF("info", 1, stream_info),
  JS_CFUNC_DEF("trailers", 1, stream_trailers),
  JS_CFUNC_DEF("respond", 2, stream_respond),
  JS_CFUNC_DEF("rstStream", 1, stream_rst_stream),
  JS_CFUNC_DEF("refreshState", 0, stream_refresh_state),
  JS_CFUNC_DEF("getAsyncId", 0, aw_get_async_id),
  JS_CFUNC_DEF("getProviderType", 0, aw_get_provider_type),
  JS_CFUNC_DEF("asyncReset", 1, aw_async_reset),
};

/* ---------------------------------------------------------------------- */
/* module functions */

static JSValue h2_error_string(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  return JS_NewString(ctx, nghttp2_strerror(nb_int32(ctx, ARG(0), 0)));
}

static JSValue h2_refresh_default_settings(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  uint32_t *b = state_of(env_get(ctx))->settings;
  b[IDX_SETTINGS_HEADER_TABLE_SIZE] = DEFAULT_SETTINGS_HEADER_TABLE_SIZE;
  b[IDX_SETTINGS_ENABLE_PUSH] = DEFAULT_SETTINGS_ENABLE_PUSH;
  b[IDX_SETTINGS_INITIAL_WINDOW_SIZE] = DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE;
  b[IDX_SETTINGS_MAX_FRAME_SIZE] = DEFAULT_SETTINGS_MAX_FRAME_SIZE;
  b[IDX_SETTINGS_MAX_CONCURRENT_STREAMS] = DEFAULT_SETTINGS_MAX_CONCURRENT_STREAMS;
  b[IDX_SETTINGS_MAX_HEADER_LIST_SIZE] = DEFAULT_SETTINGS_MAX_HEADER_LIST_SIZE;
  b[IDX_SETTINGS_ENABLE_CONNECT_PROTOCOL] = DEFAULT_SETTINGS_ENABLE_CONNECT_PROTOCOL;
  b[IDX_SETTINGS_COUNT] = (1u << IDX_SETTINGS_COUNT) - 1;
  b[IDX_SETTINGS_COUNT + 1] = 0;
  return JS_UNDEFINED;
}

static JSValue h2_pack_settings(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  nghttp2_settings_entry e[IDX_SETTINGS_COUNT + MAX_ADDITIONAL_SETTINGS];
  size_t n = settings_init(state_of(env_get(ctx)), e);
  return pack_settings(ctx, n, e);
}

/* setCallbackFunctions(error, priority, settings, ping, headers, frameError,
   goawayData, altsvc, origin, streamTrailers, streamClose) */
static JSValue h2_set_callback_functions(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  Env *env = env_get(ctx);
  H2State *st = state_of(env);
  JSValue holder = env_binding_data(env, "http2");
  int i;
  for (i = 0; i < CB_COUNT; i++) {
    char name[16];
    JSValue fn = ARG(i);
    if (!JS_IsFunction(ctx, fn))
      continue;
    snprintf(name, sizeof(name), "cb%d", i);
    JS_SetPropertyStr(ctx, holder, name, JS_DupValue(ctx, fn));
    st->cb[i] = fn; /* borrowed: the holder owns it */
  }
  st->cb_set = true;
  JS_FreeValue(ctx, holder);
  return JS_UNDEFINED;
}

JSValue binding_init_http2(Env *env) {
  JSContext *ctx = env->ctx;
  H2State *st = state_of(env);
  JSValue t = JS_NewObject(ctx), constants, names, ctor, proto;
  NodeClassDef sdef = { .name = "Http2Session", .class_id = &session_class_id,
                        .ctor = session_ctor, .ctor_length = 1,
                        .finalizer = session_finalizer, .gc_mark = session_gc_mark,
                        .proto_funcs = session_proto, .proto_funcs_count = countof(session_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef tdef = { .name = "Http2Stream", .class_id = &stream_class_id,
                        .ctor = stream_ctor, .finalizer = stream_finalizer,
                        .proto_funcs = stream_proto, .proto_funcs_count = countof(stream_proto),
                        .parent_ctor = JS_UNDEFINED };
  nb_set(ctx, t, "sessionState",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, st->session_state,
                            IDX_SESSION_STATE_COUNT, 8));
  nb_set(ctx, t, "streamState",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, st->stream_state,
                            IDX_STREAM_STATE_COUNT, 8));
  nb_set(ctx, t, "settingsBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, st->settings, SETTINGS_BUFFER_LEN, 4));
  nb_set(ctx, t, "optionsBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, st->options, IDX_OPTIONS_FLAGS + 1, 4));
  nb_set(ctx, t, "streamStats",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, st->stream_stats,
                            IDX_STREAM_STATS_COUNT, 8));
  nb_set(ctx, t, "sessionStats",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, st->session_stats,
                            IDX_SESSION_STATS_COUNT, 8));
  nb_set_int(ctx, t, "kBitfield", kBitfield);
  nb_set_int(ctx, t, "kSessionPriorityListenerCount", kSessionPriorityListenerCount);
  nb_set_int(ctx, t, "kSessionFrameErrorListenerCount", kSessionFrameErrorListenerCount);
  nb_set_int(ctx, t, "kSessionMaxInvalidFrames", kSessionMaxInvalidFrames);
  nb_set_int(ctx, t, "kSessionMaxRejectedStreams", kSessionMaxRejectedStreams);
  nb_set_int(ctx, t, "kSessionUint8FieldCount", kSessionUint8FieldCount);
  nb_set_int(ctx, t, "kSessionHasRemoteSettingsListeners", kSessionHasRemoteSettingsListeners);
  nb_set_int(ctx, t, "kSessionRemoteSettingsIsUpToDate", kSessionRemoteSettingsIsUpToDate);
  nb_set_int(ctx, t, "kSessionHasPingListeners", kSessionHasPingListeners);
  nb_set_int(ctx, t, "kSessionHasAltsvcListeners", kSessionHasAltsvcListeners);
  nb_set_method(ctx, t, "nghttp2ErrorString", h2_error_string, 1);
  nb_set_method(ctx, t, "refreshDefaultSettings", h2_refresh_default_settings, 0);
  nb_set_method(ctx, t, "packSettings", h2_pack_settings, 0);
  nb_set_method(ctx, t, "setCallbackFunctions", h2_set_callback_functions, 11);

  ctor = nb_define_class(ctx, t, &tdef);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_stream_base_funcs, node_stream_base_funcs_count);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  node_register_stream_class(stream_class_id);
  JS_FreeValue(ctx, nb_define_class(ctx, t, &sdef));

  constants = JS_ParseJSON(ctx, node_http2_constants_json, strlen(node_http2_constants_json),
                           "http2_constants.json");
  {
    /* NODE_DEFINE_HIDDEN_CONSTANT: there, but not enumerable (so not in the JSON) */
    static const struct { const char *name; int value; } hidden[] = {
      { "NGHTTP2_HCAT_REQUEST", NGHTTP2_HCAT_REQUEST },
      { "NGHTTP2_HCAT_RESPONSE", NGHTTP2_HCAT_RESPONSE },
      { "NGHTTP2_HCAT_PUSH_RESPONSE", NGHTTP2_HCAT_PUSH_RESPONSE },
      { "NGHTTP2_HCAT_HEADERS", NGHTTP2_HCAT_HEADERS },
      { "NGHTTP2_NV_FLAG_NONE", NGHTTP2_NV_FLAG_NONE },
      { "NGHTTP2_NV_FLAG_NO_INDEX", NGHTTP2_NV_FLAG_NO_INDEX },
      { "NGHTTP2_ERR_DEFERRED", NGHTTP2_ERR_DEFERRED },
      { "NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE", NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE },
      { "NGHTTP2_ERR_INVALID_ARGUMENT", NGHTTP2_ERR_INVALID_ARGUMENT },
      { "NGHTTP2_ERR_STREAM_CLOSED", NGHTTP2_ERR_STREAM_CLOSED },
      { "NGHTTP2_ERR_NOMEM", NGHTTP2_ERR_NOMEM },
      { "STREAM_OPTION_EMPTY_PAYLOAD", STREAM_OPTION_EMPTY_PAYLOAD },
      { "STREAM_OPTION_GET_TRAILERS", STREAM_OPTION_GET_TRAILERS },
    };
    size_t i;
    for (i = 0; i < countof(hidden); i++)
      JS_DefinePropertyValueStr(ctx, constants, hidden[i].name, JS_NewInt32(ctx, hidden[i].value),
                                0);
  }
  nb_set(ctx, t, "constants", constants);
  names = JS_NewArray(ctx);
  {
    static const char *const err_names[] = {
      "NGHTTP2_NO_ERROR", "NGHTTP2_PROTOCOL_ERROR", "NGHTTP2_INTERNAL_ERROR",
      "NGHTTP2_FLOW_CONTROL_ERROR", "NGHTTP2_SETTINGS_TIMEOUT", "NGHTTP2_STREAM_CLOSED",
      "NGHTTP2_FRAME_SIZE_ERROR", "NGHTTP2_REFUSED_STREAM", "NGHTTP2_CANCEL",
      "NGHTTP2_COMPRESSION_ERROR", "NGHTTP2_CONNECT_ERROR", "NGHTTP2_ENHANCE_YOUR_CALM",
      "NGHTTP2_INADEQUATE_SECURITY", "NGHTTP2_HTTP_1_1_REQUIRED",
    };
    uint32_t i;
    for (i = 0; i < countof(err_names); i++)
      JS_SetPropertyUint32(ctx, names, i, JS_NewString(ctx, err_names[i]));
  }
  nb_set(ctx, t, "nameForErrorCode", names);
  return t;
}
