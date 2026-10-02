/* internalBinding('messaging') (node_messaging.cc): MessageChannel,
 * MessagePort, structuredClone; internalBinding('blob') (node_blob.cc);
 * the performance Histogram (histogram.cc); and the main-thread side of
 * internalBinding('worker'). */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "node.h"

/* ---------------------------------------------------------------------- */
/* serialization (structured clone) over QuickJS's object serializer */

typedef struct PortData PortData;

typedef struct Message {
  struct Message *next;
  uint8_t *data;
  size_t len;
  void **sab;          /* SharedArrayBuffer memory (a reference each) */
  int sab_count;
  PortData **ports;    /* MessagePorts transferred with it (a reference each) */
  int port_count;
  bool close_message;  /* the other side was closed */
} Message;

static void port_data_abandon(PortData *d);

/* what a message holds, less the struct */
static void message_clear(Message *m) {
  int i;
  for (i = 0; i < m->sab_count; i++)
    node_sab_free(NULL, m->sab[i]);
  for (i = 0; i < m->port_count; i++)
    if (m->ports[i])
      port_data_abandon(m->ports[i]);
  free(m->ports);
  free(m->data);
  free(m->sab);
  m->ports = NULL;
  m->data = NULL;
  m->sab = NULL;
  m->port_count = m->sab_count = 0;
}

static void message_free(Message *m) {
  message_clear(m);
  free(m);
}

static JSValue throw_data_clone_error(JSContext *ctx, const char *msg) {
  Env *env = env_get(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, env->global, "DOMException"), args[2], e;
  args[0] = JS_NewString(ctx, msg);
  args[1] = JS_NewString(ctx, "DataCloneError");
  e = JS_IsFunction(ctx, ctor) ? JS_CallConstructor(ctx, ctor, 2, (JSValueConst *)args)
                               : JS_NewTypeError(ctx, "%s", msg);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  if (JS_IsException(e))
    return e;
  return JS_Throw(ctx, e);
}

/* errors become plain records QuickJS can serialize, and back */
static JSValue clone_prepare(JSContext *ctx, JSValueConst v, int depth);

static JSValue serialize(JSContext *ctx, JSValueConst value, Message *msg) {
  size_t len;
  uint8_t *buf;
  JSSABTab tab;
  int i;
  memset(&tab, 0, sizeof(tab));
  buf = JS_WriteObject2(ctx, &len, value,
                        JS_WRITE_OBJ_REFERENCE | JS_WRITE_OBJ_SAB, &tab);
  if (!buf) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    char m[512];
    snprintf(m, sizeof(m), "%s could not be cloned.", s ? s : "value");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    return throw_data_clone_error(ctx, m);
  }
  msg->data = malloc(len);
  memcpy(msg->data, buf, len);
  msg->len = len;
  js_free(ctx, buf);
  /* the message keeps the shared memory alive until it is read (and the
     reader's buffers take references of their own) */
  msg->sab_count = tab.len;
  msg->sab = tab.len ? malloc(sizeof(void *) * tab.len) : NULL;
  for (i = 0; i < (int)tab.len; i++) {
    msg->sab[i] = tab.tab[i];
    node_sab_dup(NULL, tab.tab[i]);
  }
  js_free(ctx, tab.tab);
  return JS_UNDEFINED;
}

static JSValue deserialize(JSContext *ctx, Message *msg) {
  return JS_ReadObject2(ctx, msg->data, msg->len,
                        JS_READ_OBJ_REFERENCE | JS_READ_OBJ_SAB, NULL);
}

static bool is_cloneable_error(JSContext *ctx, JSValueConst v) {
  return (JS_NodeTypeFlags(v) & JS_NODE_TYPE_NATIVE_ERROR) != 0;
}

/* functions and symbols cannot be cloned; errors are rebuilt */
static JSValue clone_check(JSContext *ctx, JSValueConst v, int depth) {
  if (depth > 1000)
    return JS_UNDEFINED;
  if (JS_IsFunction(ctx, v)) {
    const char *s = "function";
    JSValue str = JS_ToString(ctx, v);
    const char *t = JS_ToCString(ctx, str);
    char m[300];
    snprintf(m, sizeof(m), "%.200s could not be cloned.", t ? t : s);
    JS_FreeCString(ctx, t);
    JS_FreeValue(ctx, str);
    return throw_data_clone_error(ctx, m);
  }
  if (JS_IsSymbol(v))
    return throw_data_clone_error(ctx, "Symbol() could not be cloned.");
  return JS_UNDEFINED;
}

static JSValue clone_prepare(JSContext *ctx, JSValueConst v, int depth) {
  return clone_check(ctx, v, depth);
}

static JSValue structured_clone_value(JSContext *ctx, JSValueConst value) {
  Message msg;
  JSValue r;
  memset(&msg, 0, sizeof(msg));
  r = clone_prepare(ctx, value, 0);
  if (JS_IsException(r))
    return r;
  if (is_cloneable_error(ctx, value)) {
    /* name, message, stack and cause, as V8 clones errors */
    JSValue ctorname = JS_GetPropertyStr(ctx, value, "name");
    JSValue message = JS_GetPropertyStr(ctx, value, "message");
    JSValue stack = JS_GetPropertyStr(ctx, value, "stack");
    const char *n = JS_ToCString(ctx, ctorname);
    JSValue ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global,
                                     n && (!strcmp(n, "EvalError") || !strcmp(n, "RangeError") ||
                                           !strcmp(n, "ReferenceError") || !strcmp(n, "SyntaxError") ||
                                           !strcmp(n, "TypeError") || !strcmp(n, "URIError"))
                                         ? n : "Error");
    JSValue e = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&message);
    JS_FreeCString(ctx, n);
    JS_FreeValue(ctx, ctor);
    if (!JS_IsException(e)) {
      JS_DefinePropertyValueStr(ctx, e, "stack", stack, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    } else {
      JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, ctorname);
    JS_FreeValue(ctx, message);
    return e;
  }
  r = serialize(ctx, value, &msg);
  if (JS_IsException(r))
    return r;
  r = deserialize(ctx, &msg);
  message_clear(&msg);
  return r;
}

/* structuredClone(value, { transfer }): the transferred ArrayBuffers are
   detached from the original */
static JSValue m_structured_clone(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  JSValue r = structured_clone_value(ctx, ARG(0)), transfer;
  uint32_t n = 0, i;
  if (JS_IsException(r) || !JS_IsObject(ARG(1)))
    return r;
  transfer = JS_GetPropertyStr(ctx, ARG(1), "transfer");
  if (JS_IsObject(transfer)) {
    JSValue len = JS_GetPropertyStr(ctx, transfer, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
      JSValue x = JS_GetPropertyUint32(ctx, transfer, i);
      if (JS_IsArrayBuffer(x))
        JS_DetachArrayBuffer(ctx, x);
      JS_FreeValue(ctx, x);
    }
  }
  JS_FreeValue(ctx, transfer);
  return r;
}

/* ---------------------------------------------------------------------- */
/* MessagePort */

/* One end of a channel.  Its queue, its sibling (the other end) and the
   references are shared between threads (a port can move to a worker), all
   under one lock. */
struct PortData {
  Message *head, *tail;
  PortData *sibling;          /* the entangled end */
  struct MessagePort *port;   /* the port object it belongs to, if any */
  int refs;
  bool closed;
};

typedef struct MessagePort {
  AsyncWrap aw;
  uv_async_t async;
  void *track;                /* in the environment's handle list */
  PortData *data;
  bool receiving;
  bool closing;
  bool refed;
  bool handle_open;
} MessagePort;

static JSClassID message_port_class_id;
static JSClassID message_channel_class_id;
static pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;

static PortData *port_data_new(void) {
  PortData *d = calloc(1, sizeof(*d));
  d->refs = 1;
  return d;
}

static void port_data_free(PortData *d) {
  Message *m = d->head;
  d->head = d->tail = NULL;
  while (m) {
    Message *n = m->next;
    message_free(m);  /* may abandon ports it carries: never this one */
    m = n;
  }
  free(d);
}

static void port_data_unref(PortData *d) {
  bool last;
  pthread_mutex_lock(&ports_lock);
  last = --d->refs == 0;
  if (last && d->sibling) {
    d->sibling->sibling = NULL;
    d->sibling = NULL;
  }
  pthread_mutex_unlock(&ports_lock);
  if (last)
    port_data_free(d);
}

static void port_enqueue_locked(PortData *d, Message *m) {
  MessagePort *p;
  if (d->tail)
    d->tail->next = m;
  else
    d->head = m;
  d->tail = m;
  p = d->port;
  if (p && p->handle_open)
    uv_async_send(&p->async);
}

static Message *port_dequeue(PortData *d) {
  Message *m;
  pthread_mutex_lock(&ports_lock);
  m = d->head;
  if (m) {
    d->head = m->next;
    if (!d->head)
      d->tail = NULL;
    m->next = NULL;
  }
  pthread_mutex_unlock(&ports_lock);
  return m;
}

static void entangle(PortData *a, PortData *b) {
  a->sibling = b;
  b->sibling = a;
}

/* this end is closed: the sibling hears so and they part */
static void port_data_disentangle(PortData *d) {
  PortData *sib;
  pthread_mutex_lock(&ports_lock);
  d->closed = true;
  sib = d->sibling;
  if (sib) {
    Message *m = calloc(1, sizeof(*m));
    m->close_message = true;
    port_enqueue_locked(sib, m);
    sib->sibling = NULL;
    d->sibling = NULL;
  }
  pthread_mutex_unlock(&ports_lock);
}

/* a transferred port nobody took (its message was dropped): it closes */
static void port_data_abandon(PortData *d) {
  port_data_disentangle(d);
  port_data_unref(d);
}

static void port_close_handle(MessagePort *p);
static JSValue message_take_ports(Env *env, Message *m, JSValue value, JSValueConst ports);

static void port_deliver(MessagePort *p) {
  Env *env = p->aw.env;
  JSContext *ctx = env->ctx;
  int processed = 0;
  JSValue emit = JS_GetPropertyStr(ctx, env->per_context_exports, "emitMessage");
  while (p->receiving && !p->closing && processed < 1000 && p->data) {
    Message *m = port_dequeue(p->data);
    JSValue args[3], payload, ports, r;
    if (!m)
      break;
    if (m->close_message) {
      message_free(m);
      port_close_handle(p);
      break;
    }
    payload = deserialize(ctx, m);
    ports = JS_NewArray(ctx);
    if (!JS_IsException(payload) && m->port_count) {
      JSValue t = message_take_ports(env, m, payload, ports);
      payload = t;
    }
    message_free(m);
    if (JS_IsException(payload)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      args[0] = JS_UNDEFINED;
      args[1] = ports;
      args[2] = JS_NewString(ctx, "messageerror");
    } else {
      args[0] = payload;
      args[1] = ports;
      args[2] = JS_NewString(ctx, "message");
    }
    r = node_make_callback(env, p->aw.object, p->aw.object, emit, 3, (JSValueConst *)args,
                           p->aw.async_id, p->aw.trigger_async_id, p->aw.context_frame);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    processed++;
  }
  JS_FreeValue(ctx, emit);
  if (processed >= 1000 && p->data && p->handle_open) {
    pthread_mutex_lock(&ports_lock);
    if (p->data->head)
      uv_async_send(&p->async);
    pthread_mutex_unlock(&ports_lock);
  }
}

static void port_async_cb(uv_async_t *h) {
  MessagePort *p = h->data;
  port_deliver(p);
}

static void port_handle_closed(uv_handle_t *h) {
  MessagePort *p = h->data;
  Env *env = p->aw.env;
  JSContext *ctx = env->ctx;
  node_handle_untrack(p->track);
  p->track = NULL;
  JSValue sym = env_get_symbol(env, "handle_onclose_symbol"), fn;
  JSAtom a = JS_ValueToAtom(ctx, sym);
  fn = JS_GetProperty(ctx, p->aw.object, a);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
  if (JS_IsFunction(ctx, fn)) {
    JSValue r = async_wrap_make_callback(&p->aw, fn, 0, NULL);
    JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, fn);
  async_wrap_unref(&p->aw);  /* the reference the open handle held */
}

/* the port object lets go of its data: to close it, or to send it */
static PortData *port_detach(MessagePort *p) {
  PortData *d = p->data;
  if (d) {
    pthread_mutex_lock(&ports_lock);
    if (d->port == p)
      d->port = NULL;
    pthread_mutex_unlock(&ports_lock);
    p->data = NULL;
  }
  return d;
}

static void port_close_handle(MessagePort *p) {
  PortData *d;
  if (p->closing)
    return;
  p->closing = true;
  d = port_detach(p);
  if (d) {
    port_data_disentangle(d);
    port_data_unref(d);
  }
  if (p->handle_open) {
    p->handle_open = false;
    uv_close((uv_handle_t *)&p->async, port_handle_closed);
  }
}

static void port_finalizer(JSRuntime *rt, JSValueConst val) {
  MessagePort *p = JS_GetOpaque(val, message_port_class_id);
  PortData *d;
  if (!p)
    return;
  d = port_detach(p);
  if (d)
    port_data_unref(d);
  node_handle_untrack(p->track);
  async_wrap_destroy(&p->aw);
  free(p);
}

/* the environment closes its ports when it ends */
static void port_close_tracked(AsyncWrap *aw) {
  port_close_handle((MessagePort *)aw);
}

/* a port object over data (which it takes) */
static JSValue port_new(Env *env, JSValueConst new_target, PortData *data) {
  JSContext *ctx = env->ctx;
  JSValue obj, oninit_sym, oninit;
  MessagePort *p;
  JSAtom a;
  if (JS_IsUndefined(new_target)) {
    JSValue proto = JS_GetClassProto(ctx, message_port_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, message_port_class_id);
    JS_FreeValue(ctx, proto);
  } else {
    obj = nb_new_instance(ctx, new_target, message_port_class_id);
  }
  if (JS_IsException(obj))
    return obj;
  p = calloc(1, sizeof(*p));
  p->data = data;
  async_wrap_init(&p->aw, env, obj, PROVIDER_MESSAGEPORT, -1);
  JS_SetOpaque(obj, p);
  uv_async_init(env->loop, &p->async, port_async_cb);
  p->async.data = p;
  p->handle_open = true;
  p->refed = true;
  p->track = node_handle_track_fn(&p->aw, (uv_handle_t *)&p->async, port_close_tracked);
  pthread_mutex_lock(&ports_lock);
  data->port = p;
  pthread_mutex_unlock(&ports_lock);
  async_wrap_ref(&p->aw);  /* open handles keep their object */
  /* not receiving until start() (or an 'message' listener) */
  uv_unref((uv_handle_t *)&p->async);
  oninit_sym = env_get_symbol(env, "oninit_symbol");
  a = JS_ValueToAtom(ctx, oninit_sym);
  oninit = JS_GetProperty(ctx, obj, a);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, oninit_sym);
  if (JS_IsFunction(ctx, oninit)) {
    JSValue r = JS_Call(ctx, oninit, obj, 0, NULL);
    if (JS_IsException(r)) {
      JS_FreeValue(ctx, oninit);
      JS_FreeValue(ctx, obj);
      return r;
    }
    JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, oninit);
  return obj;
}

static JSValue port_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                         JSValueConst *argv) {
  return node_throw_type_error(ctx, "ERR_CONSTRUCT_CALL_INVALID", "Constructor cannot be called");
}

static MessagePort *port_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque(v, message_port_class_id);
}

/* for worker.c: an entangled pair of port ends, and a port object over one */
void *node_port_pair_new(void **other) {
  PortData *a = port_data_new(), *b = port_data_new();
  entangle(a, b);
  *other = b;
  return a;
}

JSValue node_port_object(Env *env, void *data) {
  return port_new(env, JS_UNDEFINED, data);
}

/* an end no port object took */
void node_port_data_release(void *data) {
  port_data_abandon(data);
}

/* ---- transferring ports ----
 * The ports in a message's transfer list leave this side: the value is
 * written with a marker where each one was, the message carries their data,
 * and the receiving side makes new port objects over it. */

#define PORT_MARKER "\x01nodeTransferredPort"

static JSValue replace_ports(JSContext *ctx, JSValueConst v, JSValueConst ports, JSValueConst seen,
                             int depth) {
  JSValue copy, known, r;
  uint32_t n = 0, i;
  JSValue len;
  if (!JS_IsObject(v) || depth > 1000)
    return JS_DupValue(ctx, v);
  if (port_of(ctx, v)) {
    len = JS_GetPropertyStr(ctx, ports, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
      JSValue x = JS_GetPropertyUint32(ctx, ports, i);
      bool same = JS_IsStrictEqual(ctx, x, v);
      JS_FreeValue(ctx, x);
      if (same) {
        copy = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, copy, PORT_MARKER, JS_NewUint32(ctx, i));
        return copy;
      }
    }
    return throw_data_clone_error(ctx, "Object that needs transfer was found in message but not listed in transferList");
  }
  {
    JSValue has = JS_GetPropertyStr(ctx, seen, "get");
    known = JS_Call(ctx, has, seen, 1, &v);
    JS_FreeValue(ctx, has);
    if (!JS_IsUndefined(known))
      return known;
  }
  if (JS_IsArray(v)) {
    copy = JS_NewArray(ctx);
  } else if (JS_GetClassID(v) == 1 /* JS_CLASS_OBJECT */) {
    copy = JS_NewObject(ctx);
  } else {
    return JS_DupValue(ctx, v);  /* other objects go as they are */
  }
  {
    JSValue set = JS_GetPropertyStr(ctx, seen, "set"), args[2];
    args[0] = (JSValue)v;
    args[1] = copy;
    r = JS_Call(ctx, set, seen, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, set);
  }
  {
    JSPropertyEnum *props;
    uint32_t np, k;
    if (JS_GetOwnPropertyNames(ctx, &props, &np, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
      JS_FreeValue(ctx, copy);
      return JS_EXCEPTION;
    }
    for (k = 0; k < np; k++) {
      JSValue x = JS_GetProperty(ctx, v, props[k].atom), y;
      if (JS_IsException(x)) {
        JS_FreePropertyEnum(ctx, props, np);
        JS_FreeValue(ctx, copy);
        return x;
      }
      y = replace_ports(ctx, x, ports, seen, depth + 1);
      JS_FreeValue(ctx, x);
      if (JS_IsException(y)) {
        JS_FreePropertyEnum(ctx, props, np);
        JS_FreeValue(ctx, copy);
        return y;
      }
      JS_DefinePropertyValue(ctx, copy, props[k].atom, y, JS_PROP_C_W_E);
    }
    JS_FreePropertyEnum(ctx, props, np);
  }
  return copy;
}

/* the received value with port objects where the markers are */
static JSValue restore_ports(JSContext *ctx, JSValue v, JSValueConst objs, JSValueConst seen,
                             int depth) {
  JSValue mark;
  if (!JS_IsObject(v) || depth > 1000)
    return v;
  mark = JS_GetPropertyStr(ctx, v, PORT_MARKER);
  if (JS_IsNumber(mark)) {
    uint32_t i = 0;
    JS_ToUint32(ctx, &i, mark);
    JS_FreeValue(ctx, v);
    return JS_GetPropertyUint32(ctx, objs, i);
  }
  JS_FreeValue(ctx, mark);
  if (!JS_IsArray(v) && JS_GetClassID(v) != 1 /* JS_CLASS_OBJECT */)
    return v;
  {
    JSValue has = JS_GetPropertyStr(ctx, seen, "has"), add, r;
    bool visited;
    r = JS_Call(ctx, has, seen, 1, (JSValueConst *)&v);
    visited = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, has);
    if (visited)
      return v;
    add = JS_GetPropertyStr(ctx, seen, "add");
    r = JS_Call(ctx, add, seen, 1, (JSValueConst *)&v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, add);
  }
  {
    JSPropertyEnum *props;
    uint32_t np, k;
    if (!JS_GetOwnPropertyNames(ctx, &props, &np, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
      for (k = 0; k < np; k++) {
        JSValue x = JS_GetProperty(ctx, v, props[k].atom);
        if (JS_IsObject(x))
          JS_SetProperty(ctx, v, props[k].atom, restore_ports(ctx, x, objs, seen, depth + 1));
        else
          JS_FreeValue(ctx, x);
      }
      JS_FreePropertyEnum(ctx, props, np);
    }
  }
  return v;
}

static JSValue new_global(JSContext *ctx, const char *name) {
  JSValue ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global, name);
  JSValue o = JS_CallConstructor(ctx, ctor, 0, NULL);
  JS_FreeValue(ctx, ctor);
  return o;
}

/* the ports a message carries, as port objects of env (appended to `ports`),
   in place of their markers in value (which this takes) */
static JSValue message_take_ports(Env *env, Message *m, JSValue value, JSValueConst ports) {
  JSContext *ctx = env->ctx;
  JSValue seen;
  int i;
  for (i = 0; i < m->port_count; i++) {
    JSValue po = port_new(env, JS_UNDEFINED, m->ports[i]);
    if (JS_IsException(po)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      po = JS_UNDEFINED;
    } else {
      m->ports[i] = NULL;  /* the object has it now */
    }
    JS_SetPropertyUint32(ctx, ports, i, po);
  }
  seen = new_global(ctx, "Set");
  value = restore_ports(ctx, value, ports, seen, 0);
  JS_FreeValue(ctx, seen);
  return value;
}

/* postMessage(value, transferList | { transfer }) */
static JSValue port_post_message(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  Message *m;
  JSValue r, transfer = JS_UNDEFINED, value, xports = JS_UNDEFINED;
  PortData *sib = NULL;
  uint32_t n = 0, i, nports = 0;
  if (!p)
    return node_throw_type_error(ctx, "ERR_INVALID_THIS", "Value of \"this\" must be of type MessagePort");
  if (argc == 0)
    return node_throw_type_error(ctx, "ERR_MISSING_ARGS", "The \"value\" argument must be specified");
  if (argc > 1 && JS_IsObject(argv[1])) {
    transfer = JS_IsArray(argv[1]) ? JS_DupValue(ctx, argv[1])
                                   : JS_GetPropertyStr(ctx, argv[1], "transfer");
    if (!JS_IsObject(transfer)) {
      JS_FreeValue(ctx, transfer);
      transfer = JS_UNDEFINED;
    }
  }
  r = clone_prepare(ctx, ARG(0), 0);
  if (JS_IsException(r)) {
    JS_FreeValue(ctx, transfer);
    return r;
  }
  /* the MessagePorts to send */
  if (!JS_IsUndefined(transfer)) {
    JSValue len = JS_GetPropertyStr(ctx, transfer, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    xports = JS_NewArray(ctx);
    for (i = 0; i < n; i++) {
      JSValue x = JS_GetPropertyUint32(ctx, transfer, i);
      MessagePort *xp = port_of(ctx, x);
      if (xp) {
        if (xp == p) {
          JS_FreeValue(ctx, x);
          JS_FreeValue(ctx, xports);
          JS_FreeValue(ctx, transfer);
          return throw_data_clone_error(ctx, "Transfer list contains source port");
        }
        if (!xp->data || xp->closing) {
          JS_FreeValue(ctx, x);
          JS_FreeValue(ctx, xports);
          JS_FreeValue(ctx, transfer);
          return throw_data_clone_error(ctx, "MessagePort in transfer list is already detached");
        }
        JS_SetPropertyUint32(ctx, xports, nports++, x);
      } else {
        JS_FreeValue(ctx, x);
      }
    }
  }
  value = JS_DupValue(ctx, ARG(0));
  if (nports) {
    JSValue seen = new_global(ctx, "Map");
    JSValue v2 = replace_ports(ctx, value, xports, seen, 0);
    JS_FreeValue(ctx, seen);
    JS_FreeValue(ctx, value);
    if (JS_IsException(v2)) {
      JS_FreeValue(ctx, xports);
      JS_FreeValue(ctx, transfer);
      return v2;
    }
    value = v2;
  }
  m = calloc(1, sizeof(*m));
  r = serialize(ctx, value, m);
  JS_FreeValue(ctx, value);
  if (JS_IsException(r)) {
    free(m);
    JS_FreeValue(ctx, xports);
    JS_FreeValue(ctx, transfer);
    return r;
  }
  /* the transferred ports leave this side, and ArrayBuffers are detached */
  if (nports) {
    m->ports = calloc(nports, sizeof(PortData *));
    for (i = 0; i < nports; i++) {
      JSValue x = JS_GetPropertyUint32(ctx, xports, i);
      MessagePort *xp = port_of(ctx, x);
      m->ports[m->port_count++] = port_detach(xp);
      xp->closing = true;
      if (xp->handle_open) {
        xp->handle_open = false;
        uv_close((uv_handle_t *)&xp->async, port_handle_closed);
      }
      JS_FreeValue(ctx, x);
    }
  }
  for (i = 0; i < n; i++) {
    JSValue x = JS_GetPropertyUint32(ctx, transfer, i);
    if (JS_IsArrayBuffer(x))
      JS_DetachArrayBuffer(ctx, x);
    JS_FreeValue(ctx, x);
  }
  JS_FreeValue(ctx, xports);
  JS_FreeValue(ctx, transfer);
  if (p->data) {
    pthread_mutex_lock(&ports_lock);
    sib = p->data->sibling;
    if (sib)
      port_enqueue_locked(sib, m);
    pthread_mutex_unlock(&ports_lock);
  }
  if (!sib)
    message_free(m);
  return JS_TRUE;
}

static JSValue port_start(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  if (!p)
    return JS_UNDEFINED;
  p->receiving = true;
  if (p->handle_open) {
    if (p->refed)
      uv_ref((uv_handle_t *)&p->async);
    uv_async_send(&p->async);
  }
  return JS_UNDEFINED;
}

static JSValue port_close(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  if (p)
    port_close_handle(p);
  return JS_UNDEFINED;
}

static JSValue port_ref(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  if (p && p->handle_open) {
    p->refed = true;
    if (p->receiving)
      uv_ref((uv_handle_t *)&p->async);
  }
  return JS_UNDEFINED;
}

static JSValue port_unref(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  if (p && p->handle_open) {
    p->refed = false;
    uv_unref((uv_handle_t *)&p->async);
  }
  return JS_UNDEFINED;
}

static JSValue port_has_ref(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  if (!p)
    return node_throw_type_error(ctx, "ERR_INVALID_THIS", "not a MessagePort");
  return JS_NewBool(ctx, p->handle_open && uv_has_ref((uv_handle_t *)&p->async));
}

static JSValue port_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  MessagePort *p = port_of(ctx, this_val);
  return JS_NewFloat64(ctx, p ? p->aw.async_id : -1);
}

static const JSCFunctionListEntry port_proto[] = {
  JS_CFUNC_DEF("postMessage", 2, port_post_message),
  JS_CFUNC_DEF("start", 0, port_start),
  JS_CFUNC_DEF("close", 0, port_close),
  JS_CFUNC_DEF("ref", 0, port_ref),
  JS_CFUNC_DEF("unref", 0, port_unref),
  JS_CFUNC_DEF("hasRef", 0, port_has_ref),
  JS_CFUNC_DEF("getAsyncId", 0, port_get_async_id),
};

static JSValue channel_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, new_target, message_channel_class_id), p1, p2;
  PortData *a = port_data_new(), *b = port_data_new();
  if (JS_IsException(obj))
    return obj;
  entangle(a, b);
  p1 = port_new(env, JS_UNDEFINED, a);
  if (JS_IsException(p1)) {
    JS_FreeValue(ctx, obj);
    return p1;
  }
  p2 = port_new(env, JS_UNDEFINED, b);
  if (JS_IsException(p2)) {
    JS_FreeValue(ctx, p1);
    JS_FreeValue(ctx, obj);
    return p2;
  }
  JS_DefinePropertyValueStr(ctx, obj, "port1", p1, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(ctx, obj, "port2", p2, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
  return obj;
}

static JSValue m_stop_message_port(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  MessagePort *p = port_of(ctx, ARG(0));
  if (p) {
    p->receiving = false;
    if (p->handle_open)
      uv_unref((uv_handle_t *)&p->async);
  }
  return JS_UNDEFINED;
}

static JSValue m_check_message_port(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  return JS_NewBool(ctx, port_of(ctx, ARG(0)) != NULL);
}

static JSValue m_drain_message_port(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  MessagePort *p = port_of(ctx, ARG(0));
  if (p) {
    bool was = p->receiving;
    p->receiving = true;
    port_deliver(p);
    p->receiving = was;
  }
  return JS_UNDEFINED;
}

static JSValue m_receive_message_on_port(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  Env *env = env_get(ctx);
  MessagePort *p = port_of(ctx, ARG(0));
  Message *m;
  JSValue v;
  if (!p)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "The \"port\" argument must be a MessagePort instance");
  m = p->data ? port_dequeue(p->data) : NULL;
  if (!m || m->close_message) {
    if (m) {
      message_free(m);
      port_close_handle(p);
    }
    return env_get_symbol(env, "no_message_symbol");
  }
  v = deserialize(ctx, m);
  if (!JS_IsException(v) && m->port_count) {
    JSValue ports = JS_NewArray(ctx);
    v = message_take_ports(env, m, v, ports);
    JS_FreeValue(ctx, ports);
  }
  message_free(m);
  return v;
}

static JSValue m_move_message_port_to_context(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
  return JS_DupValue(ctx, ARG(0));
}

static JSValue m_broadcast_channel(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  PortData *d = port_data_new();
  return port_new(env, JS_UNDEFINED, d);
}

static JSValue m_set_deserializer_create_object(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->messaging_deserialize_create_object);
  env->messaging_deserialize_create_object = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue m_expose_lazy_dom_exception(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  /* exposeLazyDOMExceptionProperty(target): target.DOMException, lazily */
  Env *env = env_get(ctx);
  JSValue de = JS_GetPropertyStr(ctx, env->per_context_exports, "DOMException");
  if (JS_IsObject(ARG(0)))
    JS_DefinePropertyValueStr(ctx, ARG(0), "DOMException", de,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  else
    JS_FreeValue(ctx, de);
  return JS_UNDEFINED;
}

JSValue binding_init_messaging(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), port_ctor_v;
  NodeClassDef pdef = { .name = "MessagePort", .class_id = &message_port_class_id,
                        .ctor = port_ctor, .finalizer = port_finalizer,
                        .proto_funcs = port_proto, .proto_funcs_count = countof(port_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef cdef = { .name = "MessageChannel", .class_id = &message_channel_class_id,
                        .ctor = channel_ctor, .parent_ctor = JS_UNDEFINED };
  port_ctor_v = nb_define_class(ctx, t, &pdef);
  JS_FreeValue(ctx, port_ctor_v);
  JS_FreeValue(ctx, nb_define_class(ctx, t, &cdef));
  nb_set_method(ctx, t, "stopMessagePort", m_stop_message_port, 1);
  nb_set_method(ctx, t, "checkMessagePort", m_check_message_port, 1);
  nb_set_method(ctx, t, "drainMessagePort", m_drain_message_port, 1);
  nb_set_method(ctx, t, "receiveMessageOnPort", m_receive_message_on_port, 1);
  nb_set_method(ctx, t, "moveMessagePortToContext", m_move_message_port_to_context, 2);
  nb_set_method(ctx, t, "setDeserializerCreateObjectFunction", m_set_deserializer_create_object, 1);
  nb_set_method(ctx, t, "broadcastChannel", m_broadcast_channel, 1);
  nb_set_method(ctx, t, "structuredClone", m_structured_clone, 2);
  nb_set_method(ctx, t, "exposeLazyDOMExceptionProperty", m_expose_lazy_dom_exception, 1);
  nb_set(ctx, t, "DOMException", JS_GetPropertyStr(ctx, env->per_context_exports, "DOMException"));
  return t;
}

/* ---------------------------------------------------------------------- */
/* worker: the main thread's view (worker threads are in worker.c) */

JSValue node_worker_class(Env *env, JSValueConst target); /* worker.c */
JSValue node_worker_env_message_port(Env *env);
const char *node_worker_name(Env *env);

static JSValue w_get_env_message_port(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  return node_worker_env_message_port(env_get(ctx));
}

__attribute__((weak)) JSValue node_worker_env_message_port(Env *env) {
  return JS_UNDEFINED;
}

__attribute__((weak)) JSValue node_worker_class(Env *env, JSValueConst target) {
  return JS_UNDEFINED;
}

__attribute__((weak)) const char *node_worker_name(Env *env) {
  return "worker";
}

JSValue binding_init_worker(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_bool(ctx, t, "isMainThread", env->is_main_thread);
  nb_set_bool(ctx, t, "isInternalThread", false);
  nb_set_bool(ctx, t, "ownsProcessState", env->owns_process_state);
  nb_set_int(ctx, t, "threadId", env->thread_id);
  nb_set_str(ctx, t, "threadName", env->is_main_thread ? "main" : node_worker_name(env));
  nb_set(ctx, t, "resourceLimits",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64,
                            env_scratch(env, "resource_limits", 4 * sizeof(double)), 4, 8));
  nb_set_int(ctx, t, "kMaxYoungGenerationSizeMb", 0);
  nb_set_int(ctx, t, "kMaxOldGenerationSizeMb", 1);
  nb_set_int(ctx, t, "kCodeRangeSizeMb", 2);
  nb_set_int(ctx, t, "kStackSizeMb", 3);
  nb_set_int(ctx, t, "kTotalResourceLimitCount", 4);
  nb_set_method(ctx, t, "getEnvMessagePort", w_get_env_message_port, 0);
  node_worker_class(env, t);
  return t;
}

/* ---------------------------------------------------------------------- */
/* blob */

typedef struct BlobData {
  int refs;
  uint8_t *bytes;
  size_t len;
} BlobData;

typedef struct {
  BlobData *data;
  size_t offset, len;
} BlobHandle;

typedef struct {
  BlobData *data;
  size_t offset, len;
  bool eos;
} BlobReader;

static JSClassID blob_class_id, blob_reader_class_id;

static void blob_data_unref(BlobData *d) {
  if (d && --d->refs == 0) {
    free(d->bytes);
    free(d);
  }
}

static void blob_finalizer(JSRuntime *rt, JSValueConst val) {
  BlobHandle *h = JS_GetOpaque(val, blob_class_id);
  if (h) {
    blob_data_unref(h->data);
    free(h);
  }
}

static void blob_reader_finalizer(JSRuntime *rt, JSValueConst val) {
  BlobReader *r = JS_GetOpaque(val, blob_reader_class_id);
  if (r) {
    blob_data_unref(r->data);
    free(r);
  }
}

static JSValue blob_wrap(JSContext *ctx, BlobData *d, size_t off, size_t len) {
  JSValue proto = JS_GetClassProto(ctx, blob_class_id);
  JSValue obj = JS_NewObjectProtoClass(ctx, proto, blob_class_id);
  BlobHandle *h = calloc(1, sizeof(*h));
  JS_FreeValue(ctx, proto);
  h->data = d;
  d->refs++;
  h->offset = off;
  h->len = len;
  JS_SetOpaque(obj, h);
  return obj;
}

static JSValue blob_get_reader(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  BlobHandle *h = JS_GetOpaque2(ctx, this_val, blob_class_id);
  JSValue proto, obj;
  BlobReader *r;
  if (!h)
    return JS_EXCEPTION;
  proto = JS_GetClassProto(ctx, blob_reader_class_id);
  obj = JS_NewObjectProtoClass(ctx, proto, blob_reader_class_id);
  JS_FreeValue(ctx, proto);
  r = calloc(1, sizeof(*r));
  r->data = h->data;
  h->data->refs++;
  r->offset = h->offset;
  r->len = h->len;
  JS_SetOpaque(obj, r);
  return obj;
}

static JSValue blob_slice(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  BlobHandle *h = JS_GetOpaque2(ctx, this_val, blob_class_id);
  double start = nb_double(ctx, ARG(0), 0), end = nb_double(ctx, ARG(1), 0);
  size_t s, e;
  if (!h)
    return JS_EXCEPTION;
  if (start < 0) start = 0;
  if (end < start) end = start;
  s = (size_t)start;
  e = (size_t)end;
  if (s > h->len) s = h->len;
  if (e > h->len) e = h->len;
  return blob_wrap(ctx, h->data, h->offset + s, e - s);
}

static JSValue blob_reader_pull(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  BlobReader *r = JS_GetOpaque2(ctx, this_val, blob_reader_class_id);
  JSValue args[2], ret;
  if (!r)
    return JS_EXCEPTION;
  if (r->eos || r->len == 0) {
    r->eos = true;
    args[0] = JS_NewInt32(ctx, 0);
    ret = JS_Call(ctx, ARG(0), JS_UNDEFINED, 1, (JSValueConst *)args);
    if (JS_IsException(ret))
      return ret;
    JS_FreeValue(ctx, ret);
    return JS_NewInt32(ctx, 0);
  }
  args[0] = JS_NewInt32(ctx, 1);
  args[1] = JS_NewArrayBufferCopy(ctx, r->data->bytes + r->offset, r->len);
  r->eos = true;
  ret = JS_Call(ctx, ARG(0), JS_UNDEFINED, 2, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
  if (JS_IsException(ret))
    return ret;
  JS_FreeValue(ctx, ret);
  return JS_NewInt32(ctx, 1);
}

static JSValue blob_reader_set_wakeup(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue blob_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                         JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* createBlob(sources, length): sources are views and blob handles */
static JSValue b_create_blob(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  JSValue lenv;
  uint32_t n = 0, i;
  size_t total = 0, off = 0;
  BlobData *d;
  JSValue r;
  if (!JS_IsArray(ARG(0)))
    return JS_ThrowTypeError(ctx, "sources must be an array");
  lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue s = JS_GetPropertyUint32(ctx, ARG(0), i);
    size_t l = 0;
    BlobHandle *h = JS_GetOpaque(s, blob_class_id);
    if (h)
      total += h->len;
    else if (nb_buffer_data(ctx, s, &l))
      total += l;
    JS_FreeValue(ctx, s);
  }
  d = calloc(1, sizeof(*d));
  d->bytes = malloc(total ? total : 1);
  d->len = total;
  for (i = 0; i < n; i++) {
    JSValue s = JS_GetPropertyUint32(ctx, ARG(0), i);
    size_t l = 0;
    uint8_t *p;
    BlobHandle *h = JS_GetOpaque(s, blob_class_id);
    if (h) {
      memcpy(d->bytes + off, h->data->bytes + h->offset, h->len);
      off += h->len;
    } else if ((p = nb_buffer_data(ctx, s, &l))) {
      memcpy(d->bytes + off, p, l);
      off += l;
    }
    JS_FreeValue(ctx, s);
  }
  r = blob_wrap(ctx, d, 0, total);
  return r;
}

static JSValue b_concat(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
  JSValue lenv;
  uint32_t n = 0, i;
  size_t total = 0, off = 0;
  uint8_t *buf;
  lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue s = JS_GetPropertyUint32(ctx, ARG(0), i);
    size_t l = 0;
    if (nb_buffer_data(ctx, s, &l))
      total += l;
    JS_FreeValue(ctx, s);
  }
  buf = malloc(total ? total : 1);
  for (i = 0; i < n; i++) {
    JSValue s = JS_GetPropertyUint32(ctx, ARG(0), i);
    size_t l = 0;
    uint8_t *p = nb_buffer_data(ctx, s, &l);
    if (p) {
      memcpy(buf + off, p, l);
      off += l;
    }
    JS_FreeValue(ctx, s);
  }
  {
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, total);
    free(buf);
    return ab;
  }
}

static JSValue b_create_blob_from_file_path(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  char *path = node_string_to_utf8(ctx, ARG(0), NULL);
  FILE *f;
  BlobData *d;
  long n;
  JSValue arr;
  if (!path)
    return JS_EXCEPTION;
  f = fopen(path, "rb");
  free(path);
  if (!f)
    return JS_UNDEFINED;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  d = calloc(1, sizeof(*d));
  d->bytes = malloc(n ? n : 1);
  d->len = fread(d->bytes, 1, n, f);
  fclose(f);
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, blob_wrap(ctx, d, 0, d->len));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, (double)d->len));
  return arr;
}

static JSValue b_store_data_object(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue store = env_binding_data(env, "blob"), rec = JS_NewArray(ctx);
  JSAtom a = JS_ValueToAtom(ctx, ARG(0));
  JS_SetPropertyUint32(ctx, rec, 0, JS_DupValue(ctx, ARG(1)));
  JS_SetPropertyUint32(ctx, rec, 1, JS_DupValue(ctx, ARG(2)));
  JS_SetPropertyUint32(ctx, rec, 2, JS_DupValue(ctx, ARG(3)));
  JS_SetProperty(ctx, store, a, rec);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, store);
  return JS_UNDEFINED;
}

static JSValue b_get_data_object(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue store = env_binding_data(env, "blob"), r;
  JSAtom a = JS_ValueToAtom(ctx, ARG(0));
  r = JS_GetProperty(ctx, store, a);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, store);
  return r;
}

static JSValue b_revoke_object_url(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue store = env_binding_data(env, "blob");
  const char *url = JS_ToCString(ctx, ARG(0));
  if (url && !strncmp(url, "blob:nodedata:", 14)) {
    JSAtom a = JS_NewAtom(ctx, url + 14);
    JS_DeleteProperty(ctx, store, a, 0);
    JS_FreeAtom(ctx, a);
  }
  JS_FreeCString(ctx, url);
  JS_FreeValue(ctx, store);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry blob_proto[] = {
  JS_CFUNC_DEF("getReader", 0, blob_get_reader),
  JS_CFUNC_DEF("slice", 2, blob_slice),
};
static const JSCFunctionListEntry blob_reader_proto[] = {
  JS_CFUNC_DEF("pull", 1, blob_reader_pull),
  JS_CFUNC_DEF("setWakeup", 1, blob_reader_set_wakeup),
};

JSValue binding_init_blob(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef bdef = { .name = "BlobHandle", .class_id = &blob_class_id, .ctor = blob_ctor,
                        .finalizer = blob_finalizer, .proto_funcs = blob_proto,
                        .proto_funcs_count = countof(blob_proto), .parent_ctor = JS_UNDEFINED };
  NodeClassDef rdef = { .name = "BlobReader", .class_id = &blob_reader_class_id,
                        .ctor = blob_ctor, .finalizer = blob_reader_finalizer,
                        .proto_funcs = blob_reader_proto,
                        .proto_funcs_count = countof(blob_reader_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, JS_UNDEFINED, &bdef));
  JS_FreeValue(ctx, nb_define_class(ctx, JS_UNDEFINED, &rdef));
  nb_set_method(ctx, t, "createBlob", b_create_blob, 2);
  nb_set_method(ctx, t, "storeDataObject", b_store_data_object, 4);
  nb_set_method(ctx, t, "getDataObject", b_get_data_object, 1);
  nb_set_method(ctx, t, "revokeObjectURL", b_revoke_object_url, 1);
  nb_set_method(ctx, t, "concat", b_concat, 1);
  nb_set_method(ctx, t, "createBlobFromFilePath", b_create_blob_from_file_path, 1);
  return t;
}

/* ---------------------------------------------------------------------- */
/* Histogram (perf_hooks) */

typedef struct {
  int64_t *samples;
  size_t count, cap;
  int64_t min, max;
  double sum, sumsq;
  int64_t exceeds;
  uint64_t prev_delta;
  uv_timer_t timer;   /* event loop delay sampling */
  bool timer_on, timer_init;
  int64_t interval;
  uint64_t last_ns;
} Histogram;

static JSClassID histogram_class_id;

static void hist_reset(Histogram *h) {
  h->count = 0;
  h->min = INT64_MAX;
  h->max = 0;
  h->sum = h->sumsq = 0;
  h->exceeds = 0;
  h->prev_delta = 0;
}

static void hist_record(Histogram *h, int64_t v) {
  if (v < 1) {
    h->exceeds++;
    return;
  }
  if (h->count == h->cap) {
    if (h->cap >= 1 << 20) {
      /* keep a bounded number of samples: drop every other one */
      size_t i, j = 0;
      for (i = 0; i < h->count; i += 2)
        h->samples[j++] = h->samples[i];
      h->count = j;
    } else {
      h->cap = h->cap ? h->cap * 2 : 256;
      h->samples = realloc(h->samples, h->cap * sizeof(int64_t));
    }
  }
  h->samples[h->count++] = v;
  if (v < h->min) h->min = v;
  if (v > h->max) h->max = v;
  h->sum += (double)v;
  h->sumsq += (double)v * (double)v;
}

static int cmp_i64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

static int64_t hist_percentile(Histogram *h, double p) {
  int64_t *s;
  size_t idx;
  int64_t r;
  if (h->count == 0)
    return 0;
  s = malloc(h->count * sizeof(int64_t));
  memcpy(s, h->samples, h->count * sizeof(int64_t));
  qsort(s, h->count, sizeof(int64_t), cmp_i64);
  idx = (size_t)ceil(p / 100.0 * h->count);
  if (idx > 0) idx--;
  if (idx >= h->count) idx = h->count - 1;
  r = s[idx];
  free(s);
  return r;
}

static void hist_finalizer(JSRuntime *rt, JSValueConst val) {
  Histogram *h = JS_GetOpaque(val, histogram_class_id);
  if (h) {
    if (h->timer_init) {
      free(h->samples);
      node_close_and_free((uv_handle_t *)&h->timer, h);
      return;
    }
    free(h->samples);
    free(h);
  }
}

static Histogram *hist_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, histogram_class_id);
}

#define HIST_NUM(name, expr)                                                   \
  static JSValue name(JSContext *ctx, JSValueConst this_val, int argc,         \
                      JSValueConst *argv, int magic) {                          \
    Histogram *h = hist_of(ctx, this_val);                                     \
    double v;                                                                  \
    if (!h) return JS_EXCEPTION;                                               \
    v = (expr);                                                                \
    return magic ? JS_NewBigInt64(ctx, (int64_t)v) : JS_NewFloat64(ctx, v);    \
  }

HIST_NUM(h_count, (double)h->count)
HIST_NUM(h_min, h->count ? (double)h->min : 9223372036854775807.0)
HIST_NUM(h_max, (double)h->max)
HIST_NUM(h_exceeds, (double)h->exceeds)

static JSValue h_mean(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return JS_NewFloat64(ctx, h->count ? h->sum / h->count : NAN);
}

static JSValue h_stddev(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  double mean, var;
  if (!h) return JS_EXCEPTION;
  if (!h->count)
    return JS_NewFloat64(ctx, NAN);
  mean = h->sum / h->count;
  var = h->sumsq / h->count - mean * mean;
  return JS_NewFloat64(ctx, var > 0 ? sqrt(var) : 0);
}

static JSValue h_percentile(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic) {
  Histogram *h = hist_of(ctx, this_val);
  int64_t v;
  if (!h) return JS_EXCEPTION;
  v = hist_percentile(h, nb_double(ctx, ARG(0), 50));
  return magic ? JS_NewBigInt64(ctx, v) : JS_NewFloat64(ctx, (double)v);
}

static JSValue h_percentiles(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic) {
  Histogram *h = hist_of(ctx, this_val);
  static const double ps[] = { 0, 50, 75, 87.5, 93.75, 96.875, 98.4375, 99.21875, 100 };
  JSValue set;
  size_t i;
  if (!h) return JS_EXCEPTION;
  set = JS_GetPropertyStr(ctx, ARG(0), "set");
  for (i = 0; i < countof(ps); i++) {
    int64_t v = hist_percentile(h, ps[i]);
    JSValue args[2], r;
    if (h->count == 0 && i > 0)
      break;
    args[0] = JS_NewFloat64(ctx, ps[i]);
    args[1] = magic ? JS_NewBigInt64(ctx, v) : JS_NewFloat64(ctx, (double)v);
    r = JS_Call(ctx, set, ARG(0), 2, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[1]);
  }
  JS_FreeValue(ctx, set);
  return JS_UNDEFINED;
}

static JSValue h_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  if (h) hist_reset(h);
  return JS_UNDEFINED;
}

static JSValue h_record(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  int64_t v = 0;
  if (!h) return JS_EXCEPTION;
  nb_to_int64(ctx, &v, ARG(0));
  hist_record(h, v);
  return JS_UNDEFINED;
}

static JSValue h_record_delta(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  uint64_t now = uv_hrtime();
  if (!h) return JS_EXCEPTION;
  if (h->prev_delta)
    hist_record(h, (int64_t)(now - h->prev_delta));
  h->prev_delta = now;
  return JS_UNDEFINED;
}

static JSValue h_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val), *o = JS_GetOpaque(ARG(0), histogram_class_id);
  size_t i;
  if (!h || !o) return JS_EXCEPTION;
  for (i = 0; i < o->count; i++)
    hist_record(h, o->samples[i]);
  h->exceeds += o->exceeds;
  return JS_NewFloat64(ctx, 0);
}

static void eld_timer_cb(uv_timer_t *t) {
  Histogram *h = t->data;
  uint64_t now = uv_hrtime();
  if (!h)
    return;
  if (h->last_ns) {
    int64_t delay = (int64_t)(now - h->last_ns) - h->interval * 1000000;
    if (delay > 0)
      hist_record(h, delay);
  }
  h->last_ns = now;
}

static JSValue h_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  Env *env = env_get(ctx);
  if (!h) return JS_EXCEPTION;
  if (!h->timer_init) {
    uv_timer_init(env->loop, &h->timer);
    h->timer.data = h;
    h->timer_init = true;
  }
  if (!h->timer_on) {
    h->last_ns = 0;
    uv_timer_start(&h->timer, eld_timer_cb, h->interval, h->interval);
    uv_unref((uv_handle_t *)&h->timer);
    h->timer_on = true;
  }
  return JS_TRUE;
}

static JSValue h_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Histogram *h = hist_of(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (h->timer_on) {
    uv_timer_stop(&h->timer);
    h->timer_on = false;
    return JS_TRUE;
  }
  return JS_FALSE;
}

static const JSCFunctionListEntry histogram_proto[] = {
  JS_CFUNC_MAGIC_DEF("count", 0, h_count, 0),
  JS_CFUNC_MAGIC_DEF("countBigInt", 0, h_count, 1),
  JS_CFUNC_MAGIC_DEF("exceeds", 0, h_exceeds, 0),
  JS_CFUNC_MAGIC_DEF("exceedsBigInt", 0, h_exceeds, 1),
  JS_CFUNC_MAGIC_DEF("min", 0, h_min, 0),
  JS_CFUNC_MAGIC_DEF("minBigInt", 0, h_min, 1),
  JS_CFUNC_MAGIC_DEF("max", 0, h_max, 0),
  JS_CFUNC_MAGIC_DEF("maxBigInt", 0, h_max, 1),
  JS_CFUNC_DEF("mean", 0, h_mean),
  JS_CFUNC_DEF("stddev", 0, h_stddev),
  JS_CFUNC_MAGIC_DEF("percentile", 1, h_percentile, 0),
  JS_CFUNC_MAGIC_DEF("percentileBigInt", 1, h_percentile, 1),
  JS_CFUNC_MAGIC_DEF("percentiles", 1, h_percentiles, 0),
  JS_CFUNC_MAGIC_DEF("percentilesBigInt", 1, h_percentiles, 1),
  JS_CFUNC_DEF("reset", 0, h_reset),
  JS_CFUNC_DEF("record", 1, h_record),
  JS_CFUNC_DEF("recordDelta", 0, h_record_delta),
  JS_CFUNC_DEF("add", 1, h_add),
  JS_CFUNC_DEF("start", 1, h_start),
  JS_CFUNC_DEF("stop", 0, h_stop),
};

static JSValue histogram_new(JSContext *ctx, JSValueConst new_target, int64_t interval) {
  JSValue obj;
  Histogram *h;
  if (JS_IsUndefined(new_target)) {
    JSValue proto = JS_GetClassProto(ctx, histogram_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, histogram_class_id);
    JS_FreeValue(ctx, proto);
  } else {
    obj = nb_new_instance(ctx, new_target, histogram_class_id);
  }
  if (JS_IsException(obj))
    return obj;
  h = calloc(1, sizeof(*h));
  hist_reset(h);
  h->interval = interval > 0 ? interval : 10;
  JS_SetOpaque(obj, h);
  return obj;
}

static JSValue histogram_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                              JSValueConst *argv) {
  return histogram_new(ctx, new_target, 10);
}

JSValue node_create_eld_histogram(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  return histogram_new(ctx, JS_UNDEFINED, nb_int64(ctx, ARG(0), 10));
}

JSValue node_histogram_class(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef def = { .name = "Histogram", .class_id = &histogram_class_id,
                       .ctor = histogram_ctor, .finalizer = hist_finalizer,
                       .proto_funcs = histogram_proto,
                       .proto_funcs_count = countof(histogram_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, target, &def));
  return JS_UNDEFINED;
}

/* ---------------------------------------------------------------------- */
/* cjs_lexer: Node 22's cjs-module-lexer (JS), as the binding v24 expects */

static JSValue cjs_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue arg = JS_NewString(ctx, "internal/deps/cjs-module-lexer/lexer"), mod, parse, res, out;
  JSValue set_ctor, set, exports, reexports, src;
  mod = JS_Call(ctx, env->builtin_require, JS_UNDEFINED, 1, (JSValueConst *)&arg);
  JS_FreeValue(ctx, arg);
  if (JS_IsException(mod))
    return mod;
  parse = JS_GetPropertyStr(ctx, mod, "parse");
  JS_FreeValue(ctx, mod);
  src = JS_IsString(ARG(0)) ? JS_DupValue(ctx, ARG(0)) : JS_NewString(ctx, "");
  res = JS_Call(ctx, parse, JS_UNDEFINED, 1, (JSValueConst *)&src);
  JS_FreeValue(ctx, parse);
  JS_FreeValue(ctx, src);
  set_ctor = JS_GetPropertyStr(ctx, env->global, "Set");
  if (JS_IsException(res)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    exports = JS_NewArray(ctx);
    reexports = JS_NewArray(ctx);
  } else {
    exports = JS_GetPropertyStr(ctx, res, "exports");
    reexports = JS_GetPropertyStr(ctx, res, "reexports");
    JS_FreeValue(ctx, res);
  }
  set = JS_CallConstructor(ctx, set_ctor, 1, (JSValueConst *)&exports);
  JS_FreeValue(ctx, set_ctor);
  JS_FreeValue(ctx, exports);
  out = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, out, 0, set);
  JS_SetPropertyUint32(ctx, out, 1, reexports);
  return out;
}

JSValue binding_init_cjs_lexer(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "parse", cjs_parse, 1);
  return t;
}

/* url_pattern: URLPattern is not available yet */
static JSValue url_pattern_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                JSValueConst *argv) {
  return node_throw_error(ctx, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM",
                          "URLPattern is not available in this build");
}

JSValue binding_init_url_pattern(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  JSValue c = JS_NewCFunction2(ctx, url_pattern_ctor, "URLPattern", 2, JS_CFUNC_constructor, 0);
  JSValue proto = JS_NewObject(ctx);
  JS_SetConstructor(ctx, c, proto);
  JS_FreeValue(ctx, proto);
  nb_set(ctx, t, "URLPattern", c);
  return t;
}
