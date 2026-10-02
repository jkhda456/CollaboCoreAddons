/* The WebAssembly JS API (V8's built-in `WebAssembly` namespace) on top of
 * the wasm3 interpreter: there is no JIT in the guest.
 *
 * Each realm has one wasm3 runtime (a "store") that every Module, Instance,
 * Memory, Table and Global of that realm lives in.  Linking an import is then
 * what wasm3's own module linking does - a slot pointed at another module's
 * memory, table, global or function - and only JS functions go through a host
 * thunk.  wasm3 cannot unload one module of a runtime, so instances stay until
 * the realm's last wasm object is gone; the store is reference counted by every
 * wrapper object and every ArrayBuffer over a linear memory. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"

#include "wasm3.h"
#include "m3_env.h"
#include "m3_compile.h"
#include "m3_exec_defs.h"
#include "m3_function.h"
#include "m3_validate.h"
#undef not
#undef and
#undef or

#define WASM_STACK_SIZE (1024 * 1024)
#define MAX_PAGES32 65536

enum { ERR_COMPILE, ERR_LINK, ERR_RUNTIME };

/* a host function threw: the JS exception stays pending while the trap
   unwinds the Wasm frames */
static const char kJsException[] = "[trap] JS exception";
static const char kDeadInstance[] = "[trap] host function of a collected instance";

static JSClassID store_class_id, module_class_id, instance_class_id, memory_class_id,
    table_class_id, global_class_id, wfunc_class_id;

typedef struct Blob {
  int refs;
  size_t len;
  uint8_t data[];
} Blob;

typedef struct HostFn HostFn;
typedef struct WInstance WInstance;

typedef struct Store {
  int refs;
  JSRuntime *jsrt;
  JSContext *ctx;
  IM3Environment env;
  IM3Runtime rt;
  JSAtom funcref_atom;   /* private: a wasm function's WFunc */
  JSValue err_ctors[3];  /* the holder's (marked and freed by it) */
  JSValue *externs;      /* externref handles, the holder's */
  uint32_t nexterns, cexterns;
  Blob **blobs;          /* bytes the loaded modules point into */
  size_t nblobs, cblobs;
  HostFn **hostfns;      /* userdata wasm3 code points at */
  size_t nhostfns, chostfns;
  WInstance *instances;
} Store;

struct HostFn {
  JSValue fn;     /* borrowed from the instance's imports */
  IM3Function m3fn;
  WInstance *inst;
  bool dead;
};

typedef struct WModule {
  Store *store;
  Blob *blob;
  IM3Module parsed; /* never loaded: imports/exports reflection */
} WModule;

struct WInstance {
  Store *store;
  IM3Module mod;
  JSValue obj;        /* weak */
  JSValue module_obj;
  JSValue exports;
  JSValue *keep;      /* import values the instance uses */
  uint32_t nkeep, ckeep;
  WInstance *next;
};

typedef struct WMemory {
  Store *store;
  IM3Memory mem;
  JSValue obj;    /* weak */
  JSValue buffer; /* cached ArrayBuffer over the current block */
} WMemory;

typedef struct WTable {
  Store *store;
  IM3Table tab;
  JSValue obj; /* weak */
} WTable;

typedef struct WGlobal {
  Store *store;
  IM3Global glob;
  JSValue obj; /* weak */
} WGlobal;

typedef struct WFunc {
  Store *store;
  IM3Function fn;
  JSValue func; /* weak: the exported function object */
} WFunc;

/* ---------------------------------------------------------------------- */
/* store */

static void blob_unref(Blob *b) {
  if (b && --b->refs == 0)
    free(b);
}

static void *grow_array(void *p, size_t *cap, size_t need, size_t elem) {
  if (need <= *cap)
    return p;
  *cap = *cap ? *cap * 2 : 8;
  while (*cap < need)
    *cap *= 2;
  return realloc(p, *cap * elem);
}

static void store_unref(Store *s) {
  size_t i;
  if (!s || --s->refs > 0)
    return;
  if (s->rt)
    m3_FreeRuntime(s->rt);
  if (s->env)
    m3_FreeEnvironment(s->env);
  for (i = 0; i < s->nblobs; i++)
    blob_unref(s->blobs[i]);
  free(s->blobs);
  for (i = 0; i < s->nhostfns; i++)
    free(s->hostfns[i]);
  free(s->hostfns);
  free(s->externs);
  if (s->funcref_atom != JS_ATOM_NULL)
    JS_FreeAtomRT(s->jsrt, s->funcref_atom);
  free(s);
}

static Store *store_ref(Store *s) {
  s->refs++;
  return s;
}

static int store_ensure(Store *s, JSContext *ctx) {
  if (s->rt)
    return 0;
  s->env = m3_NewEnvironment();
  if (s->env)
    s->rt = m3_NewRuntime(s->env, WASM_STACK_SIZE, s);
  if (!s->rt) {
    JS_ThrowOutOfMemory(ctx);
    return -1;
  }
  return 0;
}

static int store_keep_blob(Store *s, Blob *b) {
  s->blobs = grow_array(s->blobs, &s->cblobs, s->nblobs + 1, sizeof(Blob *));
  s->blobs[s->nblobs++] = b;
  b->refs++;
  return 0;
}

static void holder_finalizer(JSRuntime *rt, JSValueConst val) {
  Store *s = JS_GetOpaque(val, store_class_id);
  uint32_t i;
  if (!s)
    return;
  for (i = 0; i < 3; i++) {
    JS_FreeValueRT(rt, s->err_ctors[i]);
    s->err_ctors[i] = JS_UNDEFINED;
  }
  for (i = 0; i < s->nexterns; i++)
    JS_FreeValueRT(rt, s->externs[i]);
  s->nexterns = 0;
  store_unref(s);
}

static void holder_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  Store *s = JS_GetOpaque(val, store_class_id);
  uint32_t i;
  if (!s)
    return;
  for (i = 0; i < 3; i++)
    JS_MarkValue(rt, s->err_ctors[i], mark_func);
  for (i = 0; i < s->nexterns; i++)
    JS_MarkValue(rt, s->externs[i], mark_func);
}

static Store *store_of_data(JSValueConst *data) {
  return JS_GetOpaque(data[0], store_class_id);
}

/* ---------------------------------------------------------------------- */
/* errors */

static JSValue throw_wasm(JSContext *ctx, Store *s, int kind, const char *fmt, ...) {
  char msg[600];
  va_list ap;
  JSValue m, e;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (!s || !JS_IsFunction(ctx, s->err_ctors[kind]))
    return JS_ThrowPlainError(ctx, "%s", msg);
  m = JS_NewString(ctx, msg);
  e = JS_CallConstructor(ctx, s->err_ctors[kind], 1, (JSValueConst *)&m);
  JS_FreeValue(ctx, m);
  if (JS_IsException(e))
    return e;
  return JS_Throw(ctx, e);
}

static bool is_trap(M3Result r) {
  return r && strncmp(r, "[trap]", 6) == 0;
}

static const char *trap_message(M3Result r) {
  if (r == m3Err_trapUnreachable)
    return "unreachable";
  if (r == m3Err_trapDivisionByZero)
    return "divide by zero";
  if (r == m3Err_trapIntegerOverflow)
    return "divide result unrepresentable";
  if (r == m3Err_trapIntegerConversion)
    return "float unrepresentable in integer range";
  if (r == m3Err_trapOutOfBoundsMemoryAccess)
    return "memory access out of bounds";
  if (r == m3Err_trapIndirectCallTypeMismatch || r == m3Err_trapTableElementIsNull)
    return "null function or function signature mismatch";
  if (r == m3Err_trapTableIndexOutOfRange || r == m3Err_trapTableOutOfBounds)
    return "table index is out of bounds";
  if (r == m3Err_trapNullReference || r == m3Err_trapNullFunctionRef)
    return "dereferencing a null pointer";
  if (r == m3Err_trapUnalignedAtomic)
    return "operation does not support unaligned accesses";
  if (r == m3Err_trapUncaughtException)
    return "uncaught wasm exception";
  if (is_trap(r))
    return r[6] == ' ' ? r + 7 : r + 6;
  return r;
}

/* a failed m3_Call / m3_RunStart: the pending JS exception, or the trap */
static JSValue throw_call_result(JSContext *ctx, Store *s, M3Result r) {
  if (r == kJsException) {
    if (JS_HasException(ctx))
      return JS_EXCEPTION;
    return throw_wasm(ctx, s, ERR_RUNTIME, "exception in a host function");
  }
  if (r == m3Err_trapStackOverflow)
    return JS_ThrowRangeError(ctx, "Maximum call stack size exceeded");
  return throw_wasm(ctx, s, ERR_RUNTIME, "%s", trap_message(r));
}

/* ---------------------------------------------------------------------- */
/* buffer sources */

/* the bytes of an ArrayBuffer or ArrayBuffer view, copied into a blob */
static Blob *blob_from(JSContext *ctx, JSValueConst v, const char *who) {
  size_t len = 0, off = 0, bpe;
  uint8_t *p = NULL;
  Blob *b;
  if (JS_IsObject(v)) {
    if (JS_IsArrayBuffer(v)) {
      p = JS_GetArrayBuffer(ctx, &len, v);
    } else {
      JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
      if (!JS_IsException(ab)) {
        size_t ablen;
        uint8_t *d = JS_GetArrayBuffer(ctx, &ablen, ab);
        if (d)
          p = d + off;
        else if (ablen == 0)
          p = (uint8_t *)"";
        JS_FreeValue(ctx, ab);
      } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        /* a DataView */
        JSValue buf = JS_GetPropertyStr(ctx, v, "buffer");
        if (JS_IsArrayBuffer(buf)) {
          JSValue o = JS_GetPropertyStr(ctx, v, "byteOffset");
          JSValue l = JS_GetPropertyStr(ctx, v, "byteLength");
          double dof = 0, dl = 0;
          size_t ablen;
          uint8_t *d = JS_GetArrayBuffer(ctx, &ablen, buf);
          JS_ToFloat64(ctx, &dof, o);
          JS_ToFloat64(ctx, &dl, l);
          JS_FreeValue(ctx, o);
          JS_FreeValue(ctx, l);
          if (d && dof + dl <= ablen) {
            p = d + (size_t)dof;
            len = (size_t)dl;
          }
        }
        JS_FreeValue(ctx, buf);
      }
    }
  }
  if (!p && len == 0 && JS_IsObject(v) && JS_IsArrayBuffer(v))
    p = (uint8_t *)""; /* empty or detached */
  if (!p) {
    JS_ThrowTypeError(ctx, "%s: Argument 0 must be a buffer source", who);
    return NULL;
  }
  b = malloc(sizeof(Blob) + (len ? len : 1));
  if (!b) {
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  b->refs = 1;
  b->len = len;
  memcpy(b->data, p, len);
  return b;
}

/* ---------------------------------------------------------------------- */
/* modules: parse, validate, reflect */

static const char kHiddenName[] = "\xff<wasm>"; /* never an import's module name */

/* parse and type-check every body now, as V8 does at compile time (wasm3
   validates lazily on first call otherwise) */
static M3Result parse_and_validate(Store *s, Blob *b, IM3Module *out) {
  IM3Module m = NULL;
  M3Result r = m3_ParseModule(s->env, &m, b->data, (uint32_t)b->len);
  uint32_t i;
  if (r) {
    if (m)
      m3_FreeModule(m);
    return r;
  }
  m->runtime = s->rt; /* the validator keeps its scratch there */
  for (i = 0; i < m->numFunctions; i++) {
    IM3Function f = &m->functions[i];
    if (f->wasm) {
      r = ValidateFunction(f);
      if (r)
        break;
    }
  }
  m->runtime = NULL;
  if (r) {
    m3_FreeModule(m);
    return r;
  }
  *out = m;
  return NULL;
}

static void module_finalizer(JSRuntime *rt, JSValueConst val) {
  WModule *wm = JS_GetOpaque(val, module_class_id);
  if (!wm)
    return;
  if (wm->parsed)
    m3_FreeModule(wm->parsed);
  blob_unref(wm->blob);
  store_unref(wm->store);
  free(wm);
}

static JSValue proto_from_ctor(JSContext *ctx, JSValueConst new_target, JSClassID id) {
  JSValue proto;
  if (JS_IsFunction(ctx, new_target)) {
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
      return proto;
    if (JS_IsObject(proto))
      return proto;
    JS_FreeValue(ctx, proto);
  }
  return JS_GetClassProto(ctx, id);
}

static JSValue new_wrapper(JSContext *ctx, JSValueConst new_target, JSClassID id) {
  JSValue proto = proto_from_ctor(ctx, new_target, id), obj;
  if (JS_IsException(proto))
    return proto;
  obj = JS_NewObjectProtoClass(ctx, proto, id);
  JS_FreeValue(ctx, proto);
  return obj;
}

static JSValue module_new(JSContext *ctx, Store *s, JSValueConst new_target, Blob *b,
                          const char *who) {
  IM3Module parsed = NULL;
  M3Result r;
  WModule *wm;
  JSValue obj;
  if (store_ensure(s, ctx) < 0)
    return JS_EXCEPTION;
  r = parse_and_validate(s, b, &parsed);
  if (r)
    return throw_wasm(ctx, s, ERR_COMPILE, "%s: %s", who, r);
  obj = new_wrapper(ctx, new_target, module_class_id);
  if (JS_IsException(obj)) {
    m3_FreeModule(parsed);
    return obj;
  }
  wm = calloc(1, sizeof(*wm));
  wm->store = store_ref(s);
  wm->blob = b;
  b->refs++;
  wm->parsed = parsed;
  JS_SetOpaque(obj, wm);
  return obj;
}

static bool require_new(JSContext *ctx, JSValueConst this_val, const char *who) {
  if (!JS_IsFunction(ctx, this_val)) {
    JS_ThrowTypeError(ctx, "%s(): %s must be invoked with 'new'", who, who);
    return false;
  }
  return true;
}

static JSValue js_module_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic, JSValueConst *data) {
  Store *s = store_of_data(data);
  Blob *b;
  JSValue r;
  if (!require_new(ctx, this_val, "WebAssembly.Module"))
    return JS_EXCEPTION;
  b = blob_from(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WebAssembly.Module()");
  if (!b)
    return JS_EXCEPTION;
  r = module_new(ctx, s, this_val, b, "WebAssembly.Module()");
  blob_unref(b);
  return r;
}

static const char *kind_name(int kind) {
  switch (kind) {
  case d_externalKind_function: return "function";
  case d_externalKind_table: return "table";
  case d_externalKind_memory: return "memory";
  case d_externalKind_global: return "global";
  default: return "tag";
  }
}

static WModule *module_arg(JSContext *ctx, JSValueConst v, const char *who) {
  WModule *wm = JS_GetOpaque(v, module_class_id);
  if (!wm)
    JS_ThrowTypeError(ctx, "%s: Argument 0 must be a WebAssembly.Module", who);
  return wm;
}

static JSValue descr(JSContext *ctx, const char *module, const char *name, size_t name_len,
                     int kind) {
  JSValue o = JS_NewObject(ctx);
  if (module)
    JS_SetPropertyStr(ctx, o, "module", JS_NewString(ctx, module));
  JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, name, name_len));
  JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, kind_name(kind)));
  return o;
}

static JSValue js_module_imports(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic, JSValueConst *data) {
  WModule *wm = module_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WebAssembly.Module.imports()");
  IM3Module m;
  JSValue arr;
  uint32_t i, n = 0;
  if (!wm)
    return JS_EXCEPTION;
  m = wm->parsed;
  arr = JS_NewArray(ctx);
  for (i = 0; i < m->numFunctions; i++) {
    M3ImportInfo *im = &m->functions[i].import;
    if (im->moduleUtf8 && im->fieldUtf8)
      JS_SetPropertyUint32(ctx, arr, n++, descr(ctx, im->moduleUtf8, im->fieldUtf8,
                                                 strlen(im->fieldUtf8), d_externalKind_function));
  }
  for (i = 0; i < m->numTables; i++)
    if (m->tables[i]->imported)
      JS_SetPropertyUint32(ctx, arr, n++,
                           descr(ctx, m->tables[i]->import.moduleUtf8, m->tables[i]->import.fieldUtf8,
                                 strlen(m->tables[i]->import.fieldUtf8), d_externalKind_table));
  for (i = 0; i < m->numMemories; i++)
    if (m->memories[i]->imported)
      JS_SetPropertyUint32(ctx, arr, n++,
                           descr(ctx, m->memories[i]->import.moduleUtf8,
                                 m->memories[i]->import.fieldUtf8,
                                 strlen(m->memories[i]->import.fieldUtf8), d_externalKind_memory));
  for (i = 0; i < m->numGlobals; i++)
    if (m->globals[i].imported && m->globals[i].import.moduleUtf8)
      JS_SetPropertyUint32(ctx, arr, n++,
                           descr(ctx, m->globals[i].import.moduleUtf8, m->globals[i].import.fieldUtf8,
                                 strlen(m->globals[i].import.fieldUtf8), d_externalKind_global));
#if d_m3HasExceptionHandling || d_m3HasStackSwitching
  for (i = 0; i < m->numTags; i++)
    if (m->tags[i].imported && m->tags[i].import.moduleUtf8)
      JS_SetPropertyUint32(ctx, arr, n++,
                           descr(ctx, m->tags[i].import.moduleUtf8, m->tags[i].import.fieldUtf8,
                                 strlen(m->tags[i].import.fieldUtf8), d_externalKind_tag));
#endif
  return arr;
}

static JSValue js_module_exports(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic, JSValueConst *data) {
  WModule *wm = module_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WebAssembly.Module.exports()");
  JSValue arr;
  uint32_t i;
  if (!wm)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  for (i = 0; i < wm->parsed->numExports; i++) {
    M3Export *e = &wm->parsed->exports[i];
    JS_SetPropertyUint32(ctx, arr, i, descr(ctx, NULL, e->name, e->nameLength, e->kind));
  }
  return arr;
}

static int read_leb(const uint8_t **p, const uint8_t *end, uint32_t *out) {
  uint32_t v = 0;
  int shift = 0;
  while (*p < end && shift < 35) {
    uint8_t c = *(*p)++;
    v |= (uint32_t)(c & 0x7f) << shift;
    shift += 7;
    if (!(c & 0x80)) {
      *out = v;
      return 0;
    }
  }
  return -1;
}

static JSValue js_module_custom_sections(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv, int magic, JSValueConst *data) {
  WModule *wm = module_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                           "WebAssembly.Module.customSections()");
  const uint8_t *p, *end;
  const char *want;
  size_t want_len;
  JSValue arr;
  uint32_t n = 0;
  if (!wm)
    return JS_EXCEPTION;
  if (argc < 2 || JS_IsUndefined(argv[1]))
    return JS_ThrowTypeError(ctx, "WebAssembly.Module.customSections(): Argument 1 is required");
  want = JS_ToCStringLen(ctx, &want_len, argv[1]);
  if (!want)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  p = wm->blob->data + 8;
  end = wm->blob->data + wm->blob->len;
  while (p < end) {
    uint8_t id = *p++;
    uint32_t size, nlen;
    const uint8_t *sect;
    if (read_leb(&p, end, &size) < 0 || size > (size_t)(end - p))
      break;
    sect = p;
    p += size;
    if (id != 0)
      continue;
    if (read_leb(&sect, p, &nlen) < 0 || nlen > (size_t)(p - sect))
      continue;
    if (nlen == want_len && memcmp(sect, want, nlen) == 0) {
      sect += nlen;
      JS_SetPropertyUint32(ctx, arr, n++, JS_NewArrayBufferCopy(ctx, sect, p - sect));
    }
  }
  JS_FreeCString(ctx, want);
  return arr;
}

/* ---------------------------------------------------------------------- */
/* synthetic one-entity modules: what new Memory/Table/Global create */

static void put_leb(uint8_t **p, uint64_t v) {
  do {
    uint8_t c = v & 0x7f;
    v >>= 7;
    if (v)
      c |= 0x80;
    *(*p)++ = c;
  } while (v);
}

/* a module with one section `sect` defining entity 0 of `kind`, exported */
static IM3Module load_synthetic(JSContext *ctx, Store *s, uint8_t sect_id, const uint8_t *body,
                                size_t body_len, int kind) {
  uint8_t tmp[128], *p = tmp;
  static const uint8_t header[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
  Blob *b;
  IM3Module m = NULL;
  M3Result r;
  if (store_ensure(s, ctx) < 0)
    return NULL;
  memcpy(p, header, 8);
  p += 8;
  *p++ = sect_id;
  put_leb(&p, body_len);
  memcpy(p, body, body_len);
  p += body_len;
  *p++ = 7; /* export section */
  *p++ = 5;
  *p++ = 1;
  *p++ = 1;
  *p++ = 'x';
  *p++ = (uint8_t)kind;
  *p++ = 0;
  b = malloc(sizeof(Blob) + (p - tmp));
  b->refs = 1;
  b->len = p - tmp;
  memcpy(b->data, tmp, b->len);
  r = m3_ParseModule(s->env, &m, b->data, (uint32_t)b->len);
  if (r) {
    blob_unref(b);
    JS_ThrowRangeError(ctx, "%s", r);
    return NULL;
  }
  store_keep_blob(s, b);
  blob_unref(b);
  m3_SetModuleName(m, kHiddenName);
  r = m3_LoadModule(s->rt, m);
  if (r) {
    JS_ThrowRangeError(ctx, "%s", r);
    return NULL;
  }
  return m;
}

/* ---------------------------------------------------------------------- */
/* Memory */

static void *memory_ab_release(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
  if (size == 0)
    store_unref(opaque);
  return NULL; /* a linear memory's buffer is never resized from JS */
}

static void detach_buffer(WMemory *wm) {
  if (JS_IsUndefined(wm->buffer))
    return;
  JS_DetachArrayBuffer(wm->store->ctx, wm->buffer);
  JS_FreeValueRT(wm->store->jsrt, wm->buffer);
  wm->buffer = JS_UNDEFINED;
}

/* memory.grow, from Wasm or from JS: the block moved */
static void on_memory_resize(IM3Memory mem) {
  WMemory *wm = mem->hostData;
  if (wm && !wm->mem->isShared)
    detach_buffer(wm);
}

static void memory_finalizer(JSRuntime *rt, JSValueConst val) {
  WMemory *wm = JS_GetOpaque(val, memory_class_id);
  if (!wm)
    return;
  if (wm->mem->hostData == wm)
    wm->mem->hostData = NULL;
  JS_FreeValueRT(rt, wm->buffer);
  store_unref(wm->store);
  free(wm);
}

static void memory_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  WMemory *wm = JS_GetOpaque(val, memory_class_id);
  if (wm)
    JS_MarkValue(rt, wm->buffer, mark_func);
}

static JSValue memory_value(JSContext *ctx, Store *s, IM3Memory mem, JSValueConst new_target) {
  WMemory *wm = mem->hostData;
  JSValue obj;
  if (wm)
    return JS_DupValue(ctx, wm->obj);
  obj = new_wrapper(ctx, new_target, memory_class_id);
  if (JS_IsException(obj))
    return obj;
  wm = calloc(1, sizeof(*wm));
  wm->store = store_ref(s);
  wm->mem = mem;
  wm->obj = obj;
  wm->buffer = JS_UNDEFINED;
  mem->hostData = wm;
  JS_SetOpaque(obj, wm);
  return obj;
}

/* a descriptor's integer property, or -1 when absent */
static int desc_index(JSContext *ctx, JSValueConst desc, const char *name, double max,
                      const char *who, double *out) {
  JSValue v = JS_GetPropertyStr(ctx, desc, name);
  double d;
  if (JS_IsException(v))
    return -2;
  if (JS_IsUndefined(v))
    return -1;
  if (JS_ToFloat64(ctx, &d, v) < 0) {
    JS_FreeValue(ctx, v);
    return -2;
  }
  JS_FreeValue(ctx, v);
  if (d != d || d < 0 || d == 1.0 / 0.0) {
    JS_ThrowTypeError(ctx, "%s: Property '%s' must be convertible to a valid number", who, name);
    return -2;
  }
  d = (double)(int64_t)d;
  if (d > max) {
    JS_ThrowRangeError(ctx, "%s: Property '%s': value %.0f is above the upper bound %.0f", who,
                       name, d, max);
    return -2;
  }
  *out = d;
  return 0;
}

static JSValue js_memory_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic, JSValueConst *data) {
  static const char who[] = "WebAssembly.Memory()";
  Store *s = store_of_data(data);
  JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
  double initial = 0, maximum = 0;
  int has_initial, has_max;
  bool shared;
  JSValue v;
  uint8_t body[32], *p = body;
  IM3Module m;
  if (!require_new(ctx, this_val, "WebAssembly.Memory"))
    return JS_EXCEPTION;
  if (!JS_IsObject(desc))
    return JS_ThrowTypeError(ctx, "%s: Argument 0 must be a memory descriptor", who);
  has_initial = desc_index(ctx, desc, "initial", MAX_PAGES32, who, &initial);
  if (has_initial == -2)
    return JS_EXCEPTION;
  if (has_initial == -1) {
    has_initial = desc_index(ctx, desc, "minimum", MAX_PAGES32, who, &initial);
    if (has_initial == -2)
      return JS_EXCEPTION;
    if (has_initial == -1)
      return JS_ThrowTypeError(ctx, "%s: Property 'initial' is required", who);
  }
  has_max = desc_index(ctx, desc, "maximum", MAX_PAGES32, who, &maximum);
  if (has_max == -2)
    return JS_EXCEPTION;
  v = JS_GetPropertyStr(ctx, desc, "shared");
  shared = JS_ToBool(ctx, v);
  JS_FreeValue(ctx, v);
  if (has_max == 0 && maximum < initial)
    return JS_ThrowRangeError(ctx, "%s: Property 'maximum': value %.0f is below the lower bound %.0f",
                              who, maximum, initial);
  if (shared && has_max != 0)
    return JS_ThrowTypeError(ctx, "%s: If shared is true, maximum property should be defined.", who);
  *p++ = 1; /* one memory */
  *p++ = shared ? 3 : has_max == 0 ? 1 : 0;
  put_leb(&p, (uint64_t)initial);
  if (has_max == 0)
    put_leb(&p, (uint64_t)maximum);
  m = load_synthetic(ctx, s, 5, body, p - body, d_externalKind_memory);
  if (!m)
    return JS_EXCEPTION;
  return memory_value(ctx, s, m->memories[0], this_val);
}

static WMemory *memory_this(JSContext *ctx, JSValueConst this_val) {
  return JS_GetOpaque2(ctx, this_val, memory_class_id);
}

static JSValue js_memory_buffer(JSContext *ctx, JSValueConst this_val) {
  WMemory *wm = memory_this(ctx, this_val);
  M3MemoryHeader *h;
  if (!wm)
    return JS_EXCEPTION;
  if (!JS_IsUndefined(wm->buffer))
    return JS_DupValue(ctx, wm->buffer);
  h = wm->mem->mallocated;
  wm->buffer = JS_NewArrayBuffer(ctx, h ? m3MemData(h) : NULL, h ? h->length : 0, 0,
                                 memory_ab_release, store_ref(wm->store), wm->mem->isShared);
  if (JS_IsException(wm->buffer)) {
    store_unref(wm->store);
    wm->buffer = JS_UNDEFINED;
    return JS_EXCEPTION;
  }
  return JS_DupValue(ctx, wm->buffer);
}

static JSValue js_memory_grow(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  WMemory *wm = memory_this(ctx, this_val);
  IM3Memory mem;
  double delta;
  uint64_t old, max;
  M3Result r;
  if (!wm)
    return JS_EXCEPTION;
  mem = wm->mem;
  if (JS_ToFloat64(ctx, &delta, argc > 0 ? argv[0] : JS_UNDEFINED) < 0)
    return JS_EXCEPTION;
  if (delta != delta || delta < 0 || delta > 4294967295.0)
    return JS_ThrowTypeError(ctx, "WebAssembly.Memory.grow(): Argument 0 must be convertible to a valid number");
  delta = (double)(uint64_t)delta;
  old = mem->numPages;
  max = mem->hasMax ? mem->maxPages : MAX_PAGES32;
  if (max > MAX_PAGES32 && !mem->isMemory64)
    max = MAX_PAGES32;
  if (old + (uint64_t)delta > max)
    return JS_ThrowRangeError(ctx, "WebAssembly.Memory.grow(): Maximum memory size exceeded");
  r = ResizeMemory(wm->store->rt, mem, old + (uint64_t)delta);
  if (r)
    return JS_ThrowRangeError(ctx, "WebAssembly.Memory.grow(): Unable to grow instance memory");
  detach_buffer(wm); /* even for a zero delta */
  return JS_NewFloat64(ctx, (double)old);
}

/* ---------------------------------------------------------------------- */
/* values */

static JSValue function_value(JSContext *ctx, Store *s, IM3Function fn);

static JSValue unbox_extern(JSContext *ctx, Store *s, const void *ref) {
  uintptr_t h = (uintptr_t)ref;
  if (h == 0)
    return JS_NULL;
  if (h - 1 < s->nexterns)
    return JS_DupValue(ctx, s->externs[h - 1]);
  return JS_UNDEFINED;
}

static void *box_extern(Store *s, JSContext *ctx, JSValueConst v) {
  size_t cap = s->cexterns;
  uint32_t i;
  if (JS_IsNull(v))
    return NULL;
  for (i = 0; i < s->nexterns; i++)
    if (JS_IsStrictEqual(ctx, s->externs[i], v))
      return (void *)(uintptr_t)(i + 1);
  s->externs = grow_array(s->externs, &cap, s->nexterns + 1, sizeof(JSValue));
  s->cexterns = (uint32_t)cap;
  s->externs[s->nexterns++] = JS_DupValue(ctx, v);
  return (void *)(uintptr_t)s->nexterns;
}

static WFunc *wfunc_of(Store *s, JSContext *ctx, JSValueConst v) {
  JSValue r;
  WFunc *wf;
  if (!JS_IsFunction(ctx, v) || s->funcref_atom == JS_ATOM_NULL)
    return NULL;
  r = JS_GetProperty(ctx, v, s->funcref_atom);
  wf = JS_GetOpaque(r, wfunc_class_id);
  JS_FreeValue(ctx, r);
  return wf;
}

/* JS -> Wasm (ToWebAssemblyValue); the value goes into a 64-bit slot */
static int to_wasm(JSContext *ctx, Store *s, m3type_t type, JSValueConst v, uint64_t *out) {
  *out = 0;
  switch (BaseTypeOf(type)) {
  case c_m3Type_i32: {
    int32_t x;
    if (JS_ToInt32(ctx, &x, v) < 0)
      return -1;
    *(int32_t *)out = x;
    return 0;
  }
  case c_m3Type_i64: {
    int64_t x;
    if (JS_ToBigInt64(ctx, &x, v) < 0)
      return -1;
    *(int64_t *)out = x;
    return 0;
  }
  case c_m3Type_f32: {
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0)
      return -1;
    *(float *)out = (float)d;
    return 0;
  }
  case c_m3Type_f64: {
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0)
      return -1;
    *(double *)out = d;
    return 0;
  }
  case c_m3Type_funcref: {
    WFunc *wf;
    if (JS_IsNull(v))
      return 0;
    wf = wfunc_of(s, ctx, v);
    if (!wf) {
      JS_ThrowTypeError(ctx, "type incompatibility when transforming from/to JS");
      return -1;
    }
    *(uintptr_t *)out = (uintptr_t)Function_Implementation(wf->fn);
    return 0;
  }
  case c_m3Type_externref:
    *(uintptr_t *)out = (uintptr_t)box_extern(s, ctx, v);
    return 0;
  default:
    JS_ThrowTypeError(ctx, "type incompatibility when transforming from/to JS");
    return -1;
  }
}

/* Wasm -> JS (ToJSValue) */
static JSValue to_js(JSContext *ctx, Store *s, m3type_t type, const uint64_t *in) {
  switch (BaseTypeOf(type)) {
  case c_m3Type_i32: return JS_NewInt32(ctx, *(const int32_t *)in);
  case c_m3Type_i64: return JS_NewBigInt64(ctx, *(const int64_t *)in);
  case c_m3Type_f32: return JS_NewFloat64(ctx, *(const float *)in);
  case c_m3Type_f64: return JS_NewFloat64(ctx, *(const double *)in);
  case c_m3Type_funcref: {
    IM3Function f = *(IM3Function const *)in;
    return f ? function_value(ctx, s, f) : JS_NULL;
  }
  case c_m3Type_externref: return unbox_extern(ctx, s, *(void *const *)in);
  default:
    return JS_ThrowTypeError(ctx, "type incompatibility when transforming from/to JS");
  }
}

/* ---------------------------------------------------------------------- */
/* exported functions */

static void wfunc_finalizer(JSRuntime *rt, JSValueConst val) {
  WFunc *wf = JS_GetOpaque(val, wfunc_class_id);
  if (!wf)
    return;
  if (wf->fn->hostData == wf)
    wf->fn->hostData = NULL;
  store_unref(wf->store);
  free(wf);
}

static JSValue call_export(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic, JSValueConst *data) {
  WFunc *wf = JS_GetOpaque(data[1], wfunc_class_id);
  Store *s;
  IM3Function f;
  IM3FuncType t;
  uint64_t *vals, rets[8], *retv = rets;
  const void **ptrs, *rptr[8], **rptrs = rptr;
  uint32_t i, nargs, nrets;
  M3Result r;
  JSValue result;
  if (!wf)
    return JS_ThrowTypeError(ctx, "not a WebAssembly function");
  s = wf->store;
  f = Function_Implementation(wf->fn);
  t = f->funcType;
  nargs = t->numArgs;
  nrets = t->numRets;
  if (!f->compiled) {
    r = CompileFunction(f);
    if (r)
      return throw_wasm(ctx, s, is_trap(r) ? ERR_RUNTIME : ERR_LINK, "%s", trap_message(r));
  }
  vals = alloca((nargs ? nargs : 1) * sizeof(uint64_t));
  ptrs = alloca((nargs ? nargs : 1) * sizeof(void *));
  for (i = 0; i < nargs; i++) {
    if (to_wasm(ctx, s, t->types[nrets + i], i < (uint32_t)argc ? argv[i] : JS_UNDEFINED,
                &vals[i]) < 0)
      return JS_EXCEPTION;
    ptrs[i] = &vals[i];
  }
  if (nrets > 8) {
    retv = alloca(nrets * sizeof(uint64_t));
    rptrs = alloca(nrets * sizeof(void *));
  }
  for (i = 0; i < nrets; i++) {
    retv[i] = 0;
    rptrs[i] = &retv[i];
  }
  r = m3_Call(f, nargs, ptrs);
  if (r)
    return throw_call_result(ctx, s, r);
  if (nrets == 0)
    return JS_UNDEFINED;
  r = m3_GetResults(f, nrets, rptrs);
  if (r)
    return throw_wasm(ctx, s, ERR_RUNTIME, "%s", r);
  if (nrets == 1)
    return to_js(ctx, s, t->types[0], &retv[0]);
  result = JS_NewArray(ctx);
  for (i = 0; i < nrets; i++) {
    JSValue v = to_js(ctx, s, t->types[i], &retv[i]);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, result);
      return v;
    }
    JS_SetPropertyUint32(ctx, result, i, v);
  }
  return result;
}

static WInstance *instance_of_module(Store *s, IM3Module m) {
  WInstance *wi;
  for (wi = s->instances; wi; wi = wi->next)
    if (wi->mod == m)
      return wi;
  return NULL;
}

/* the one JS function object for a Wasm function of this store */
static JSValue function_value(JSContext *ctx, Store *s, IM3Function fn) {
  WFunc *wf = fn->hostData;
  WInstance *wi;
  JSValue holder, data[2], func;
  char name[16];
  if (wf)
    return JS_DupValue(ctx, wf->func);
  holder = JS_NewObjectClass(ctx, wfunc_class_id);
  if (JS_IsException(holder))
    return holder;
  wf = calloc(1, sizeof(*wf));
  wf->store = store_ref(s);
  wf->fn = fn;
  JS_SetOpaque(holder, wf);
  wi = instance_of_module(s, fn->module);
  data[0] = wi ? JS_DupValue(ctx, wi->obj) : JS_UNDEFINED; /* keeps its imports alive */
  data[1] = holder;
  snprintf(name, sizeof(name), "%u", (unsigned)(fn - fn->module->functions));
  func = JS_NewCFunctionData2(ctx, call_export, name, fn->funcType->numArgs, 0, 2,
                              (JSValueConst *)data);
  JS_FreeValue(ctx, data[0]);
  if (JS_IsException(func)) {
    JS_FreeValue(ctx, holder);
    return func;
  }
  JS_DefinePropertyValue(ctx, func, s->funcref_atom, holder, 0);
  wf->func = func;
  fn->hostData = wf;
  return func;
}

/* ---------------------------------------------------------------------- */
/* host functions: JS imports called from Wasm */

static const void *host_thunk(IM3Runtime runtime, IM3ImportContext ictx, uint64_t *sp, void *mem) {
  HostFn *h = ictx->userdata;
  Store *s = m3_GetUserData(runtime);
  JSContext *ctx = s->ctx;
  IM3FuncType t = ictx->function->funcType;
  uint32_t nrets = t->numRets, nargs = t->numArgs, i;
  JSValue *args, r;
  if (h->dead)
    return kDeadInstance;
  args = alloca((nargs ? nargs : 1) * sizeof(JSValue));
  for (i = 0; i < nargs; i++) {
    args[i] = to_js(ctx, s, t->types[nrets + i], &sp[nrets + i]);
    if (JS_IsException(args[i])) {
      while (i--)
        JS_FreeValue(ctx, args[i]);
      return kJsException;
    }
  }
  r = JS_Call(ctx, h->fn, JS_UNDEFINED, nargs, (JSValueConst *)args);
  for (i = 0; i < nargs; i++)
    JS_FreeValue(ctx, args[i]);
  if (JS_IsException(r))
    return kJsException;
  if (nrets == 1) {
    if (to_wasm(ctx, s, t->types[0], r, &sp[0]) < 0) {
      JS_FreeValue(ctx, r);
      return kJsException;
    }
  } else if (nrets > 1) {
    /* an iterable of nrets values */
    JSValue arr = JS_UNDEFINED;
    if (JS_IsObject(r)) {
      JSValue global = JS_GetGlobalObject(ctx), from, array_ctor;
      array_ctor = JS_GetPropertyStr(ctx, global, "Array");
      JS_FreeValue(ctx, global);
      from = JS_GetPropertyStr(ctx, array_ctor, "from");
      arr = JS_Call(ctx, from, array_ctor, 1, (JSValueConst *)&r);
      JS_FreeValue(ctx, from);
      JS_FreeValue(ctx, array_ctor);
    } else {
      JS_ThrowTypeError(ctx, "multi-return value must be iterable");
      arr = JS_EXCEPTION;
    }
    if (JS_IsException(arr)) {
      JS_FreeValue(ctx, r);
      return kJsException;
    }
    {
      JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
      uint32_t len = 0;
      JS_ToUint32(ctx, &len, lenv);
      JS_FreeValue(ctx, lenv);
      if (len != nrets) {
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, r);
        JS_ThrowTypeError(ctx, "multi-return length mismatch");
        return kJsException;
      }
    }
    for (i = 0; i < nrets; i++) {
      JSValue e = JS_GetPropertyUint32(ctx, arr, i);
      int rc = to_wasm(ctx, s, t->types[i], e, &sp[i]);
      JS_FreeValue(ctx, e);
      if (rc < 0) {
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, r);
        return kJsException;
      }
    }
    JS_FreeValue(ctx, arr);
  }
  JS_FreeValue(ctx, r);
  return m3Err_none;
}

/* ---------------------------------------------------------------------- */
/* Table and Global wrappers */

static void table_finalizer(JSRuntime *rt, JSValueConst val) {
  WTable *wt = JS_GetOpaque(val, table_class_id);
  if (!wt)
    return;
  if (wt->tab->hostData == wt)
    wt->tab->hostData = NULL;
  store_unref(wt->store);
  free(wt);
}

static JSValue table_value(JSContext *ctx, Store *s, IM3Table tab, JSValueConst new_target) {
  WTable *wt = tab->hostData;
  JSValue obj;
  if (wt)
    return JS_DupValue(ctx, wt->obj);
  obj = new_wrapper(ctx, new_target, table_class_id);
  if (JS_IsException(obj))
    return obj;
  wt = calloc(1, sizeof(*wt));
  wt->store = store_ref(s);
  wt->tab = tab;
  wt->obj = obj;
  tab->hostData = wt;
  JS_SetOpaque(obj, wt);
  return obj;
}

static void global_finalizer(JSRuntime *rt, JSValueConst val) {
  WGlobal *wg = JS_GetOpaque(val, global_class_id);
  if (!wg)
    return;
  if (wg->glob->hostData == wg)
    wg->glob->hostData = NULL;
  store_unref(wg->store);
  free(wg);
}

static JSValue global_value(JSContext *ctx, Store *s, IM3Global g, JSValueConst new_target) {
  WGlobal *wg;
  JSValue obj;
  if (g->resolved)
    g = g->resolved;
  wg = g->hostData;
  if (wg)
    return JS_DupValue(ctx, wg->obj);
  obj = new_wrapper(ctx, new_target, global_class_id);
  if (JS_IsException(obj))
    return obj;
  wg = calloc(1, sizeof(*wg));
  wg->store = store_ref(s);
  wg->glob = g;
  wg->obj = obj;
  g->hostData = wg;
  JS_SetOpaque(obj, wg);
  return obj;
}

static int value_type_of(JSContext *ctx, JSValueConst v, m3type_t *out, uint8_t *code) {
  const char *t = JS_ToCString(ctx, v);
  int r = 0;
  if (!t)
    return -1;
  if (!strcmp(t, "i32"))
    *out = c_m3Type_i32, *code = 0x7f;
  else if (!strcmp(t, "i64"))
    *out = c_m3Type_i64, *code = 0x7e;
  else if (!strcmp(t, "f32"))
    *out = c_m3Type_f32, *code = 0x7d;
  else if (!strcmp(t, "f64"))
    *out = c_m3Type_f64, *code = 0x7c;
  else if (!strcmp(t, "anyfunc") || !strcmp(t, "funcref"))
    *out = c_m3Type_funcref, *code = 0x70;
  else if (!strcmp(t, "externref"))
    *out = c_m3Type_externref, *code = 0x6f;
  else
    r = 1;
  JS_FreeCString(ctx, t);
  return r;
}

static void global_store(IM3Global g, m3type_t type, uint64_t v) {
  switch (BaseTypeOf(type)) {
  case c_m3Type_i32: g->i32Value = *(int32_t *)&v; break;
  case c_m3Type_f32: g->f32Value = *(float *)&v; break;
  case c_m3Type_f64: g->f64Value = *(double *)&v; break;
  case c_m3Type_funcref:
  case c_m3Type_externref: g->refValue = *(void **)&v; break;
  default: g->i64Value = *(int64_t *)&v;
  }
}

static uint64_t global_load(IM3Global g) {
  uint64_t v = 0;
  switch (BaseTypeOf(g->type)) {
  case c_m3Type_i32: *(int32_t *)&v = g->i32Value; break;
  case c_m3Type_f32: *(float *)&v = g->f32Value; break;
  case c_m3Type_f64: *(double *)&v = g->f64Value; break;
  case c_m3Type_funcref:
  case c_m3Type_externref: *(void **)&v = g->refValue; break;
  default: *(int64_t *)&v = g->i64Value;
  }
  return v;
}

static JSValue js_global_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic, JSValueConst *data) {
  static const char who[] = "WebAssembly.Global()";
  Store *s = store_of_data(data);
  JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
  JSValue tv, mv, obj;
  m3type_t type;
  uint8_t code, body[16], *p = body;
  bool mut;
  int rc;
  uint64_t val = 0;
  IM3Module m;
  if (!require_new(ctx, this_val, "WebAssembly.Global"))
    return JS_EXCEPTION;
  if (!JS_IsObject(desc))
    return JS_ThrowTypeError(ctx, "%s: Argument 0 must be a global descriptor", who);
  mv = JS_GetPropertyStr(ctx, desc, "mutable");
  mut = JS_ToBool(ctx, mv);
  JS_FreeValue(ctx, mv);
  tv = JS_GetPropertyStr(ctx, desc, "value");
  if (JS_IsException(tv))
    return tv;
  rc = value_type_of(ctx, tv, &type, &code);
  JS_FreeValue(ctx, tv);
  if (rc < 0)
    return JS_EXCEPTION;
  if (rc)
    return JS_ThrowTypeError(ctx, "%s: Descriptor property 'value' must be a WebAssembly type", who);
  if (store_ensure(s, ctx) < 0)
    return JS_EXCEPTION;
  if (s->funcref_atom == JS_ATOM_NULL) {
    JSValue sym = JS_NewPrivateSymbol(ctx, "wasm function");
    s->funcref_atom = JS_ValueToAtom(ctx, sym);
    JS_FreeValue(ctx, sym);
  }
  if (argc > 1 && !JS_IsUndefined(argv[1])) {
    if (to_wasm(ctx, s, type, argv[1], &val) < 0)
      return JS_EXCEPTION;
  } else if (type == c_m3Type_externref) {
    val = (uint64_t)(uintptr_t)box_extern(s, ctx, JS_UNDEFINED);
  }
  *p++ = 1;
  *p++ = code;
  *p++ = mut ? 1 : 0;
  switch (code) {
  case 0x7f: *p++ = 0x41; *p++ = 0; break;
  case 0x7e: *p++ = 0x42; *p++ = 0; break;
  case 0x7d: *p++ = 0x43; memset(p, 0, 4); p += 4; break;
  case 0x7c: *p++ = 0x44; memset(p, 0, 8); p += 8; break;
  default: *p++ = 0xd0; *p++ = code; break;
  }
  *p++ = 0x0b;
  m = load_synthetic(ctx, s, 6, body, p - body, d_externalKind_global);
  if (!m)
    return JS_EXCEPTION;
  global_store(&m->globals[0], type, val);
  obj = global_value(ctx, s, &m->globals[0], this_val);
  return obj;
}

static WGlobal *global_this(JSContext *ctx, JSValueConst this_val) {
  return JS_GetOpaque2(ctx, this_val, global_class_id);
}

static JSValue js_global_get(JSContext *ctx, JSValueConst this_val) {
  WGlobal *wg = global_this(ctx, this_val);
  uint64_t v;
  if (!wg)
    return JS_EXCEPTION;
  v = global_load(wg->glob);
  return to_js(ctx, wg->store, wg->glob->type, &v);
}

static JSValue js_global_value_of(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  return js_global_get(ctx, this_val);
}

static JSValue js_global_set(JSContext *ctx, JSValueConst this_val, JSValueConst v) {
  WGlobal *wg = global_this(ctx, this_val);
  uint64_t val;
  if (!wg)
    return JS_EXCEPTION;
  if (!wg->glob->isMutable)
    return JS_ThrowTypeError(ctx, "set WebAssembly.Global.value: Can't set the value of an immutable global.");
  if (to_wasm(ctx, wg->store, wg->glob->type, v, &val) < 0)
    return JS_EXCEPTION;
  global_store(wg->glob, wg->glob->type, val);
  return JS_UNDEFINED;
}

static JSValue js_table_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic, JSValueConst *data) {
  static const char who[] = "WebAssembly.Table()";
  Store *s = store_of_data(data);
  JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
  double initial = 0, maximum = 0;
  int has_initial, has_max, rc;
  JSValue ev, obj;
  m3type_t type;
  uint8_t code, body[32], *p = body;
  IM3Module m;
  IM3Table tab;
  uint64_t init = 0;
  uint32_t i;
  if (!require_new(ctx, this_val, "WebAssembly.Table"))
    return JS_EXCEPTION;
  if (!JS_IsObject(desc))
    return JS_ThrowTypeError(ctx, "%s: Argument 0 must be a table descriptor", who);
  ev = JS_GetPropertyStr(ctx, desc, "element");
  if (JS_IsException(ev))
    return ev;
  rc = value_type_of(ctx, ev, &type, &code);
  JS_FreeValue(ctx, ev);
  if (rc < 0)
    return JS_EXCEPTION;
  if (rc || (type != c_m3Type_funcref && type != c_m3Type_externref))
    return JS_ThrowTypeError(ctx, "%s: Descriptor property 'element' must be a WebAssembly reference type", who);
  has_initial = desc_index(ctx, desc, "initial", d_m3MaxSaneTableSize, who, &initial);
  if (has_initial == -2)
    return JS_EXCEPTION;
  if (has_initial == -1) {
    has_initial = desc_index(ctx, desc, "minimum", d_m3MaxSaneTableSize, who, &initial);
    if (has_initial == -2)
      return JS_EXCEPTION;
    if (has_initial == -1)
      return JS_ThrowTypeError(ctx, "%s: Property 'initial' is required", who);
  }
  has_max = desc_index(ctx, desc, "maximum", 4294967295.0, who, &maximum);
  if (has_max == -2)
    return JS_EXCEPTION;
  if (has_max == 0 && maximum < initial)
    return JS_ThrowRangeError(ctx, "%s: Property 'maximum': value %.0f is below the lower bound %.0f",
                              who, maximum, initial);
  if (store_ensure(s, ctx) < 0)
    return JS_EXCEPTION;
  if (argc > 1 && !JS_IsUndefined(argv[1])) {
    if (to_wasm(ctx, s, type, argv[1], &init) < 0)
      return JS_EXCEPTION;
  } else if (type == c_m3Type_externref && argc > 1) {
    init = (uint64_t)(uintptr_t)box_extern(s, ctx, JS_UNDEFINED);
  }
  *p++ = 1;
  *p++ = code;
  *p++ = has_max == 0 ? 1 : 0;
  put_leb(&p, (uint64_t)initial);
  if (has_max == 0)
    put_leb(&p, (uint64_t)maximum);
  m = load_synthetic(ctx, s, 4, body, p - body, d_externalKind_table);
  if (!m)
    return JS_EXCEPTION;
  tab = m->tables[0];
  for (i = 0; i < tab->size; i++)
    tab->elements[i] = (void *)(uintptr_t)init;
  obj = table_value(ctx, s, tab, this_val);
  return obj;
}

static WTable *table_this(JSContext *ctx, JSValueConst this_val) {
  return JS_GetOpaque2(ctx, this_val, table_class_id);
}

static JSValue js_table_length(JSContext *ctx, JSValueConst this_val) {
  WTable *wt = table_this(ctx, this_val);
  return wt ? JS_NewUint32(ctx, wt->tab->size) : JS_EXCEPTION;
}

static int table_index(JSContext *ctx, JSValueConst v, uint32_t *out, const char *who) {
  double d;
  if (JS_ToFloat64(ctx, &d, v) < 0)
    return -1;
  if (d != d || d < 0 || d > 4294967295.0) {
    JS_ThrowTypeError(ctx, "%s: Argument 0 must be convertible to a valid number", who);
    return -1;
  }
  *out = (uint32_t)d;
  return 0;
}

static JSValue elem_to_js(JSContext *ctx, Store *s, IM3Table tab, void *e) {
  uint64_t v = (uint64_t)(uintptr_t)e;
  return to_js(ctx, s, tab->type, &v);
}

static JSValue js_table_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  WTable *wt = table_this(ctx, this_val);
  uint32_t i;
  if (!wt || table_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &i, "WebAssembly.Table.get()") < 0)
    return JS_EXCEPTION;
  if (i >= wt->tab->size)
    return JS_ThrowRangeError(ctx, "WebAssembly.Table.get(): invalid address %u in %s table of size %u",
                              i, BaseTypeOf(wt->tab->type) == c_m3Type_funcref ? "funcref" : "externref",
                              wt->tab->size);
  return elem_to_js(ctx, wt->store, wt->tab, wt->tab->elements[i]);
}

static int table_init_value(JSContext *ctx, WTable *wt, int argc, JSValueConst *argv, int idx,
                            uint64_t *out) {
  if (argc > idx)
    return to_wasm(ctx, wt->store, wt->tab->type, argv[idx], out);
  *out = 0; /* null */
  return 0;
}

static JSValue js_table_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  WTable *wt = table_this(ctx, this_val);
  uint32_t i;
  uint64_t v;
  if (!wt || table_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &i, "WebAssembly.Table.set()") < 0)
    return JS_EXCEPTION;
  if (i >= wt->tab->size)
    return JS_ThrowRangeError(ctx, "WebAssembly.Table.set(): invalid address %u in table of size %u",
                              i, wt->tab->size);
  if (table_init_value(ctx, wt, argc, argv, 1, &v) < 0)
    return JS_EXCEPTION;
  wt->tab->elements[i] = (void *)(uintptr_t)v;
  return JS_UNDEFINED;
}

static JSValue js_table_grow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  WTable *wt = table_this(ctx, this_val);
  uint32_t delta, old, max, i;
  uint64_t v;
  IM3Table tab;
  void **el;
  IM3Runtime rt;
  if (!wt || table_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &delta, "WebAssembly.Table.grow()") < 0)
    return JS_EXCEPTION;
  if (table_init_value(ctx, wt, argc, argv, 1, &v) < 0)
    return JS_EXCEPTION;
  tab = wt->tab;
  rt = wt->store->rt;
  old = tab->size;
  max = tab->maxSize ? tab->maxSize : d_m3MaxSaneTableSize;
  if (delta > max - old ||
      (rt->tableElementsLimit && delta > rt->tableElementsLimit - rt->tableElementsUsed))
    return JS_ThrowRangeError(ctx, "WebAssembly.Table.grow(): failed to grow table by %u", delta);
  if (delta == 0)
    return JS_NewUint32(ctx, old);
  el = realloc(tab->elements, (size_t)(old + delta) * sizeof(void *));
  if (!el)
    return JS_ThrowOutOfMemory(ctx);
  tab->elements = el;
  for (i = old; i < old + delta; i++)
    el[i] = (void *)(uintptr_t)v;
  tab->size = old + delta;
  rt->tableElementsUsed += delta;
  return JS_NewUint32(ctx, old);
}

/* ---------------------------------------------------------------------- */
/* Instance */

static void instance_keep(WInstance *wi, JSContext *ctx, JSValueConst v) {
  size_t cap = wi->ckeep;
  wi->keep = grow_array(wi->keep, &cap, wi->nkeep + 1, sizeof(JSValue));
  wi->ckeep = (uint32_t)cap;
  wi->keep[wi->nkeep++] = JS_DupValue(ctx, v);
}

static void instance_finalizer(JSRuntime *rt, JSValueConst val) {
  WInstance *wi = JS_GetOpaque(val, instance_class_id);
  Store *s;
  WInstance **pp;
  size_t i;
  uint32_t k;
  if (!wi)
    return;
  s = wi->store;
  for (i = 0; i < s->nhostfns; i++)
    if (s->hostfns[i]->inst == wi) {
      s->hostfns[i]->dead = true;
      s->hostfns[i]->fn = JS_UNDEFINED;
      s->hostfns[i]->inst = NULL;
    }
  for (pp = &s->instances; *pp; pp = &(*pp)->next)
    if (*pp == wi) {
      *pp = wi->next;
      break;
    }
  for (k = 0; k < wi->nkeep; k++)
    JS_FreeValueRT(rt, wi->keep[k]);
  free(wi->keep);
  JS_FreeValueRT(rt, wi->module_obj);
  JS_FreeValueRT(rt, wi->exports);
  store_unref(s);
  free(wi);
}

static void instance_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  WInstance *wi = JS_GetOpaque(val, instance_class_id);
  uint32_t k;
  if (!wi)
    return;
  JS_MarkValue(rt, wi->module_obj, mark_func);
  JS_MarkValue(rt, wi->exports, mark_func);
  for (k = 0; k < wi->nkeep; k++)
    JS_MarkValue(rt, wi->keep[k], mark_func);
}

/* importObject[module][name], with V8's TypeError for a missing module */
static JSValue lookup_import(JSContext *ctx, JSValueConst imports, const char *module,
                             const char *name, uint32_t index) {
  JSValue mod = JS_GetPropertyStr(ctx, imports, module), v;
  if (JS_IsException(mod))
    return mod;
  if (!JS_IsObject(mod)) {
    JS_FreeValue(ctx, mod);
    return JS_ThrowTypeError(ctx, "WebAssembly.Instance(): Import #%u \"%s\": module is not an object or function",
                             index, module);
  }
  v = JS_GetPropertyStr(ctx, mod, name);
  JS_FreeValue(ctx, mod);
  return v;
}

static bool limits_satisfy(uint64_t have_min, bool have_has_max, uint64_t have_max, uint64_t want_min,
                           bool want_has_max, uint64_t want_max) {
  if (have_min < want_min)
    return false;
  if (want_has_max)
    return have_has_max && have_max <= want_max;
  return true;
}

typedef struct {
  IM3Function f;
  JSValue fn;
} PendingHost;

static JSValue instantiate(JSContext *ctx, Store *s, JSValueConst module_obj, JSValueConst imports,
                           JSValueConst new_target) {
  static const char who[] = "WebAssembly.Instance()";
  WModule *wm = JS_GetOpaque(module_obj, module_class_id);
  IM3Module m = NULL;
  M3Result r;
  WInstance *wi = NULL;
  JSValue obj = JS_UNDEFINED, exports = JS_UNDEFINED, v;
  PendingHost *pend = NULL;
  uint32_t npend = 0, i, index = 0;
  bool has_imports;

  if (!wm)
    return JS_ThrowTypeError(ctx, "%s: Argument 0 must be a WebAssembly.Module", who);
  if (!JS_IsUndefined(imports) && !JS_IsObject(imports))
    return JS_ThrowTypeError(ctx, "%s: Argument 1 must be an object", who);
  has_imports = wm->parsed->numFuncImports > 0;
  for (i = 0; i < wm->parsed->numMemories && !has_imports; i++)
    has_imports = wm->parsed->memories[i]->imported;
  for (i = 0; i < wm->parsed->numTables && !has_imports; i++)
    has_imports = wm->parsed->tables[i]->imported;
  for (i = 0; i < wm->parsed->numGlobals && !has_imports; i++)
    has_imports = wm->parsed->globals[i].imported;
  if (has_imports && !JS_IsObject(imports))
    return JS_ThrowTypeError(ctx, "%s: Imports argument must be present and must be an object", who);
  if (store_ensure(s, ctx) < 0)
    return JS_EXCEPTION;
  if (s->funcref_atom == JS_ATOM_NULL) {
    JSValue sym = JS_NewPrivateSymbol(ctx, "wasm function");
    s->funcref_atom = JS_ValueToAtom(ctx, sym);
    JS_FreeValue(ctx, sym);
  }

  r = m3_ParseModule(s->env, &m, wm->blob->data, (uint32_t)wm->blob->len);
  if (r)
    return throw_wasm(ctx, s, ERR_COMPILE, "%s: %s", who, r);
  m3_SetModuleName(m, kHiddenName);
  obj = new_wrapper(ctx, new_target, instance_class_id);
  if (JS_IsException(obj)) {
    m3_FreeModule(m);
    return obj;
  }
  wi = calloc(1, sizeof(*wi));
  wi->store = store_ref(s);
  wi->obj = obj;
  wi->module_obj = JS_DupValue(ctx, module_obj);
  wi->exports = JS_UNDEFINED;
  JS_SetOpaque(obj, wi);
  pend = calloc(m->numFunctions + 1, sizeof(*pend));

  /* functions */
  for (i = 0; i < m->numFunctions; i++) {
    IM3Function f = &m->functions[i];
    WFunc *wf;
    if (f->wasm || !f->import.moduleUtf8)
      continue;
    v = lookup_import(ctx, imports, f->import.moduleUtf8, f->import.fieldUtf8, index++);
    if (JS_IsException(v))
      goto fail_parsed;
    if ((wf = wfunc_of(s, ctx, v)) != NULL) {
      IM3Function impl = Function_Implementation(wf->fn);
      if (impl->funcType != f->funcType && !AreFuncTypesEqual(impl->funcType, f->funcType)) {
        JS_FreeValue(ctx, v);
        throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": imported function does not match the expected type",
                   who, index - 1, f->import.moduleUtf8, f->import.fieldUtf8);
        goto fail_parsed;
      }
      f->resolved = impl;
      instance_keep(wi, ctx, v);
    } else if (JS_IsFunction(ctx, v)) {
      pend[npend].f = f;
      pend[npend].fn = JS_DupValue(ctx, v);
      npend++;
      instance_keep(wi, ctx, v);
    } else {
      JS_FreeValue(ctx, v);
      throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": function import requires a callable",
                 who, index - 1, f->import.moduleUtf8, f->import.fieldUtf8);
      goto fail_parsed;
    }
    JS_FreeValue(ctx, v);
  }

  /* tables */
  for (i = 0; i < m->numTables; i++) {
    IM3Table t = m->tables[i];
    WTable *wt;
    if (!t->imported || t->owner != m)
      continue;
    v = lookup_import(ctx, imports, t->import.moduleUtf8, t->import.fieldUtf8, index++);
    if (JS_IsException(v))
      goto fail_parsed;
    wt = JS_GetOpaque(v, table_class_id);
    if (!wt || wt->store != s) {
      JS_FreeValue(ctx, v);
      throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": table import requires a WebAssembly.Table",
                 who, index - 1, t->import.moduleUtf8, t->import.fieldUtf8);
      goto fail_parsed;
    }
    if (BaseTypeOf(wt->tab->type) != BaseTypeOf(t->type) || wt->tab->isTable64 != t->isTable64 ||
        !limits_satisfy(wt->tab->size, wt->tab->hasMax, wt->tab->maxSize, t->initSize, t->hasMax,
                        t->maxSize)) {
      JS_FreeValue(ctx, v);
      throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": table import has a mismatched type or size",
                 who, index - 1, t->import.moduleUtf8, t->import.fieldUtf8);
      goto fail_parsed;
    }
    m3_Free(t->elements);
    FreeImportInfo(&t->import);
    m3_Free(t);
    m->tables[i] = wt->tab;
    instance_keep(wi, ctx, v);
    JS_FreeValue(ctx, v);
  }

  /* memories */
  for (i = 0; i < m->numMemories; i++) {
    IM3Memory mem = m->memories[i];
    WMemory *wmem;
    if (!mem->imported || mem->owner != m)
      continue;
    v = lookup_import(ctx, imports, mem->import.moduleUtf8, mem->import.fieldUtf8, index++);
    if (JS_IsException(v))
      goto fail_parsed;
    wmem = JS_GetOpaque(v, memory_class_id);
    if (!wmem || wmem->store != s) {
      JS_FreeValue(ctx, v);
      throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": memory import must be a WebAssembly.Memory object",
                 who, index - 1, mem->import.moduleUtf8, mem->import.fieldUtf8);
      goto fail_parsed;
    }
    if (wmem->mem->isMemory64 != mem->isMemory64 || wmem->mem->isShared != mem->isShared ||
        Memory_PageSize(wmem->mem) != Memory_PageSize(mem) ||
        !limits_satisfy(wmem->mem->numPages, wmem->mem->hasMax, wmem->mem->maxPages, mem->initPages,
                        mem->hasMax, mem->maxPages)) {
      JS_FreeValue(ctx, v);
      throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": memory import has a mismatched type or size",
                 who, index - 1, mem->import.moduleUtf8, mem->import.fieldUtf8);
      goto fail_parsed;
    }
    FreeMemoryBlock(mem);
    FreeImportInfo(&mem->import);
    m3_Free(mem);
    m->memories[i] = wmem->mem;
    instance_keep(wi, ctx, v);
    JS_FreeValue(ctx, v);
  }

  /* globals */
  for (i = 0; i < m->numGlobals; i++) {
    IM3Global g = &m->globals[i];
    WGlobal *wg;
    if (!g->imported || !g->import.moduleUtf8)
      continue;
    v = lookup_import(ctx, imports, g->import.moduleUtf8, g->import.fieldUtf8, index++);
    if (JS_IsException(v))
      goto fail_parsed;
    wg = JS_GetOpaque(v, global_class_id);
    if (wg && wg->store == s) {
      if (wg->glob->type != g->type || wg->glob->isMutable != g->isMutable) {
        JS_FreeValue(ctx, v);
        throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": imported global does not match the expected %s",
                   who, index - 1, g->import.moduleUtf8, g->import.fieldUtf8,
                   wg->glob->type != g->type ? "type" : "mutability");
        goto fail_parsed;
      }
      g->resolved = wg->glob;
      instance_keep(wi, ctx, v);
    } else {
      uint64_t val;
      int base = BaseTypeOf(g->type);
      if (g->isMutable) {
        JS_FreeValue(ctx, v);
        throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": imported mutable global must be a WebAssembly.Global object",
                   who, index - 1, g->import.moduleUtf8, g->import.fieldUtf8);
        goto fail_parsed;
      }
      if ((base == c_m3Type_i64 && !JS_IsBigInt(v)) ||
          ((base == c_m3Type_i32 || base == c_m3Type_f32 || base == c_m3Type_f64) && !JS_IsNumber(v))) {
        JS_FreeValue(ctx, v);
        throw_wasm(ctx, s, ERR_LINK, "%s: Import #%u \"%s\" \"%s\": global import must be a number, valid Wasm reference, or WebAssembly.Global object",
                   who, index - 1, g->import.moduleUtf8, g->import.fieldUtf8);
        goto fail_parsed;
      }
      if (to_wasm(ctx, s, g->type, v, &val) < 0) {
        JS_FreeValue(ctx, v);
        goto fail_parsed;
      }
      global_store(g, g->type, val);
    }
    JS_FreeValue(ctx, v);
  }

#if d_m3HasExceptionHandling || d_m3HasStackSwitching
  for (i = 0; i < m->numTags; i++)
    if (m->tags[i].imported && m->tags[i].import.moduleUtf8) {
      throw_wasm(ctx, s, ERR_LINK, "%s: Import \"%s\" \"%s\": tag imports are not supported",
                 who, m->tags[i].import.moduleUtf8, m->tags[i].import.fieldUtf8);
      goto fail_parsed;
    }
#endif

  /* the store keeps the bytes for as long as the runtime keeps the module */
  store_keep_blob(s, wm->blob);
  r = m3_LoadModule(s->rt, m); /* owns m from here, success or not */
  m = NULL;
  if (r) {
    /* segments that do not fit trap at instantiation: RuntimeError */
    if (is_trap(r) || strstr(r, "out of bounds"))
      throw_wasm(ctx, s, ERR_RUNTIME, "%s: %s", who, trap_message(r));
    else
      throw_wasm(ctx, s, ERR_LINK, "%s: %s", who, r);
    goto fail;
  }
  wi->mod = m = s->rt->modules; /* LoadModule puts it first */
  wi->next = s->instances;
  s->instances = wi;
  for (i = 0; i < npend; i++) {
    HostFn *h = calloc(1, sizeof(*h));
    h->fn = pend[i].fn; /* the instance's keep list holds the reference */
    h->m3fn = pend[i].f;
    h->inst = wi;
    s->hostfns = grow_array(s->hostfns, &s->chostfns, s->nhostfns + 1, sizeof(HostFn *));
    s->hostfns[s->nhostfns++] = h;
    r = CompileRawFunction(m, pend[i].f, (const void *)host_thunk, h);
    if (r) {
      throw_wasm(ctx, s, ERR_LINK, "%s: %s", who, r);
      goto fail;
    }
  }

  r = m3_RunStart(m);
  if (r) {
    throw_call_result(ctx, s, r);
    goto fail;
  }

  /* exports */
  exports = JS_NewObjectProto(ctx, JS_NULL);
  for (i = 0; i < m->numExports; i++) {
    M3Export *e = &m->exports[i];
    JSAtom name;
    switch (e->kind) {
    case d_externalKind_function: {
      IM3Function f = Module_GetFunction(m, e->index);
      /* a JS import exported again gets a Wasm function of its own, as in V8 */
      v = f ? function_value(ctx, s, Function_Implementation(f)) : JS_UNDEFINED;
      break;
    }
    case d_externalKind_table:
      v = table_value(ctx, s, m->tables[e->index], JS_UNDEFINED);
      break;
    case d_externalKind_memory:
      v = memory_value(ctx, s, m->memories[e->index], JS_UNDEFINED);
      break;
    case d_externalKind_global:
      v = global_value(ctx, s, &m->globals[e->index], JS_UNDEFINED);
      break;
    default:
      v = JS_NewObjectProto(ctx, JS_NULL); /* tags are opaque */
    }
    if (JS_IsException(v))
      goto fail;
    name = JS_NewAtomLen(ctx, e->name, e->nameLength);
    JS_DefinePropertyValue(ctx, exports, name, v, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, name);
  }
  JS_FreezeObject(ctx, exports);
  wi->exports = exports;
  for (i = 0; i < npend; i++)
    JS_FreeValue(ctx, pend[i].fn);
  free(pend);
  return obj;

fail_parsed:
  if (m)
    m3_FreeModule(m);
fail:
  for (i = 0; i < npend; i++)
    JS_FreeValue(ctx, pend[i].fn);
  free(pend);
  JS_FreeValue(ctx, exports);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue js_instance_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic, JSValueConst *data) {
  if (!require_new(ctx, this_val, "WebAssembly.Instance"))
    return JS_EXCEPTION;
  return instantiate(ctx, store_of_data(data), argc > 0 ? argv[0] : JS_UNDEFINED,
                     argc > 1 ? argv[1] : JS_UNDEFINED, this_val);
}

static JSValue js_instance_exports(JSContext *ctx, JSValueConst this_val) {
  WInstance *wi = JS_GetOpaque2(ctx, this_val, instance_class_id);
  return wi ? JS_DupValue(ctx, wi->exports) : JS_EXCEPTION;
}

/* ---------------------------------------------------------------------- */
/* namespace functions */

static JSValue js_validate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic, JSValueConst *data) {
  Store *s = store_of_data(data);
  Blob *b = blob_from(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WebAssembly.validate()");
  IM3Module m = NULL;
  M3Result r;
  if (!b)
    return JS_EXCEPTION;
  if (store_ensure(s, ctx) < 0) {
    blob_unref(b);
    return JS_EXCEPTION;
  }
  r = parse_and_validate(s, b, &m);
  if (!r)
    m3_FreeModule(m);
  blob_unref(b);
  return JS_NewBool(ctx, r == NULL);
}

static JSValue settle(JSContext *ctx, JSValue value) {
  JSValue funcs[2], p = JS_NewPromiseCapability(ctx, funcs), r;
  if (JS_IsException(p)) {
    JS_FreeValue(ctx, value);
    return p;
  }
  if (JS_IsException(value)) {
    JSValue err = JS_GetException(ctx);
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, err);
  } else {
    r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(ctx, value);
  }
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, funcs[0]);
  JS_FreeValue(ctx, funcs[1]);
  return p;
}

static JSValue js_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic, JSValueConst *data) {
  Store *s = store_of_data(data);
  Blob *b = blob_from(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "WebAssembly.compile()");
  JSValue r;
  if (!b)
    return settle(ctx, JS_EXCEPTION);
  r = module_new(ctx, s, JS_UNDEFINED, b, "WebAssembly.compile()");
  blob_unref(b);
  return settle(ctx, r);
}

static JSValue js_instantiate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                              int magic, JSValueConst *data) {
  Store *s = store_of_data(data);
  JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
  JSValueConst imports = argc > 1 ? argv[1] : JS_UNDEFINED;
  JSValue mod, inst, res;
  Blob *b;
  if (JS_GetOpaque(src, module_class_id))
    return settle(ctx, instantiate(ctx, s, src, imports, JS_UNDEFINED));
  b = blob_from(ctx, src, "WebAssembly.instantiate()");
  if (!b)
    return settle(ctx, JS_EXCEPTION);
  mod = module_new(ctx, s, JS_UNDEFINED, b, "WebAssembly.instantiate()");
  blob_unref(b);
  if (JS_IsException(mod))
    return settle(ctx, mod);
  inst = instantiate(ctx, s, mod, imports, JS_UNDEFINED);
  if (JS_IsException(inst)) {
    JS_FreeValue(ctx, mod);
    return settle(ctx, inst);
  }
  res = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, res, "module", mod);
  JS_SetPropertyStr(ctx, res, "instance", inst);
  return settle(ctx, res);
}

/* ---------------------------------------------------------------------- */
/* installation */

static const JSCFunctionListEntry memory_proto[] = {
  JS_CGETSET_DEF("buffer", js_memory_buffer, NULL),
  JS_CFUNC_DEF("grow", 1, js_memory_grow),
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Memory", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry table_proto[] = {
  JS_CGETSET_DEF("length", js_table_length, NULL),
  JS_CFUNC_DEF("get", 1, js_table_get),
  JS_CFUNC_DEF("set", 1, js_table_set),
  JS_CFUNC_DEF("grow", 1, js_table_grow),
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Table", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry global_proto[] = {
  JS_CGETSET_DEF("value", js_global_get, js_global_set),
  JS_CFUNC_DEF("valueOf", 0, js_global_value_of),
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Global", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry instance_proto[] = {
  JS_CGETSET_DEF("exports", js_instance_exports, NULL),
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Instance", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry module_proto[] = {
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Module", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry namespace_props[] = {
  JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly", JS_PROP_CONFIGURABLE),
};

/* the error classes and the streaming functions, which are easiest in JS */
static const char helper_src[] =
  "(function (WA) {\n"
  "  'use strict';\n"
  "  const make = (name) => {\n"
  "    const C = ({ [name]: class extends Error {} })[name];\n"
  "    Object.defineProperty(C.prototype, 'name', { value: name, writable: true, configurable: true });\n"
  "    Object.defineProperty(WA, name, { value: C, writable: true, configurable: true });\n"
  "    return C;\n"
  "  };\n"
  "  const errors = [make('CompileError'), make('LinkError'), make('RuntimeError')];\n"
  "  const responseError = (m) => {\n"
  "    const e = new TypeError(`WebAssembly response ${m}`);\n"
  "    e.code = 'ERR_WEBASSEMBLY_RESPONSE';\n"
  "    return e;\n"
  "  };\n"
  "  const bytesOf = async (source) => {\n"
  "    const response = await source;\n"
  "    if (response === null || typeof response !== 'object' ||\n"
  "        typeof response.arrayBuffer !== 'function' || typeof response.headers !== 'object') {\n"
  "      const e = new TypeError('The \"source\" argument must be an instance of Response or an instance of Promise. Received ' + (response === null ? 'null' : typeof response));\n"
  "      e.code = 'ERR_INVALID_ARG_TYPE';\n"
  "      throw e;\n"
  "    }\n"
  "    const contentType = response.headers.get('Content-Type');\n"
  "    if (contentType !== 'application/wasm')\n"
  "      throw responseError(`has unsupported MIME type '${contentType}'`);\n"
  "    if (!response.ok)\n"
  "      throw responseError(`has status code ${response.status}`);\n"
  "    if (response.bodyUsed !== false)\n"
  "      throw responseError('body has already been used');\n"
  "    return response.arrayBuffer();\n"
  "  };\n"
  "  const { compile, instantiate } = WA;\n"
  "  const fns = {\n"
  "    compileStreaming(source) { return bytesOf(source).then((b) => compile(b)); },\n"
  "    instantiateStreaming(source, imports) {\n"
  "      return bytesOf(source).then((b) => instantiate(b, imports));\n"
  "    },\n"
  "  };\n"
  "  for (const k of ['compileStreaming', 'instantiateStreaming'])\n"
  "    Object.defineProperty(WA, k, { value: fns[k], writable: true, configurable: true });\n"
  "  return errors;\n"
  "})";

static pthread_mutex_t class_ids_lock = PTHREAD_MUTEX_INITIALIZER;

/* class ids are process-wide (each runtime registers the classes itself) */
static void alloc_class_ids(JSRuntime *rt) {
  pthread_mutex_lock(&class_ids_lock);
  if (!store_class_id) {
    JS_NewClassID(rt, &store_class_id);
    JS_NewClassID(rt, &module_class_id);
    JS_NewClassID(rt, &instance_class_id);
    JS_NewClassID(rt, &memory_class_id);
    JS_NewClassID(rt, &table_class_id);
    JS_NewClassID(rt, &global_class_id);
    JS_NewClassID(rt, &wfunc_class_id);
    m3_MemoryResizeHook = on_memory_resize;
  }
  pthread_mutex_unlock(&class_ids_lock);
}

static void register_class(JSRuntime *rt, JSClassID id, const char *name, JSClassFinalizer *fin,
                           JSClassGCMark *mark) {
  JSClassDef def = { 0 };
  if (JS_IsRegisteredClass(rt, id))
    return;
  def.class_name = name;
  def.finalizer = fin;
  def.gc_mark = mark;
  JS_NewClass(rt, id, &def);
}

static JSValue define_ctor(JSContext *ctx, JSValueConst ns, JSValueConst holder, const char *name,
                           JSCFunctionData *fn, JSClassID id, const JSCFunctionListEntry *proto_funcs,
                           int nproto) {
  JSValue ctor = JS_NewCFunctionData2(ctx, fn, name, 1, 0, 1, &holder);
  JSValue proto = JS_NewObject(ctx);
  JS_SetConstructorBit(ctx, ctor, true);
  if (nproto)
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, nproto);
  JS_SetConstructor(ctx, ctor, proto);
  JS_SetClassProto(ctx, id, proto); /* takes the reference */
  JS_DefinePropertyValueStr(ctx, ns, name, JS_DupValue(ctx, ctor),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  return ctor;
}

static void define_fn(JSContext *ctx, JSValueConst target, JSValueConst holder, const char *name,
                      JSCFunctionData *fn, int length) {
  JSValue f = JS_NewCFunctionData2(ctx, fn, name, length, 0, 1, &holder);
  JS_DefinePropertyValueStr(ctx, target, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

void node_wasm_install(JSContext *ctx) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  JSValue global, ns, holder, ctor, helper, errors;
  Store *s;
  int i;

  alloc_class_ids(rt);
  register_class(rt, store_class_id, "WebAssemblyStore", holder_finalizer, holder_mark);
  register_class(rt, module_class_id, "Module", module_finalizer, NULL);
  register_class(rt, instance_class_id, "Instance", instance_finalizer, instance_mark);
  register_class(rt, memory_class_id, "Memory", memory_finalizer, memory_mark);
  register_class(rt, table_class_id, "Table", table_finalizer, NULL);
  register_class(rt, global_class_id, "Global", global_finalizer, NULL);
  register_class(rt, wfunc_class_id, "WebAssemblyFunction", wfunc_finalizer, NULL);

  s = calloc(1, sizeof(*s));
  s->refs = 1; /* the holder's */
  s->jsrt = rt;
  s->ctx = ctx;
  s->funcref_atom = JS_ATOM_NULL;
  for (i = 0; i < 3; i++)
    s->err_ctors[i] = JS_UNDEFINED;
  holder = JS_NewObjectClass(ctx, store_class_id);
  JS_SetOpaque(holder, s);

  ns = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, ns, namespace_props, countof(namespace_props));
  /* the namespace keeps the store alive for as long as the realm uses it */
  {
    JSValue sym = JS_NewPrivateSymbol(ctx, "wasm store");
    JSAtom a = JS_ValueToAtom(ctx, sym);
    JS_DefinePropertyValue(ctx, ns, a, JS_DupValue(ctx, holder), 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, sym);
  }

  ctor = define_ctor(ctx, ns, holder, "Module", js_module_ctor, module_class_id, module_proto,
                     countof(module_proto));
  define_fn(ctx, ctor, holder, "imports", js_module_imports, 1);
  define_fn(ctx, ctor, holder, "exports", js_module_exports, 1);
  define_fn(ctx, ctor, holder, "customSections", js_module_custom_sections, 2);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, define_ctor(ctx, ns, holder, "Instance", js_instance_ctor, instance_class_id,
                                instance_proto, countof(instance_proto)));
  JS_FreeValue(ctx, define_ctor(ctx, ns, holder, "Memory", js_memory_ctor, memory_class_id,
                                memory_proto, countof(memory_proto)));
  JS_FreeValue(ctx, define_ctor(ctx, ns, holder, "Table", js_table_ctor, table_class_id,
                                table_proto, countof(table_proto)));
  JS_FreeValue(ctx, define_ctor(ctx, ns, holder, "Global", js_global_ctor, global_class_id,
                                global_proto, countof(global_proto)));
  define_fn(ctx, ns, holder, "validate", js_validate, 1);
  define_fn(ctx, ns, holder, "compile", js_compile, 1);
  define_fn(ctx, ns, holder, "instantiate", js_instantiate, 1);

  helper = JS_Eval(ctx, helper_src, sizeof(helper_src) - 1, "node:internal/wasm",
                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
  if (!JS_IsException(helper)) {
    errors = JS_Call(ctx, helper, JS_UNDEFINED, 1, (JSValueConst *)&ns);
    JS_FreeValue(ctx, helper);
    if (!JS_IsException(errors)) {
      for (i = 0; i < 3; i++)
        s->err_ctors[i] = JS_GetPropertyUint32(ctx, errors, i);
      JS_FreeValue(ctx, errors);
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
  } else {
    JS_FreeValue(ctx, JS_GetException(ctx));
  }

  global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(ctx, global, "WebAssembly", ns,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, holder);
}
