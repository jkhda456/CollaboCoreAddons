/* internalBinding('serdes') (node_serdes.cc) and internalBinding('ipc_serdes')
 * (node_ipc_serdes.cc): V8's ValueSerializer / ValueDeserializer
 * (src/objects/value-serializer.cc), wire format version 15, over QuickJS
 * values, byte for byte what Node's V8 writes for the same values.
 *
 * Where V8's output depends on its heap layout and QuickJS has no
 * equivalent, the common case is followed: integral numbers in the int32
 * range are Smis ('I'), an all-number array with a non-Smi element is a
 * double array (every element 'N'), and an array is dense when it has no
 * holes.  Objects of classes the bindings define are host objects, as V8's
 * API objects are. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

size_t node_stack_limit(void); /* main.c */

#define LATEST_VERSION 15

enum {
  T_VERSION = 0xFF, T_PADDING = 0, T_VERIFY_COUNT = '?', T_THE_HOLE = '-',
  T_UNDEFINED = '_', T_NULL = '0', T_TRUE = 'T', T_FALSE = 'F', T_INT32 = 'I',
  T_UINT32 = 'U', T_DOUBLE = 'N', T_BIGINT = 'Z', T_UTF8 = 'S', T_ONE_BYTE = '"',
  T_TWO_BYTE = 'c', T_REF = '^', T_BEGIN_OBJECT = 'o', T_END_OBJECT = '{',
  T_BEGIN_SPARSE = 'a', T_END_SPARSE = '@', T_BEGIN_DENSE = 'A', T_END_DENSE = '$',
  T_DATE = 'D', T_TRUE_OBJECT = 'y', T_FALSE_OBJECT = 'x', T_NUMBER_OBJECT = 'n',
  T_BIGINT_OBJECT = 'z', T_STRING_OBJECT = 's', T_REGEXP = 'R', T_BEGIN_MAP = ';',
  T_END_MAP = ':', T_BEGIN_SET = '\'', T_END_SET = ',', T_ARRAY_BUFFER = 'B',
  T_RESIZABLE_AB = '~', T_AB_TRANSFER = 't', T_VIEW = 'V', T_SAB = 'u',
  T_SHARED_OBJECT = 'p', T_WASM_MODULE = 'w', T_HOST_OBJECT = '\\',
  T_WASM_MEMORY = 'm', T_ERROR = 'r',
};

/* error subtags */
enum { E_EVAL = 'E', E_RANGE = 'R', E_REFERENCE = 'F', E_SYNTAX = 'S', E_TYPE = 'T',
       E_URI = 'U', E_MESSAGE = 'm', E_CAUSE = 'c', E_STACK = 's', E_END = '.' };

/* RegExp flag bits (V8's JSRegExp::Flag) */
static const struct { char c; uint32_t bit; } re_flags[] = {
  { 'd', 128 }, { 'g', 1 }, { 'i', 2 }, { 'l', 64 }, { 'm', 4 }, { 's', 32 },
  { 'u', 16 }, { 'v', 256 }, { 'y', 8 },
};
#define RE_FLAG_COUNT 9

/* ipc_serdes host objects */
enum { IPC_VIEW = 0, IPC_NOT_VIEW = 1, IPC_BUFFER_INDEX = 10 };

/* ---------------------------------------------------------------------- */
/* the realm's own methods, taken before user code can change them */

static const char helpers_src[] =
  "(function () {\n"
  "  'use strict';\n"
  "  const uncurry = (f) => Function.prototype.call.bind(f);\n"
  "  const getter = (C, k) => { const d = Object.getOwnPropertyDescriptor(C.prototype, k);\n"
  "    return d && d.get ? uncurry(d.get) : () => false; };\n"
  "  const bigToString = uncurry(BigInt.prototype.toString);\n"
  "  const fnToString = uncurry(Function.prototype.toString);\n"
  "  const symToString = uncurry(Symbol.prototype.toString);\n"
  "  const { getPrototypeOf, getOwnPropertyDescriptor } = Object;\n"
  "  const _Object = Object, _Map = Map, _Set = Set, _Date = Date, _RegExp = RegExp,\n"
  "    _AB = ArrayBuffer, _DV = DataView, _BigInt = BigInt, _Number = Number,\n"
  "    _String = String, _Boolean = Boolean;\n"
  "  return {\n"
  "    date: uncurry(Date.prototype.getTime),\n"
  "    num: uncurry(Number.prototype.valueOf), str: uncurry(String.prototype.valueOf),\n"
  "    bool: uncurry(Boolean.prototype.valueOf), big: uncurry(BigInt.prototype.valueOf),\n"
  "    bigParts: (b) => b < 0n ? [true, bigToString(-b, 16)] : [false, bigToString(b, 16)],\n"
  "    bigFromHex: (h, neg) => { const b = _BigInt('0x' + (h || '0')); return neg ? -b : b; },\n"
  "    reSource: getter(RegExp, 'source'), reFlags: getter(RegExp, 'flags'),\n"
  "    newRegExp: (p, f) => new _RegExp(p, f),\n"
  "    newDate: (t) => new _Date(t),\n"
  "    newNumber: (n) => new _Number(n), newString: (s) => new _String(s),\n"
  "    newBoolean: (b) => new _Boolean(b), newBigInt: (b) => _Object(b),\n"
  "    newMap: () => new _Map(), newSet: () => new _Set(),\n"
  "    mapSet: uncurry(Map.prototype.set), setAdd: uncurry(Set.prototype.add),\n"
  "    abResizable: getter(ArrayBuffer, 'resizable'), abMax: getter(ArrayBuffer, 'maxByteLength'),\n"
  "    abDetached: getter(ArrayBuffer, 'detached'),\n"
  "    sabGrowable: getter(SharedArrayBuffer, 'growable'),\n"
  "    newResizable: (len, max) => new _AB(len, { maxByteLength: max }),\n"
  "    newDataView: (ab, off, len) => len === undefined ? new _DV(ab, off) : new _DV(ab, off, len),\n"
  "    describe: (v) => {\n"
  "      try {\n"
  "        if (typeof v === 'function') return fnToString(v);\n"
  "        if (typeof v === 'symbol') return symToString(v);\n"
  "        const p = getPrototypeOf(v);\n"
  "        const d = p && getOwnPropertyDescriptor(p, 'constructor');\n"
  "        const n = d && typeof d.value === 'function' && d.value.name;\n"
  "        return '#<' + (typeof n === 'string' && n ? n : 'Object') + '>';\n"
  "      } catch { return '#<Object>'; }\n"
  "    },\n"
  "    errors: { E: EvalError, R: RangeError, F: ReferenceError, S: SyntaxError,\n"
  "              T: TypeError, U: URIError, '': Error },\n"
  "  };\n"
  "})()";

static JSValue get_helpers(JSContext *ctx) {
  Env *env = env_get(ctx);
  JSValue holder = env_binding_data(env, "serdes");
  JSValue h = JS_GetPropertyStr(ctx, holder, "helpers");
  if (JS_IsUndefined(h)) {
    h = JS_Eval(ctx, helpers_src, sizeof(helpers_src) - 1, "node:internal/serdes",
                JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(h))
      JS_SetPropertyStr(ctx, holder, "helpers", JS_DupValue(ctx, h));
  }
  JS_FreeValue(ctx, holder);
  return h;
}

static JSValue hcall(JSContext *ctx, JSValueConst h, const char *name, int argc,
                     JSValueConst *argv) {
  JSValue f = JS_GetPropertyStr(ctx, h, name), r;
  r = JS_Call(ctx, f, JS_UNDEFINED, argc, argv);
  JS_FreeValue(ctx, f);
  return r;
}

static JSValue hcall1(JSContext *ctx, JSValueConst h, const char *name, JSValueConst a) {
  return hcall(ctx, h, name, 1, &a);
}

static int max_depth(void) {
  static int depth;
  if (!depth) {
    size_t d = node_stack_limit() / 2048;
    depth = d < 64 ? 64 : d > 10000 ? 10000 : (int)d;
  }
  return depth;
}

/* a canonical array index (V8's elements: 0 .. 2^32 - 2) */
static bool array_index(const char *s, size_t len, uint32_t *out) {
  uint64_t v = 0;
  size_t i;
  if (len == 0 || len > 10 || (len > 1 && s[0] == '0'))
    return false;
  for (i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9')
      return false;
    v = v * 10 + (uint64_t)(s[i] - '0');
  }
  if (v > 4294967294u)
    return false;
  *out = (uint32_t)v;
  return true;
}

/* ---------------------------------------------------------------------- */
/* the serializer */

typedef struct {
  JSValue obj;
  uint32_t id;
} IdEntry;

typedef struct {
  uint32_t id;
  JSValue ab;
} Transfer;

typedef struct SerState {
  JSContext *ctx;
  uint8_t *buf;
  size_t len, cap;
  bool host_views;
  bool ipc;
  IdEntry *ids;          /* open addressing on the object pointer */
  uint32_t ids_cap, ids_count;
  uint32_t next_id;
  Transfer *xfer;
  uint32_t nxfer;
  int depth;
  JSValueConst self;     /* the Serializer object (serdes), during a call */
  JSValueConst buffer_ctor; /* ipc: Buffer */
  JSValue h;             /* helpers, during a call */
} SerState;

static uint32_t ptr_hash(const void *p) {
  uint64_t x = (uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (uint32_t)x;
}

static bool ids_find(SerState *s, JSValueConst obj, uint32_t *id) {
  void *p = JS_VALUE_GET_PTR(obj);
  uint32_t i;
  if (!s->ids_cap)
    return false;
  for (i = ptr_hash(p) & (s->ids_cap - 1);; i = (i + 1) & (s->ids_cap - 1)) {
    if (JS_IsUndefined(s->ids[i].obj))
      return false;
    if (JS_VALUE_GET_PTR(s->ids[i].obj) == p) {
      *id = s->ids[i].id;
      return true;
    }
  }
}

static void ids_put(SerState *s, JSValue obj, uint32_t id) {
  uint32_t i;
  if ((s->ids_count + 1) * 10 > s->ids_cap * 7) {
    uint32_t cap = s->ids_cap ? s->ids_cap * 2 : 64, k;
    IdEntry *old = s->ids, *t = malloc(sizeof(IdEntry) * cap);
    for (k = 0; k < cap; k++)
      t[k].obj = JS_UNDEFINED;
    for (k = 0; k < s->ids_cap; k++) {
      if (JS_IsUndefined(old[k].obj))
        continue;
      for (i = ptr_hash(JS_VALUE_GET_PTR(old[k].obj)) & (cap - 1);
           !JS_IsUndefined(t[i].obj); i = (i + 1) & (cap - 1))
        ;
      t[i] = old[k];
    }
    free(old);
    s->ids = t;
    s->ids_cap = cap;
  }
  for (i = ptr_hash(JS_VALUE_GET_PTR(obj)) & (s->ids_cap - 1); !JS_IsUndefined(s->ids[i].obj);
       i = (i + 1) & (s->ids_cap - 1))
    ;
  s->ids[i].obj = obj;
  s->ids[i].id = id;
  s->ids_count++;
}

static void ser_free_state(JSRuntime *rt, SerState *s) {
  uint32_t i;
  for (i = 0; i < s->ids_cap; i++)
    JS_FreeValueRT(rt, s->ids[i].obj);
  for (i = 0; i < s->nxfer; i++)
    JS_FreeValueRT(rt, s->xfer[i].ab);
  free(s->ids);
  free(s->xfer);
  free(s->buf);
}

static void ser_mark(JSRuntime *rt, SerState *s, JS_MarkFunc *mark) {
  uint32_t i;
  for (i = 0; i < s->ids_cap; i++)
    JS_MarkValue(rt, s->ids[i].obj, mark);
  for (i = 0; i < s->nxfer; i++)
    JS_MarkValue(rt, s->xfer[i].ab, mark);
}

static uint8_t *reserve(SerState *s, size_t n) {
  if (s->len + n > s->cap) {
    size_t cap = (s->len + n > s->cap * 2 ? s->len + n : s->cap * 2) + 64;
    uint8_t *b = realloc(s->buf, cap);
    if (!b) {
      JS_ThrowOutOfMemory(s->ctx);
      return NULL;
    }
    s->buf = b;
    s->cap = cap;
  }
  s->len += n;
  return s->buf + s->len - n;
}

static int w_raw(SerState *s, const void *p, size_t n) {
  uint8_t *d = reserve(s, n);
  if (!d)
    return -1;
  if (n)
    memcpy(d, p, n);
  return 0;
}

static int w_byte(SerState *s, uint8_t b) {
  return w_raw(s, &b, 1);
}

static int w_varint(SerState *s, uint64_t v) {
  uint8_t tmp[10];
  int n = 0;
  do {
    tmp[n++] = (uint8_t)((v & 0x7F) | 0x80);
    v >>= 7;
  } while (v);
  tmp[n - 1] &= 0x7F;
  return w_raw(s, tmp, n);
}

static size_t varint_size(uint64_t v) {
  size_t n = 0;
  do {
    n++;
    v >>= 7;
  } while (v);
  return n;
}

static int w_zigzag(SerState *s, int32_t v) {
  return w_varint(s, (uint32_t)(((uint32_t)v << 1) ^ (uint32_t)(v >> 31)));
}

static int w_double(SerState *s, double d) {
  return w_raw(s, &d, sizeof(d));
}

/* DataCloneError: the serializer's _getDataCloneError(message) (serdes), or
   an Error (ipc_serdes) */
static int throw_clone_error(SerState *s, const char *msg) {
  JSContext *ctx = s->ctx;
  if (!s->ipc && JS_IsObject(s->self)) {
    JSValue f = JS_GetPropertyStr(ctx, s->self, "_getDataCloneError");
    if (JS_IsException(f))
      return -1;
    if (JS_IsFunction(ctx, f)) {
      JSValue m = JS_NewString(ctx, msg);
      JSValue e = JS_Call(ctx, f, s->self, 1, (JSValueConst *)&m);
      JS_FreeValue(ctx, m);
      JS_FreeValue(ctx, f);
      if (JS_IsException(e))
        return -1;
      JS_Throw(ctx, e);
      return -1;
    }
    JS_FreeValue(ctx, f);
  }
  JS_ThrowPlainError(ctx, "%s", msg);
  return -1;
}

static int clone_error_for(SerState *s, JSValueConst v, const char *fallback) {
  JSContext *ctx = s->ctx;
  char *m;
  int r;
  const char *d = NULL;
  JSValue str = JS_UNDEFINED;
  if (!fallback) {
    str = hcall1(ctx, s->h, "describe", v);
    if (JS_IsException(str))
      return -1;
    d = JS_ToCString(ctx, str);
  }
  m = malloc(strlen(fallback ? fallback : d ? d : "value") + 32);
  sprintf(m, "%s could not be cloned.", fallback ? fallback : d ? d : "value");
  JS_FreeCString(ctx, d);
  JS_FreeValue(ctx, str);
  r = throw_clone_error(s, m);
  free(m);
  return r;
}

static int write_value(SerState *s, JSValueConst v);

static int write_string(SerState *s, JSValueConst str) {
  JSContext *ctx = s->ctx;
  JSValue v = JS_DupValue(ctx, str);
  uint32_t len, i;
  int wide, r = 0;
  const void *data = JS_NodeStringData(ctx, &v, &len, &wide);
  if (!data) {
    JS_FreeValue(ctx, v);
    return -1;
  }
  if (wide) {
    const uint16_t *u = data;
    uint16_t c = 0;
    for (i = 0; i < len; i++)
      c |= u[i];
    if (c < 0x100) {
      /* every character fits: V8 has it as a one-byte string */
      uint8_t *d;
      if (w_byte(s, T_ONE_BYTE) < 0 || w_varint(s, len) < 0 || !(d = reserve(s, len))) {
        r = -1;
      } else {
        for (i = 0; i < len; i++)
          d[i] = (uint8_t)u[i];
      }
    } else {
      uint32_t bytes = len * 2;
      /* the characters start at an even offset */
      if (((s->len + 1 + varint_size(bytes)) & 1) && w_byte(s, T_PADDING) < 0)
        r = -1;
      else if (w_byte(s, T_TWO_BYTE) < 0 || w_varint(s, bytes) < 0 || w_raw(s, u, bytes) < 0)
        r = -1;
    }
  } else {
    if (w_byte(s, T_ONE_BYTE) < 0 || w_varint(s, len) < 0 || w_raw(s, data, len) < 0)
      r = -1;
  }
  JS_FreeValue(ctx, v);
  return r;
}

static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}

/* bitfield (sign | byte length << 1), then the digits, 64-bit little endian */
static int write_bigint_contents(SerState *s, JSValueConst b) {
  JSContext *ctx = s->ctx;
  JSValue parts = hcall1(ctx, s->h, "bigParts", b), negv, hexv;
  const char *hex;
  size_t hlen, nbytes, padded, i;
  bool neg;
  uint8_t *d;
  int r = 0;
  if (JS_IsException(parts))
    return -1;
  negv = JS_GetPropertyUint32(ctx, parts, 0);
  hexv = JS_GetPropertyUint32(ctx, parts, 1);
  JS_FreeValue(ctx, parts);
  neg = JS_ToBool(ctx, negv);
  hex = JS_ToCStringLen(ctx, &hlen, hexv);
  JS_FreeValue(ctx, hexv);
  if (!hex)
    return -1;
  if (hlen == 1 && hex[0] == '0')
    hlen = 0;
  nbytes = (hlen + 1) / 2;
  padded = (nbytes + 7) / 8 * 8;
  if (w_varint(s, (uint32_t)((neg ? 1 : 0) | (padded << 1))) < 0 || !(d = reserve(s, padded))) {
    r = -1;
  } else {
    memset(d, 0, padded);
    /* hex is most significant first: byte i takes hex[hlen-2i-2 .. hlen-2i-1] */
    for (i = 0; i < hlen; i++) {
      size_t nib = hlen - 1 - i; /* nibble index from the least significant */
      d[nib / 2] |= (uint8_t)(hexval(hex[i]) << ((nib & 1) * 4));
    }
  }
  JS_FreeCString(ctx, hex);
  return r;
}

/* the value as V8 writes a number: a Smi when it is one */
static int write_number(SerState *s, double d) {
  if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d &&
      !(d == 0 && signbit(d))) {
    if (w_byte(s, T_INT32) < 0)
      return -1;
    return w_zigzag(s, (int32_t)d);
  }
  if (w_byte(s, T_DOUBLE) < 0)
    return -1;
  return w_double(s, d);
}

static bool is_smi(JSContext *ctx, JSValueConst v) {
  double d;
  if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
    return true;
  JS_ToFloat64(ctx, &d, v);
  return d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d &&
         !(d == 0 && signbit(d));
}

/* a property key: array indices are numbers, the rest strings */
static int write_key(SerState *s, JSAtom atom) {
  JSContext *ctx = s->ctx;
  JSValue k = JS_AtomToString(ctx, atom);
  size_t len;
  const char *c;
  uint32_t idx;
  int r;
  if (JS_IsException(k))
    return -1;
  c = JS_ToCStringLen(ctx, &len, k);
  if (c && array_index(c, len, &idx)) {
    JS_FreeCString(ctx, c);
    JS_FreeValue(ctx, k);
    return write_number(s, (double)idx);
  }
  JS_FreeCString(ctx, c);
  r = write_string(s, k);
  JS_FreeValue(ctx, k);
  return r;
}

/* the object's own enumerable string-keyed properties, as key/value pairs;
   skip_indices leaves out the array elements (dense arrays) */
static int write_properties(SerState *s, JSValueConst obj, bool skip_indices, uint32_t *count) {
  JSContext *ctx = s->ctx;
  JSPropertyEnum *props;
  uint32_t n, i;
  int r = 0;
  *count = 0;
  if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
    return -1;
  for (i = 0; i < n && r == 0; i++) {
    JSPropertyDescriptor desc;
    JSValue val;
    int found;
    if (skip_indices) {
      JSValue k = JS_AtomToString(ctx, props[i].atom);
      size_t len;
      const char *c = JS_ToCStringLen(ctx, &len, k);
      uint32_t idx;
      bool is_idx = c && array_index(c, len, &idx);
      JS_FreeCString(ctx, c);
      JS_FreeValue(ctx, k);
      if (is_idx)
        continue;
    }
    /* still there (an earlier getter may have deleted it)? */
    found = JS_GetOwnProperty(ctx, &desc, obj, props[i].atom);
    if (found < 0) {
      r = -1;
      break;
    }
    if (!found)
      continue;
    if (desc.flags & JS_PROP_GETSET) {
      JS_FreeValue(ctx, desc.getter);
      JS_FreeValue(ctx, desc.setter);
      JS_FreeValue(ctx, desc.value);
      val = JS_GetProperty(ctx, obj, props[i].atom);
      if (JS_IsException(val)) {
        r = -1;
        break;
      }
    } else {
      JS_FreeValue(ctx, desc.getter);
      JS_FreeValue(ctx, desc.setter);
      val = desc.value;
    }
    if (write_key(s, props[i].atom) < 0 || write_value(s, val) < 0)
      r = -1;
    else
      (*count)++;
    JS_FreeValue(ctx, val);
  }
  JS_FreePropertyEnum(ctx, props, n);
  return r;
}

static int write_js_object(SerState *s, JSValueConst obj) {
  uint32_t n;
  if (w_byte(s, T_BEGIN_OBJECT) < 0 || write_properties(s, obj, false, &n) < 0)
    return -1;
  if (w_byte(s, T_END_OBJECT) < 0)
    return -1;
  return w_varint(s, n);
}

static int write_array(SerState *s, JSValueConst arr) {
  JSContext *ctx = s->ctx;
  JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t length = 0, i, nindex = 0, nprops;
  JSPropertyEnum *props;
  uint32_t n, k;
  bool dense;
  if (JS_IsException(lv))
    return -1;
  JS_ToUint32(ctx, &length, lv);
  JS_FreeValue(ctx, lv);
  /* dense when every element is there */
  if (JS_GetOwnPropertyNames(ctx, &props, &n, arr, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
    return -1;
  for (k = 0; k < n; k++) {
    JSValue kv = JS_AtomToString(ctx, props[k].atom);
    size_t len;
    const char *c = JS_ToCStringLen(ctx, &len, kv);
    uint32_t idx;
    if (c && array_index(c, len, &idx) && idx < length)
      nindex++;
    JS_FreeCString(ctx, c);
    JS_FreeValue(ctx, kv);
  }
  JS_FreePropertyEnum(ctx, props, n);
  dense = nindex == length;
  if (!dense) {
    if (w_byte(s, T_BEGIN_SPARSE) < 0 || w_varint(s, length) < 0 ||
        write_properties(s, arr, false, &nprops) < 0 || w_byte(s, T_END_SPARSE) < 0 ||
        w_varint(s, nprops) < 0)
      return -1;
    return w_varint(s, length);
  }
  if (w_byte(s, T_BEGIN_DENSE) < 0 || w_varint(s, length) < 0)
    return -1;
  {
    /* an array of numbers with a non-Smi among them is a double array:
       every element a double */
    bool numbers = length > 0, any_double = false;
    for (i = 0; i < length && numbers; i++) {
      JSValue e = JS_GetPropertyUint32(ctx, arr, i);
      if (JS_IsException(e))
        return -1;
      if (!JS_IsNumber(e))
        numbers = false;
      else if (!is_smi(ctx, e))
        any_double = true;
      JS_FreeValue(ctx, e);
    }
    for (i = 0; i < length; i++) {
      JSValue e = JS_GetPropertyUint32(ctx, arr, i);
      int r;
      if (JS_IsException(e))
        return -1;
      if (numbers && any_double && JS_IsNumber(e)) {
        double d;
        JS_ToFloat64(ctx, &d, e);
        r = w_byte(s, T_DOUBLE) < 0 || w_double(s, d) < 0 ? -1 : 0;
      } else {
        r = write_value(s, e);
      }
      JS_FreeValue(ctx, e);
      if (r < 0)
        return -1;
    }
  }
  if (write_properties(s, arr, true, &nprops) < 0 || w_byte(s, T_END_DENSE) < 0 ||
      w_varint(s, nprops) < 0)
    return -1;
  return w_varint(s, length);
}

static int write_regexp(SerState *s, JSValueConst re) {
  JSContext *ctx = s->ctx;
  JSValue src = hcall1(ctx, s->h, "reSource", re), fl;
  const char *f;
  uint32_t bits = 0;
  int r, i;
  if (JS_IsException(src))
    return -1;
  fl = hcall1(ctx, s->h, "reFlags", re);
  if (JS_IsException(fl)) {
    JS_FreeValue(ctx, src);
    return -1;
  }
  f = JS_ToCString(ctx, fl);
  for (i = 0; f && f[i]; i++) {
    int k;
    for (k = 0; k < RE_FLAG_COUNT; k++)
      if (re_flags[k].c == f[i])
        bits |= re_flags[k].bit;
  }
  JS_FreeCString(ctx, f);
  r = w_byte(s, T_REGEXP) < 0 || !JS_IsString(src) || write_string(s, src) < 0 ||
      w_varint(s, bits) < 0 ? -1 : 0;
  JS_FreeValue(ctx, src);
  JS_FreeValue(ctx, fl);
  return r;
}

static int write_collection(SerState *s, JSValueConst obj, bool is_map) {
  JSContext *ctx = s->ctx;
  bool kv;
  JSValue entries = JS_NodePreviewEntries(ctx, obj, &kv), lv;
  uint32_t n = 0, i;
  if (JS_IsException(entries))
    return -1;
  lv = JS_GetPropertyStr(ctx, entries, "length");
  JS_ToUint32(ctx, &n, lv);
  JS_FreeValue(ctx, lv);
  if (w_byte(s, is_map ? T_BEGIN_MAP : T_BEGIN_SET) < 0)
    goto fail;
  for (i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, entries, i);
    int r = write_value(s, e);
    JS_FreeValue(ctx, e);
    if (r < 0)
      goto fail;
  }
  JS_FreeValue(ctx, entries);
  if (w_byte(s, is_map ? T_END_MAP : T_END_SET) < 0)
    return -1;
  return w_varint(s, n);
fail:
  JS_FreeValue(ctx, entries);
  return -1;
}

static int write_array_buffer(SerState *s, JSValueConst ab) {
  JSContext *ctx = s->ctx;
  JSValue rv, dv;
  size_t len;
  uint8_t *data;
  uint32_t i;
  bool resizable, detached;
  if (JS_NodeIsSharedArrayBuffer(ab)) {
    JSValue f = JS_UNDEFINED, idv;
    uint32_t id;
    if (!s->ipc && JS_IsObject(s->self)) {
      f = JS_GetPropertyStr(ctx, s->self, "_getSharedArrayBufferId");
      if (JS_IsException(f))
        return -1;
    }
    if (!JS_IsFunction(ctx, f)) {
      JS_FreeValue(ctx, f);
      return clone_error_for(s, ab, "#<SharedArrayBuffer>");
    }
    idv = JS_Call(ctx, f, s->self, 1, &ab);
    JS_FreeValue(ctx, f);
    if (JS_IsException(idv))
      return -1;
    if (JS_ToUint32(ctx, &id, idv) < 0) {
      JS_FreeValue(ctx, idv);
      return -1;
    }
    JS_FreeValue(ctx, idv);
    if (w_byte(s, T_SAB) < 0)
      return -1;
    return w_varint(s, id);
  }
  for (i = 0; i < s->nxfer; i++) {
    if (JS_VALUE_GET_PTR(s->xfer[i].ab) == JS_VALUE_GET_PTR(ab)) {
      if (w_byte(s, T_AB_TRANSFER) < 0)
        return -1;
      return w_varint(s, s->xfer[i].id);
    }
  }
  dv = hcall1(ctx, s->h, "abDetached", ab);
  if (JS_IsException(dv))
    return -1;
  detached = JS_ToBool(ctx, dv);
  JS_FreeValue(ctx, dv);
  if (detached)
    return throw_clone_error(s, "An ArrayBuffer is detached and could not be cloned.");
  data = JS_NodeGetBufferBytes(ctx, ab, &len);
  if (len > 0xFFFFFFFFu)
    return clone_error_for(s, ab, NULL);
  rv = hcall1(ctx, s->h, "abResizable", ab);
  if (JS_IsException(rv))
    return -1;
  resizable = JS_ToBool(ctx, rv);
  JS_FreeValue(ctx, rv);
  if (resizable) {
    JSValue mv = hcall1(ctx, s->h, "abMax", ab);
    double max = 0;
    if (JS_IsException(mv))
      return -1;
    JS_ToFloat64(ctx, &max, mv);
    JS_FreeValue(ctx, mv);
    if (max > 4294967295.0)
      return clone_error_for(s, ab, NULL);
    data = JS_NodeGetBufferBytes(ctx, ab, &len);
    if (w_byte(s, T_RESIZABLE_AB) < 0 || w_varint(s, len) < 0 || w_varint(s, (uint32_t)max) < 0)
      return -1;
    data = JS_NodeGetBufferBytes(ctx, ab, &len);
    return w_raw(s, data, len);
  }
  if (w_byte(s, T_ARRAY_BUFFER) < 0 || w_varint(s, len) < 0)
    return -1;
  data = JS_NodeGetBufferBytes(ctx, ab, &len);
  return w_raw(s, data, len);
}

static int view_subtag(JSValueConst v) {
  if (JS_NodeTypeFlags(v) & JS_NODE_TYPE_DATA_VIEW)
    return '?';
  switch (JS_GetTypedArrayType(v)) {
  case JS_TYPED_ARRAY_UINT8C: return 'C';
  case JS_TYPED_ARRAY_INT8: return 'b';
  case JS_TYPED_ARRAY_UINT8: return 'B';
  case JS_TYPED_ARRAY_INT16: return 'w';
  case JS_TYPED_ARRAY_UINT16: return 'W';
  case JS_TYPED_ARRAY_INT32: return 'd';
  case JS_TYPED_ARRAY_UINT32: return 'D';
  case JS_TYPED_ARRAY_BIG_INT64: return 'q';
  case JS_TYPED_ARRAY_BIG_UINT64: return 'Q';
  case JS_TYPED_ARRAY_FLOAT16: return 'h';
  case JS_TYPED_ARRAY_FLOAT32: return 'f';
  case JS_TYPED_ARRAY_FLOAT64: return 'F';
  default: return -1;
  }
}

static int write_host_object(SerState *s, JSValueConst obj);

static int write_view(SerState *s, JSValueConst view) {
  JSContext *ctx = s->ctx;
  size_t off = 0, len = 0;
  JSValue ab, rv;
  uint32_t flags = 0;
  int tag;
  if (s->host_views)
    return write_host_object(s, view);
  tag = view_subtag(view);
  ab = JS_NodeGetViewBuffer(ctx, view, &off, &len);
  if (!JS_NodeIsSharedArrayBuffer(ab)) {
    rv = hcall1(ctx, s->h, "abResizable", ab);
    if (JS_IsException(rv)) {
      JS_FreeValue(ctx, ab);
      return -1;
    }
    if (JS_ToBool(ctx, rv))
      flags |= 2; /* backed by a resizable buffer */
    JS_FreeValue(ctx, rv);
  }
  JS_FreeValue(ctx, ab);
  if (tag < 0)
    return clone_error_for(s, view, NULL);
  if (w_byte(s, T_VIEW) < 0 || w_varint(s, (uint8_t)tag) < 0 || w_varint(s, (uint32_t)off) < 0 ||
      w_varint(s, (uint32_t)len) < 0)
    return -1;
  return w_varint(s, flags);
}

static int write_error(SerState *s, JSValueConst err) {
  JSContext *ctx = s->ctx;
  JSAtom amsg = JS_NewAtom(ctx, "message"), acause = JS_NewAtom(ctx, "cause");
  JSPropertyDescriptor mdesc, cdesc;
  int mfound, cfound, r = -1;
  JSValue name = JS_UNDEFINED, stack = JS_UNDEFINED, msg = JS_UNDEFINED;
  const char *n = NULL;
  mfound = JS_GetOwnProperty(ctx, &mdesc, err, amsg);
  cfound = mfound < 0 ? -1 : JS_GetOwnProperty(ctx, &cdesc, err, acause);
  JS_FreeAtom(ctx, amsg);
  JS_FreeAtom(ctx, acause);
  if (mfound < 0 || cfound < 0) {
    if (mfound > 0) {
      JS_FreeValue(ctx, mdesc.value);
      JS_FreeValue(ctx, mdesc.getter);
      JS_FreeValue(ctx, mdesc.setter);
    }
    return -1;
  }
  if (w_byte(s, T_ERROR) < 0)
    goto done;
  name = JS_GetPropertyStr(ctx, err, "name");
  if (JS_IsException(name))
    goto done;
  {
    JSValue ns = JS_ToString(ctx, name);
    if (JS_IsException(ns))
      goto done;
    n = JS_ToCString(ctx, ns);
    JS_FreeValue(ctx, ns);
  }
  if (n) {
    int t = !strcmp(n, "EvalError") ? E_EVAL : !strcmp(n, "RangeError") ? E_RANGE
          : !strcmp(n, "ReferenceError") ? E_REFERENCE : !strcmp(n, "SyntaxError") ? E_SYNTAX
          : !strcmp(n, "TypeError") ? E_TYPE : !strcmp(n, "URIError") ? E_URI : 0;
    if (t && w_varint(s, t) < 0)
      goto done;
  }
  if (mfound > 0 && !(mdesc.flags & JS_PROP_GETSET)) {
    msg = JS_ToString(ctx, mdesc.value);
    if (JS_IsException(msg) || w_varint(s, E_MESSAGE) < 0 || write_string(s, msg) < 0)
      goto done;
  }
  stack = JS_GetPropertyStr(ctx, err, "stack");
  if (JS_IsException(stack))
    goto done;
  if (JS_IsString(stack) && (w_varint(s, E_STACK) < 0 || write_string(s, stack) < 0))
    goto done;
  if (cfound > 0 && !(cdesc.flags & JS_PROP_GETSET)) {
    if (w_varint(s, E_CAUSE) < 0 || write_value(s, cdesc.value) < 0)
      goto done;
  }
  r = w_varint(s, E_END);
done:
  if (mfound > 0) {
    JS_FreeValue(ctx, mdesc.value);
    JS_FreeValue(ctx, mdesc.getter);
    JS_FreeValue(ctx, mdesc.setter);
  }
  if (cfound > 0) {
    JS_FreeValue(ctx, cdesc.value);
    JS_FreeValue(ctx, cdesc.getter);
    JS_FreeValue(ctx, cdesc.setter);
  }
  JS_FreeCString(ctx, n);
  JS_FreeValue(ctx, name);
  JS_FreeValue(ctx, msg);
  JS_FreeValue(ctx, stack);
  return r;
}

/* ipc_serdes' host objects: views as their bytes, other objects as a copy of
   their own enumerable properties */
static int ipc_view_index(JSValueConst v) {
  if (JS_NodeTypeFlags(v) & JS_NODE_TYPE_DATA_VIEW)
    return 9;
  switch (JS_GetTypedArrayType(v)) {
  case JS_TYPED_ARRAY_INT8: return 0;
  case JS_TYPED_ARRAY_UINT8: return 1;
  case JS_TYPED_ARRAY_UINT8C: return 2;
  case JS_TYPED_ARRAY_INT16: return 3;
  case JS_TYPED_ARRAY_UINT16: return 4;
  case JS_TYPED_ARRAY_INT32: return 5;
  case JS_TYPED_ARRAY_UINT32: return 6;
  case JS_TYPED_ARRAY_FLOAT32: return 7;
  case JS_TYPED_ARRAY_FLOAT64: return 8;
  case JS_TYPED_ARRAY_BIG_INT64: return 11;
  case JS_TYPED_ARRAY_BIG_UINT64: return 12;
  case JS_TYPED_ARRAY_FLOAT16: return 13;
  default: return -1;
  }
}

static int ipc_write_host(SerState *s, JSValueConst obj) {
  JSContext *ctx = s->ctx;
  if (JS_NodeIsArrayBufferView(obj)) {
    JSValue c = JS_GetPropertyStr(ctx, obj, "constructor");
    int idx;
    size_t len;
    uint8_t *data;
    if (JS_IsException(c))
      return -1;
    idx = JS_IsStrictEqual(ctx, c, s->buffer_ctor) ? IPC_BUFFER_INDEX : ipc_view_index(obj);
    JS_FreeValue(ctx, c);
    if (idx < 0) {
      node_throw_error(ctx, "ERR_INVALID_STATE", "Unserializable host object");
      return -1;
    }
    data = JS_NodeGetBufferBytes(ctx, obj, &len);
    if (w_varint(s, IPC_VIEW) < 0 || w_varint(s, idx) < 0 || w_varint(s, (uint32_t)len) < 0)
      return -1;
    data = JS_NodeGetBufferBytes(ctx, obj, &len);
    return w_raw(s, data, len);
  }
  {
    JSPropertyEnum *props;
    uint32_t n, i;
    JSValue copy;
    int r;
    if (w_varint(s, IPC_NOT_VIEW) < 0)
      return -1;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
      return -1;
    copy = JS_NewObject(ctx);
    for (i = 0; i < n; i++) {
      JSValue v = JS_GetProperty(ctx, obj, props[i].atom);
      if (JS_IsException(v)) {
        JS_FreePropertyEnum(ctx, props, n);
        JS_FreeValue(ctx, copy);
        return -1;
      }
      JS_DefinePropertyValue(ctx, copy, props[i].atom, v, JS_PROP_C_W_E);
    }
    JS_FreePropertyEnum(ctx, props, n);
    r = write_value(s, copy);
    JS_FreeValue(ctx, copy);
    return r;
  }
}

static int write_host_object(SerState *s, JSValueConst obj) {
  JSContext *ctx = s->ctx;
  JSValue f, r;
  if (w_byte(s, T_HOST_OBJECT) < 0)
    return -1;
  if (s->ipc)
    return ipc_write_host(s, obj);
  f = JS_IsObject(s->self) ? JS_GetPropertyStr(ctx, s->self, "_writeHostObject") : JS_UNDEFINED;
  if (JS_IsException(f))
    return -1;
  if (!JS_IsFunction(ctx, f)) {
    JS_FreeValue(ctx, f);
    return clone_error_for(s, obj, "[object Object]");
  }
  r = JS_Call(ctx, f, s->self, 1, &obj);
  JS_FreeValue(ctx, f);
  if (JS_IsException(r))
    return -1;
  JS_FreeValue(ctx, r);
  return 0;
}

static int write_receiver(SerState *s, JSValueConst obj) {
  JSContext *ctx = s->ctx;
  uint32_t flags, id, cls;
  int r;
  if (ids_find(s, obj, &id)) {
    if (w_byte(s, T_REF) < 0)
      return -1;
    return w_varint(s, id);
  }
  ids_put(s, JS_DupValue(ctx, obj), s->next_id++);
  if (JS_IsFunction(ctx, obj))
    return clone_error_for(s, obj, NULL);
  flags = JS_NodeTypeFlags(obj);
  if (flags & (JS_NODE_TYPE_PROXY | JS_NODE_TYPE_ARGUMENTS))
    return clone_error_for(s, obj, "#<Object>");
  if (flags & JS_NODE_TYPE_SYMBOL_OBJECT)
    return clone_error_for(s, obj, "[object Symbol]");
  if (flags & JS_NODE_TYPE_GENERATOR_OBJECT)
    return clone_error_for(s, obj, "[object Generator]");
  if (flags & (JS_NODE_TYPE_MODULE_NAMESPACE | JS_NODE_TYPE_PROMISE | JS_NODE_TYPE_WEAKMAP |
               JS_NODE_TYPE_WEAKSET | JS_NODE_TYPE_WEAK_REF | JS_NODE_TYPE_MAP_ITERATOR |
               JS_NODE_TYPE_SET_ITERATOR))
    return clone_error_for(s, obj, NULL);
  if (JS_IsArray(obj))
    return write_array(s, obj);
  if (flags & JS_NODE_TYPE_DATE) {
    JSValue t = hcall1(ctx, s->h, "date", obj);
    double d = NAN;
    if (JS_IsException(t))
      return -1;
    JS_ToFloat64(ctx, &d, t);
    JS_FreeValue(ctx, t);
    if (w_byte(s, T_DATE) < 0)
      return -1;
    return w_double(s, d);
  }
  if (flags & JS_NODE_TYPE_BOOLEAN_OBJECT) {
    JSValue b = hcall1(ctx, s->h, "bool", obj);
    bool t;
    if (JS_IsException(b))
      return -1;
    t = JS_ToBool(ctx, b);
    JS_FreeValue(ctx, b);
    return w_byte(s, t ? T_TRUE_OBJECT : T_FALSE_OBJECT);
  }
  if (flags & JS_NODE_TYPE_NUMBER_OBJECT) {
    JSValue nv = hcall1(ctx, s->h, "num", obj);
    double d = NAN;
    if (JS_IsException(nv))
      return -1;
    JS_ToFloat64(ctx, &d, nv);
    JS_FreeValue(ctx, nv);
    if (w_byte(s, T_NUMBER_OBJECT) < 0)
      return -1;
    return w_double(s, d);
  }
  if (flags & JS_NODE_TYPE_BIGINT_OBJECT) {
    JSValue bv = hcall1(ctx, s->h, "big", obj);
    if (JS_IsException(bv))
      return -1;
    r = w_byte(s, T_BIGINT_OBJECT) < 0 || write_bigint_contents(s, bv) < 0 ? -1 : 0;
    JS_FreeValue(ctx, bv);
    return r;
  }
  if (flags & JS_NODE_TYPE_STRING_OBJECT) {
    JSValue sv = hcall1(ctx, s->h, "str", obj);
    if (JS_IsException(sv))
      return -1;
    r = w_byte(s, T_STRING_OBJECT) < 0 || write_string(s, sv) < 0 ? -1 : 0;
    JS_FreeValue(ctx, sv);
    return r;
  }
  if (flags & JS_NODE_TYPE_REGEXP)
    return write_regexp(s, obj);
  if (flags & JS_NODE_TYPE_MAP)
    return write_collection(s, obj, true);
  if (flags & JS_NODE_TYPE_SET)
    return write_collection(s, obj, false);
  if (flags & (JS_NODE_TYPE_ARRAY_BUFFER | JS_NODE_TYPE_SHARED_ARRAY_BUFFER))
    return write_array_buffer(s, obj);
  if (flags & (JS_NODE_TYPE_TYPED_ARRAY | JS_NODE_TYPE_DATA_VIEW))
    return write_view(s, obj);
  if (flags & JS_NODE_TYPE_NATIVE_ERROR)
    return write_error(s, obj);
  JS_GetAnyOpaque(obj, &cls);
  if (cls == 1 /* JS_CLASS_OBJECT: ordinary objects */)
    return write_js_object(s, obj);
  /* objects of the bindings' classes (and other exotic ones) */
  return write_host_object(s, obj);
}

static int write_value(SerState *s, JSValueConst v) {
  JSContext *ctx = s->ctx;
  int tag = JS_VALUE_GET_NORM_TAG(v), r;
  switch (tag) {
  case JS_TAG_UNDEFINED:
    return w_byte(s, T_UNDEFINED);
  case JS_TAG_NULL:
    return w_byte(s, T_NULL);
  case JS_TAG_BOOL:
    return w_byte(s, JS_VALUE_GET_BOOL(v) ? T_TRUE : T_FALSE);
  case JS_TAG_INT:
    if (w_byte(s, T_INT32) < 0)
      return -1;
    return w_zigzag(s, JS_VALUE_GET_INT(v));
  case JS_TAG_FLOAT64:
    return write_number(s, JS_VALUE_GET_FLOAT64(v));
  case JS_TAG_STRING:
  case JS_TAG_STRING_ROPE:
    return write_string(s, v);
  case JS_TAG_SYMBOL:
    return clone_error_for(s, v, NULL);
  case JS_TAG_OBJECT:
    break;
  default:
    if (JS_IsBigInt(v)) {
      if (w_byte(s, T_BIGINT) < 0)
        return -1;
      return write_bigint_contents(s, v);
    }
    return clone_error_for(s, v, NULL);
  }
  if (++s->depth > max_depth()) {
    s->depth--;
    JS_ThrowRangeError(ctx, "Maximum call stack size exceeded");
    return -1;
  }
  /* a view goes after its buffer (unless views are host objects) */
  if (JS_NodeIsArrayBufferView(v) && !s->host_views) {
    uint32_t id;
    if (!ids_find(s, v, &id)) {
      size_t off, len;
      JSValue ab = JS_NodeGetViewBuffer(ctx, v, &off, &len);
      r = write_receiver(s, ab);
      JS_FreeValue(ctx, ab);
      if (r < 0) {
        s->depth--;
        return -1;
      }
    }
  }
  r = write_receiver(s, v);
  s->depth--;
  return r;
}

/* ---------------------------------------------------------------------- */
/* the deserializer */

typedef struct DesState {
  JSContext *ctx;
  uint8_t *data;
  size_t len, pos;
  uint32_t version;
  JSValue *objs;
  uint32_t nobjs;
  uint32_t next_id;
  Transfer *xfer;
  uint32_t nxfer;
  int depth;
  bool ipc;
  JSValueConst self;
  JSValue h;
} DesState;

static void des_free_state(JSRuntime *rt, DesState *d) {
  uint32_t i;
  for (i = 0; i < d->nobjs; i++)
    JS_FreeValueRT(rt, d->objs[i]);
  for (i = 0; i < d->nxfer; i++)
    JS_FreeValueRT(rt, d->xfer[i].ab);
  free(d->objs);
  free(d->xfer);
  free(d->data);
}

static void des_mark(JSRuntime *rt, DesState *d, JS_MarkFunc *mark) {
  uint32_t i;
  for (i = 0; i < d->nobjs; i++)
    JS_MarkValue(rt, d->objs[i], mark);
  for (i = 0; i < d->nxfer; i++)
    JS_MarkValue(rt, d->xfer[i].ab, mark);
}

/* malformed data: V8's DataCloneDeserializationError */
static JSValue des_fail(DesState *d) {
  if (!JS_HasException(d->ctx))
    JS_ThrowPlainError(d->ctx, "Unable to deserialize cloned data.");
  return JS_EXCEPTION;
}

static void add_object(DesState *d, uint32_t id, JSValueConst obj) {
  if (id >= d->nobjs) {
    uint32_t n = id + 16, i;
    d->objs = realloc(d->objs, n * sizeof(JSValue));
    for (i = d->nobjs; i < n; i++)
      d->objs[i] = JS_UNDEFINED;
    d->nobjs = n;
  }
  JS_FreeValue(d->ctx, d->objs[id]);
  d->objs[id] = JS_DupValue(d->ctx, obj);
}

static bool r_tag(DesState *d, uint8_t *tag) {
  do {
    if (d->pos >= d->len)
      return false;
    *tag = d->data[d->pos++];
  } while (*tag == T_PADDING);
  return true;
}

static bool peek_tag(DesState *d, uint8_t *tag) {
  size_t p = d->pos;
  do {
    if (p >= d->len)
      return false;
    *tag = d->data[p++];
  } while (*tag == T_PADDING);
  return true;
}

static void consume_tag(DesState *d) {
  uint8_t t;
  r_tag(d, &t);
}

static bool r_varint(DesState *d, uint64_t *out, int bits) {
  uint64_t v = 0;
  unsigned shift = 0;
  bool more;
  do {
    uint8_t b;
    if (d->pos >= d->len)
      return false;
    b = d->data[d->pos];
    more = b & 0x80;
    if (shift < (unsigned)bits) {
      v |= (uint64_t)(b & 0x7F) << shift;
      shift += 7;
    } else {
      *out = bits < 64 ? v & ((1ULL << bits) - 1) : v;
      return true;
    }
    d->pos++;
  } while (more);
  *out = bits < 64 ? v & ((1ULL << bits) - 1) : v;
  return true;
}

static bool r_u32(DesState *d, uint32_t *out) {
  uint64_t v;
  if (!r_varint(d, &v, 32))
    return false;
  *out = (uint32_t)v;
  return true;
}

static bool r_u8v(DesState *d, uint8_t *out) {
  uint64_t v;
  if (!r_varint(d, &v, 8))
    return false;
  *out = (uint8_t)v;
  return true;
}

static bool r_double(DesState *d, double *out) {
  if (d->len - d->pos < sizeof(double))
    return false;
  memcpy(out, d->data + d->pos, sizeof(double));
  d->pos += sizeof(double);
  if (isnan(*out))
    *out = NAN;
  return true;
}

static bool r_raw(DesState *d, size_t n, const uint8_t **out) {
  if (n > d->len - d->pos)
    return false;
  *out = d->data + d->pos;
  d->pos += n;
  return true;
}

static JSValue read_value(DesState *d);

static JSValue read_one_byte(DesState *d) {
  uint32_t n;
  const uint8_t *p;
  if (!r_u32(d, &n) || !r_raw(d, n, &p))
    return des_fail(d);
  return JS_NodeNewStringLatin1(d->ctx, p, n);
}

static JSValue read_two_byte(DesState *d) {
  uint32_t n;
  const uint8_t *p;
  uint16_t *tmp;
  JSValue r;
  if (!r_u32(d, &n) || (n & 1) || !r_raw(d, n, &p))
    return des_fail(d);
  tmp = malloc(n ? n : 1);
  memcpy(tmp, p, n);
  r = JS_NodeNewStringUTF16(d->ctx, tmp, n / 2);
  free(tmp);
  return r;
}

static JSValue read_utf8(DesState *d) {
  uint32_t n;
  const uint8_t *p;
  if (!r_u32(d, &n) || !r_raw(d, n, &p))
    return des_fail(d);
  return node_new_utf8_string(d->ctx, p, n);
}

static JSValue read_string(DesState *d) {
  JSValue v;
  if (d->version < 12)
    return read_utf8(d);
  v = read_value(d);
  if (JS_IsException(v))
    return v;
  if (!JS_IsString(v)) {
    JS_FreeValue(d->ctx, v);
    return des_fail(d);
  }
  return v;
}

static JSValue read_bigint(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t bitfield;
  size_t bytes, i, k = 0;
  const uint8_t *p;
  char *hex;
  JSValue args[2], r;
  if (!r_u32(d, &bitfield))
    return des_fail(d);
  bytes = (bitfield >> 1) & 0x3FFFFFFF;
  if (!r_raw(d, bytes, &p))
    return des_fail(d);
  hex = malloc(bytes * 2 + 1);
  for (i = bytes; i > 0; i--) {
    static const char digits[] = "0123456789abcdef";
    uint8_t b = p[i - 1];
    if (k == 0 && b == 0)
      continue;
    hex[k++] = digits[b >> 4];
    hex[k++] = digits[b & 15];
  }
  hex[k] = 0;
  args[0] = JS_NewStringLen(ctx, hex, k);
  args[1] = JS_NewBool(ctx, bitfield & 1);
  free(hex);
  r = hcall(ctx, d->h, "bigFromHex", 2, args);
  JS_FreeValue(ctx, args[0]);
  return r;
}

static bool valid_key(JSValueConst k) {
  int t = JS_VALUE_GET_NORM_TAG(k);
  return t == JS_TAG_INT || t == JS_TAG_FLOAT64 || t == JS_TAG_STRING ||
         t == JS_TAG_STRING_ROPE || t == JS_TAG_SYMBOL;
}

/* key/value pairs up to end_tag, onto obj; the count of them */
static int read_properties(DesState *d, JSValueConst obj, uint8_t end_tag, uint32_t *count) {
  JSContext *ctx = d->ctx;
  *count = 0;
  for (;;) {
    uint8_t tag;
    JSValue k, v;
    JSAtom a;
    int found;
    if (!peek_tag(d, &tag)) {
      des_fail(d);
      return -1;
    }
    if (tag == end_tag) {
      consume_tag(d);
      return 0;
    }
    k = read_value(d);
    if (JS_IsException(k))
      return -1;
    if (!valid_key(k)) {
      JS_FreeValue(ctx, k);
      des_fail(d);
      return -1;
    }
    v = read_value(d);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, k);
      return -1;
    }
    a = JS_ValueToAtom(ctx, k);
    JS_FreeValue(ctx, k);
    if (a == JS_ATOM_NULL) {
      JS_FreeValue(ctx, v);
      return -1;
    }
    found = JS_GetOwnProperty(ctx, NULL, obj, a);
    if (found != 0) {
      JS_FreeAtom(ctx, a);
      JS_FreeValue(ctx, v);
      if (found > 0)
        des_fail(d);
      return -1;
    }
    if (JS_DefinePropertyValue(ctx, obj, a, v, JS_PROP_C_W_E) < 0) {
      JS_FreeAtom(ctx, a);
      return -1;
    }
    JS_FreeAtom(ctx, a);
    (*count)++;
  }
}

static JSValue read_js_object(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++, n, expected;
  JSValue obj = JS_NewObject(ctx);
  add_object(d, id, obj);
  if (read_properties(d, obj, T_END_OBJECT, &n) < 0 || !r_u32(d, &expected) || n != expected) {
    JS_FreeValue(ctx, obj);
    return des_fail(d);
  }
  return obj;
}

static int set_length(JSContext *ctx, JSValueConst arr, uint32_t len) {
  return JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, len));
}

static JSValue read_sparse_array(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t length, id, n, en, elen;
  JSValue arr;
  if (!r_u32(d, &length))
    return des_fail(d);
  id = d->next_id++;
  arr = JS_NewArray(ctx);
  if (set_length(ctx, arr, length) < 0) {
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
  }
  add_object(d, id, arr);
  if (read_properties(d, arr, T_END_SPARSE, &n) < 0 || !r_u32(d, &en) || !r_u32(d, &elen) ||
      n != en || length != elen) {
    JS_FreeValue(ctx, arr);
    return des_fail(d);
  }
  return arr;
}

static JSValue read_dense_array(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t length, id, i, n, en, elen;
  JSValue arr;
  if (!r_u32(d, &length) || length > d->len - d->pos)
    return des_fail(d);
  id = d->next_id++;
  arr = JS_NewArray(ctx);
  add_object(d, id, arr);
  for (i = 0; i < length; i++) {
    uint8_t tag;
    JSValue e;
    if (peek_tag(d, &tag) && tag == T_THE_HOLE) {
      consume_tag(d);
      continue;
    }
    e = read_value(d);
    if (JS_IsException(e)) {
      JS_FreeValue(ctx, arr);
      return e;
    }
    if (d->version < 11 && JS_IsUndefined(e))
      continue;
    if (JS_DefinePropertyValueUint32(ctx, arr, i, e, JS_PROP_C_W_E) < 0) {
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
  }
  if (set_length(ctx, arr, length) < 0) {
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
  }
  if (read_properties(d, arr, T_END_DENSE, &n) < 0 || !r_u32(d, &en) || !r_u32(d, &elen) ||
      n != en || length != elen) {
    JS_FreeValue(ctx, arr);
    return des_fail(d);
  }
  return arr;
}

static JSValue read_wrapper(DesState *d, uint8_t tag) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++;
  JSValue v, inner;
  double num;
  switch (tag) {
  case T_TRUE_OBJECT:
  case T_FALSE_OBJECT:
    inner = JS_NewBool(ctx, tag == T_TRUE_OBJECT);
    v = hcall1(ctx, d->h, "newBoolean", inner);
    break;
  case T_NUMBER_OBJECT:
    if (!r_double(d, &num))
      return des_fail(d);
    inner = JS_NewFloat64(ctx, num);
    v = hcall1(ctx, d->h, "newNumber", inner);
    break;
  case T_BIGINT_OBJECT:
    inner = read_bigint(d);
    if (JS_IsException(inner))
      return inner;
    v = hcall1(ctx, d->h, "newBigInt", inner);
    JS_FreeValue(ctx, inner);
    inner = JS_UNDEFINED;
    break;
  default:
    inner = read_string(d);
    if (JS_IsException(inner))
      return inner;
    v = hcall1(ctx, d->h, "newString", inner);
    JS_FreeValue(ctx, inner);
    inner = JS_UNDEFINED;
    break;
  }
  JS_FreeValue(ctx, inner);
  if (!JS_IsException(v))
    add_object(d, id, v);
  return v;
}

static JSValue read_regexp(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++, raw;
  JSValue pattern = read_string(d), args[2], re;
  char flags[RE_FLAG_COUNT + 1];
  int i, k = 0;
  if (JS_IsException(pattern))
    return pattern;
  if (!r_u32(d, &raw)) {
    JS_FreeValue(ctx, pattern);
    return des_fail(d);
  }
  /* unknown bits, and the linear engine (not enabled in Node), are invalid */
  if (raw & ~(uint32_t)(511 & ~64)) {
    JS_FreeValue(ctx, pattern);
    return des_fail(d);
  }
  for (i = 0; i < RE_FLAG_COUNT; i++)
    if (raw & re_flags[i].bit)
      flags[k++] = re_flags[i].c;
  flags[k] = 0;
  args[0] = pattern;
  args[1] = JS_NewString(ctx, flags);
  re = hcall(ctx, d->h, "newRegExp", 2, args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  if (JS_IsException(re)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return des_fail(d);
  }
  add_object(d, id, re);
  return re;
}

static JSValue read_collection(DesState *d, bool is_map) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++, length = 0, expected;
  JSValue coll = hcall(ctx, d->h, is_map ? "newMap" : "newSet", 0, NULL);
  JSValue fn;
  if (JS_IsException(coll))
    return coll;
  add_object(d, id, coll);
  fn = JS_GetPropertyStr(ctx, d->h, is_map ? "mapSet" : "setAdd");
  for (;;) {
    uint8_t tag;
    JSValue args[3], r;
    if (!peek_tag(d, &tag))
      goto fail;
    if (tag == (is_map ? T_END_MAP : T_END_SET)) {
      consume_tag(d);
      break;
    }
    args[0] = coll;
    args[1] = read_value(d);
    if (JS_IsException(args[1]))
      goto fail;
    args[2] = JS_UNDEFINED;
    if (is_map) {
      args[2] = read_value(d);
      if (JS_IsException(args[2])) {
        JS_FreeValue(ctx, args[1]);
        goto fail;
      }
    }
    r = JS_Call(ctx, fn, JS_UNDEFINED, is_map ? 3 : 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    if (JS_IsException(r))
      goto fail;
    JS_FreeValue(ctx, r);
    length += is_map ? 2 : 1;
  }
  JS_FreeValue(ctx, fn);
  if (!r_u32(d, &expected) || expected != length) {
    JS_FreeValue(ctx, coll);
    return des_fail(d);
  }
  return coll;
fail:
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, coll);
  return des_fail(d);
}

static JSValue find_transfer(DesState *d, uint32_t id) {
  uint32_t i;
  for (i = 0; i < d->nxfer; i++)
    if (d->xfer[i].id == id)
      return JS_DupValue(d->ctx, d->xfer[i].ab);
  return JS_UNDEFINED;
}

static JSValue read_array_buffer(DesState *d, bool shared, bool resizable) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++, len, max;
  const uint8_t *p;
  JSValue ab;
  if (shared) {
    uint32_t xid;
    if (!r_u32(d, &xid))
      return des_fail(d);
    ab = find_transfer(d, xid);
    if (!JS_NodeIsSharedArrayBuffer(ab)) {
      JS_FreeValue(ctx, ab);
      return des_fail(d);
    }
    add_object(d, id, ab);
    return ab;
  }
  if (!r_u32(d, &len))
    return des_fail(d);
  max = len;
  if (resizable && (!r_u32(d, &max) || len > max))
    return des_fail(d);
  if (len > d->len - d->pos)
    return des_fail(d);
  r_raw(d, len, &p);
  if (resizable) {
    JSValue args[2];
    size_t n;
    uint8_t *dst;
    args[0] = JS_NewUint32(ctx, len);
    args[1] = JS_NewUint32(ctx, max);
    ab = hcall(ctx, d->h, "newResizable", 2, args);
    if (JS_IsException(ab))
      return ab;
    dst = JS_NodeGetBufferBytes(ctx, ab, &n);
    if (dst && n >= len && len)
      memcpy(dst, p, len);
  } else {
    ab = JS_NewArrayBufferCopy(ctx, p, len);
    if (JS_IsException(ab))
      return ab;
  }
  add_object(d, id, ab);
  return ab;
}

static JSValue read_transferred(DesState *d) {
  uint32_t id = d->next_id++, xid;
  JSValue ab;
  if (!r_u32(d, &xid))
    return des_fail(d);
  ab = find_transfer(d, xid);
  if (JS_IsUndefined(ab))
    return des_fail(d);
  add_object(d, id, ab);
  return ab;
}

static int subtag_type(uint8_t tag, int *size) {
  switch (tag) {
  case 'b': *size = 1; return JS_TYPED_ARRAY_INT8;
  case 'B': *size = 1; return JS_TYPED_ARRAY_UINT8;
  case 'C': *size = 1; return JS_TYPED_ARRAY_UINT8C;
  case 'w': *size = 2; return JS_TYPED_ARRAY_INT16;
  case 'W': *size = 2; return JS_TYPED_ARRAY_UINT16;
  case 'd': *size = 4; return JS_TYPED_ARRAY_INT32;
  case 'D': *size = 4; return JS_TYPED_ARRAY_UINT32;
  case 'h': *size = 2; return JS_TYPED_ARRAY_FLOAT16;
  case 'f': *size = 4; return JS_TYPED_ARRAY_FLOAT32;
  case 'F': *size = 8; return JS_TYPED_ARRAY_FLOAT64;
  case 'q': *size = 8; return JS_TYPED_ARRAY_BIG_INT64;
  case 'Q': *size = 8; return JS_TYPED_ARRAY_BIG_UINT64;
  default: *size = 0; return -1;
  }
}

static JSValue read_view(DesState *d, JSValue ab) {
  JSContext *ctx = d->ctx;
  size_t ablen;
  uint8_t tag;
  uint32_t off, len, flags = 0, id;
  bool tracking, rab, resizable;
  JSValue rv, view;
  int size, type;
  JS_NodeGetBufferBytes(ctx, ab, &ablen);
  if (!r_u8v(d, &tag) || !r_u32(d, &off) || !r_u32(d, &len) || off > ablen ||
      len > ablen - off) {
    JS_FreeValue(ctx, ab);
    return des_fail(d);
  }
  if (d->version >= 14 && !r_u32(d, &flags)) {
    JS_FreeValue(ctx, ab);
    return des_fail(d);
  }
  id = d->next_id++;
  tracking = flags & 1;
  rab = flags & 2;
  rv = hcall1(ctx, d->h, JS_NodeIsSharedArrayBuffer(ab) ? "sabGrowable" : "abResizable", ab);
  resizable = JS_ToBool(ctx, rv);
  JS_FreeValue(ctx, rv);
  if (((rab || tracking) && !resizable) || (rab && JS_NodeIsSharedArrayBuffer(ab)) ||
      (resizable && !JS_NodeIsSharedArrayBuffer(ab) && !rab)) {
    JS_FreeValue(ctx, ab);
    return des_fail(d);
  }
  if (tag == '?') {
    JSValue args[3] = { ab, JS_NewUint32(ctx, off),
                        tracking ? JS_UNDEFINED : JS_NewUint32(ctx, len) };
    view = hcall(ctx, d->h, "newDataView", 3, args);
  } else {
    JSValue args[3];
    type = subtag_type(tag, &size);
    if (type < 0 || off % size || len % size) {
      JS_FreeValue(ctx, ab);
      return des_fail(d);
    }
    args[0] = ab;
    args[1] = JS_NewUint32(ctx, off);
    args[2] = JS_NewUint32(ctx, len / size);
    view = JS_NewTypedArray(ctx, tracking ? 2 : 3, (JSValueConst *)args, type);
  }
  JS_FreeValue(ctx, ab);
  if (JS_IsException(view))
    return view;
  add_object(d, id, view);
  return view;
}

static JSValue read_error(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++;
  uint8_t tag;
  const char *ctor = "";
  JSValue message = JS_UNDEFINED, stack = JS_UNDEFINED, errors, c, e;
  if (!r_u8v(d, &tag))
    return des_fail(d);
  switch (tag) {
  case E_EVAL: ctor = "E"; break;
  case E_RANGE: ctor = "R"; break;
  case E_REFERENCE: ctor = "F"; break;
  case E_SYNTAX: ctor = "S"; break;
  case E_TYPE: ctor = "T"; break;
  case E_URI: ctor = "U"; break;
  default: break;
  }
  if (*ctor && !r_u8v(d, &tag))
    return des_fail(d);
  if (tag == E_MESSAGE) {
    message = read_string(d);
    if (JS_IsException(message))
      return message;
    if (!r_u8v(d, &tag)) {
      JS_FreeValue(ctx, message);
      return des_fail(d);
    }
  }
  if (tag == E_STACK) {
    stack = read_string(d);
    if (JS_IsException(stack)) {
      JS_FreeValue(ctx, message);
      return stack;
    }
    if (!r_u8v(d, &tag)) {
      JS_FreeValue(ctx, message);
      JS_FreeValue(ctx, stack);
      return des_fail(d);
    }
  }
  errors = JS_GetPropertyStr(ctx, d->h, "errors");
  c = JS_GetPropertyStr(ctx, errors, ctor);
  JS_FreeValue(ctx, errors);
  e = JS_CallConstructor(ctx, c, JS_IsUndefined(message) ? 0 : 1, (JSValueConst *)&message);
  JS_FreeValue(ctx, c);
  JS_FreeValue(ctx, message);
  if (JS_IsException(e)) {
    JS_FreeValue(ctx, stack);
    return e;
  }
  JS_DefinePropertyValueStr(ctx, e, "stack", stack, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  add_object(d, id, e);
  if (tag == E_CAUSE) {
    JSValue cause = read_value(d);
    if (JS_IsException(cause)) {
      JS_FreeValue(ctx, e);
      return cause;
    }
    JS_DefinePropertyValueStr(ctx, e, "cause", cause, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (!r_u8v(d, &tag)) {
      JS_FreeValue(ctx, e);
      return des_fail(d);
    }
  }
  if (tag != E_END) {
    JS_FreeValue(ctx, e);
    return des_fail(d);
  }
  return e;
}

static int ipc_bytes_per_element(uint32_t idx) {
  switch (idx) {
  case 3: case 4: case 13: return 2;
  case 5: case 6: case 7: return 4;
  case 8: case 11: case 12: return 8;
  default: return 1;
  }
}

static JSValue ipc_read_host(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t tag, idx, len;
  const uint8_t *p;
  JSValue ab, view, args[3];
  int type;
  if (!r_u32(d, &tag))
    return des_fail(d);
  if (tag == IPC_NOT_VIEW) {
    JSValue v = read_value(d);
    if (JS_IsException(v))
      return v;
    if (!JS_IsObject(v)) {
      JS_FreeValue(ctx, v);
      return node_throw_error(ctx, "ERR_INVALID_STATE", "Host object must be an object");
    }
    return v;
  }
  if (tag != IPC_VIEW)
    return node_throw_error(ctx, "ERR_INVALID_STATE", "Invalid host object tag");
  if (!r_u32(d, &idx) || !r_u32(d, &len) || !r_raw(d, len, &p))
    return des_fail(d);
  if (idx == IPC_BUFFER_INDEX)
    return nb_new_buffer(ctx, p, len);
  ab = JS_NewArrayBufferCopy(ctx, p, len);
  if (JS_IsException(ab))
    return ab;
  if (idx == 9) {
    JSValue a[3] = { ab, JS_NewUint32(ctx, 0), JS_NewUint32(ctx, len) };
    view = hcall(ctx, d->h, "newDataView", 3, a);
    JS_FreeValue(ctx, ab);
    return view;
  }
  switch (idx) {
  case 0: type = JS_TYPED_ARRAY_INT8; break;
  case 1: type = JS_TYPED_ARRAY_UINT8; break;
  case 2: type = JS_TYPED_ARRAY_UINT8C; break;
  case 3: type = JS_TYPED_ARRAY_INT16; break;
  case 4: type = JS_TYPED_ARRAY_UINT16; break;
  case 5: type = JS_TYPED_ARRAY_INT32; break;
  case 6: type = JS_TYPED_ARRAY_UINT32; break;
  case 7: type = JS_TYPED_ARRAY_FLOAT32; break;
  case 8: type = JS_TYPED_ARRAY_FLOAT64; break;
  case 11: type = JS_TYPED_ARRAY_BIG_INT64; break;
  case 12: type = JS_TYPED_ARRAY_BIG_UINT64; break;
  case 13: type = JS_TYPED_ARRAY_FLOAT16; break;
  default:
    JS_FreeValue(ctx, ab);
    return node_throw_error(ctx, "ERR_INVALID_STATE", "Invalid host object type index");
  }
  args[0] = ab;
  args[1] = JS_NewUint32(ctx, 0);
  args[2] = JS_NewUint32(ctx, len / ipc_bytes_per_element(idx));
  view = JS_NewTypedArray(ctx, 3, (JSValueConst *)args, type);
  JS_FreeValue(ctx, ab);
  return view;
}

static JSValue read_host_object(DesState *d) {
  JSContext *ctx = d->ctx;
  uint32_t id = d->next_id++;
  JSValue f, r;
  if (d->ipc) {
    r = ipc_read_host(d);
  } else {
    f = JS_IsObject(d->self) ? JS_GetPropertyStr(ctx, d->self, "_readHostObject") : JS_UNDEFINED;
    if (JS_IsException(f))
      return f;
    if (!JS_IsFunction(ctx, f)) {
      JS_FreeValue(ctx, f);
      return des_fail(d);
    }
    r = JS_Call(ctx, f, d->self, 0, NULL);
    JS_FreeValue(ctx, f);
    if (!JS_IsException(r) && !JS_IsObject(r)) {
      JS_FreeValue(ctx, r);
      return JS_ThrowTypeError(ctx, "readHostObject must return an object");
    }
  }
  if (!JS_IsException(r))
    add_object(d, id, r);
  return r;
}

static JSValue read_internal(DesState *d) {
  JSContext *ctx = d->ctx;
  uint8_t tag;
  if (!r_tag(d, &tag))
    return des_fail(d);
  switch (tag) {
  case T_VERIFY_COUNT: {
    uint32_t n;
    if (!r_u32(d, &n))
      return des_fail(d);
    return read_value(d);
  }
  case T_UNDEFINED: return JS_UNDEFINED;
  case T_NULL: return JS_NULL;
  case T_TRUE: return JS_TRUE;
  case T_FALSE: return JS_FALSE;
  case T_INT32: {
    uint32_t u;
    if (!r_u32(d, &u))
      return des_fail(d);
    return JS_NewInt32(ctx, (int32_t)((u >> 1) ^ (uint32_t)-(int32_t)(u & 1)));
  }
  case T_UINT32: {
    uint32_t u;
    if (!r_u32(d, &u))
      return des_fail(d);
    return JS_NewUint32(ctx, u);
  }
  case T_DOUBLE: {
    double v;
    if (!r_double(d, &v))
      return des_fail(d);
    return JS_NewFloat64(ctx, v);
  }
  case T_BIGINT: return read_bigint(d);
  case T_UTF8: return read_utf8(d);
  case T_ONE_BYTE: return read_one_byte(d);
  case T_TWO_BYTE: return read_two_byte(d);
  case T_REF: {
    uint32_t id;
    if (!r_u32(d, &id) || id >= d->nobjs || JS_IsUndefined(d->objs[id]))
      return des_fail(d);
    return JS_DupValue(ctx, d->objs[id]);
  }
  case T_BEGIN_OBJECT: return read_js_object(d);
  case T_BEGIN_SPARSE: return read_sparse_array(d);
  case T_BEGIN_DENSE: return read_dense_array(d);
  case T_DATE: {
    double t;
    uint32_t id;
    JSValue tv, date;
    if (!r_double(d, &t))
      return des_fail(d);
    id = d->next_id++;
    tv = JS_NewFloat64(ctx, t);
    date = hcall1(ctx, d->h, "newDate", tv);
    if (!JS_IsException(date))
      add_object(d, id, date);
    return date;
  }
  case T_TRUE_OBJECT:
  case T_FALSE_OBJECT:
  case T_NUMBER_OBJECT:
  case T_BIGINT_OBJECT:
  case T_STRING_OBJECT:
    return read_wrapper(d, tag);
  case T_REGEXP: return read_regexp(d);
  case T_BEGIN_MAP: return read_collection(d, true);
  case T_BEGIN_SET: return read_collection(d, false);
  case T_ARRAY_BUFFER: return read_array_buffer(d, false, false);
  case T_RESIZABLE_AB: return read_array_buffer(d, false, true);
  case T_AB_TRANSFER: return read_transferred(d);
  case T_SAB: return read_array_buffer(d, true, false);
  case T_ERROR: return read_error(d);
  case T_HOST_OBJECT: return read_host_object(d);
  default:
    if (d->version < 13) {
      d->pos--;
      return read_host_object(d);
    }
    return des_fail(d);
  }
}

static JSValue read_value(DesState *d) {
  JSValue v;
  uint8_t tag;
  if (++d->depth > max_depth()) {
    d->depth--;
    return JS_ThrowRangeError(d->ctx, "Maximum call stack size exceeded");
  }
  v = read_internal(d);
  if (!JS_IsException(v) && JS_IsArrayBuffer(v) && peek_tag(d, &tag) && tag == T_VIEW) {
    consume_tag(d);
    v = read_view(d, v);
  } else if (!JS_IsException(v) && JS_NodeIsSharedArrayBuffer(v) && peek_tag(d, &tag) &&
             tag == T_VIEW) {
    consume_tag(d);
    v = read_view(d, v);
  }
  d->depth--;
  if (JS_IsException(v))
    return des_fail(d);
  return v;
}

static bool read_header(DesState *d) {
  if (d->pos < d->len && d->data[d->pos] == T_VERSION) {
    d->pos++;
    if (!r_u32(d, &d->version) || d->version > LATEST_VERSION) {
      JS_ThrowPlainError(d->ctx,
                         "Unable to deserialize cloned data due to invalid or unsupported version.");
      return false;
    }
  }
  return true;
}

/* ---------------------------------------------------------------------- */
/* internalBinding('serdes'): Serializer and Deserializer */

static JSClassID serializer_class_id, deserializer_class_id;

static void serializer_finalizer(JSRuntime *rt, JSValueConst val) {
  SerState *s = JS_GetOpaque(val, serializer_class_id);
  if (s) {
    ser_free_state(rt, s);
    free(s);
  }
}

static void serializer_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  SerState *s = JS_GetOpaque(val, serializer_class_id);
  if (s)
    ser_mark(rt, s, mark);
}

static void deserializer_finalizer(JSRuntime *rt, JSValueConst val) {
  DesState *d = JS_GetOpaque(val, deserializer_class_id);
  if (d) {
    des_free_state(rt, d);
    free(d);
  }
}

static void deserializer_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  DesState *d = JS_GetOpaque(val, deserializer_class_id);
  if (d)
    des_mark(rt, d, mark);
}

static JSValue serializer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj;
  SerState *s;
  if (JS_IsUndefined(nt))
    return node_throw_type_error(ctx, "ERR_CONSTRUCT_CALL_REQUIRED",
                                 "Class constructor Serializer cannot be invoked without 'new'");
  obj = nb_new_instance(ctx, nt, serializer_class_id);
  if (JS_IsException(obj))
    return obj;
  s = calloc(1, sizeof(*s));
  s->self = JS_UNDEFINED;
  s->buffer_ctor = JS_UNDEFINED;
  s->h = JS_UNDEFINED;
  JS_SetOpaque(obj, s);
  return obj;
}

static SerState *ser_of(JSContext *ctx, JSValueConst v) {
  SerState *s = JS_GetOpaque2(ctx, v, serializer_class_id);
  if (s)
    s->ctx = ctx;
  return s;
}

static JSValue ser_write_header(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  if (!s)
    return JS_EXCEPTION;
  if (w_byte(s, T_VERSION) < 0 || w_varint(s, LATEST_VERSION) < 0)
    return JS_EXCEPTION;
  return JS_UNDEFINED;
}

static JSValue ser_write_value(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  JSValueConst prev_self;
  JSValue prev_h;
  int r;
  if (!s)
    return JS_EXCEPTION;
  prev_self = s->self;
  prev_h = s->h;
  s->self = this_val;
  s->h = get_helpers(ctx);
  if (JS_IsException(s->h)) {
    s->h = prev_h;
    s->self = prev_self;
    return JS_EXCEPTION;
  }
  r = write_value(s, ARG(0));
  JS_FreeValue(ctx, s->h);
  s->h = prev_h;
  s->self = prev_self;
  return r < 0 ? JS_EXCEPTION : JS_TRUE;
}

static JSValue ser_release_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  uint8_t *b;
  size_t n;
  if (!s)
    return JS_EXCEPTION;
  b = s->buf;
  n = s->len;
  s->buf = NULL;
  s->len = s->cap = 0;
  return nb_new_buffer_owned(ctx, b, n);
}

static JSValue ser_transfer_array_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  uint32_t id;
  if (!s)
    return JS_EXCEPTION;
  if (JS_ToUint32(ctx, &id, ARG(0)) < 0)
    return JS_EXCEPTION;
  if (!JS_IsArrayBuffer(ARG(1)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "arrayBuffer must be an ArrayBuffer");
  s->xfer = realloc(s->xfer, sizeof(Transfer) * (s->nxfer + 1));
  s->xfer[s->nxfer].id = id;
  s->xfer[s->nxfer].ab = JS_DupValue(ctx, ARG(1));
  s->nxfer++;
  return JS_UNDEFINED;
}

static JSValue ser_write_uint32(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  uint32_t v;
  if (!s || JS_ToUint32(ctx, &v, ARG(0)) < 0 || w_varint(s, v) < 0)
    return JS_EXCEPTION;
  return JS_UNDEFINED;
}

static JSValue ser_write_uint64(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  uint32_t hi, lo;
  if (!s || JS_ToUint32(ctx, &hi, ARG(0)) < 0 || JS_ToUint32(ctx, &lo, ARG(1)) < 0 ||
      w_varint(s, ((uint64_t)hi << 32) | lo) < 0)
    return JS_EXCEPTION;
  return JS_UNDEFINED;
}

static JSValue ser_write_double(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  double d;
  if (!s || JS_ToFloat64(ctx, &d, ARG(0)) < 0 || w_double(s, d) < 0)
    return JS_EXCEPTION;
  return JS_UNDEFINED;
}

static JSValue ser_write_raw_bytes(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  size_t len;
  uint8_t *p;
  if (!s)
    return JS_EXCEPTION;
  if (!JS_NodeIsArrayBufferView(ARG(0)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "source must be a TypedArray or a DataView");
  p = JS_NodeGetBufferBytes(ctx, ARG(0), &len);
  if (w_raw(s, p, len) < 0)
    return JS_EXCEPTION;
  return JS_UNDEFINED;
}

static JSValue ser_set_treat_views(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  SerState *s = ser_of(ctx, this_val);
  if (!s)
    return JS_EXCEPTION;
  s->host_views = JS_ToBool(ctx, ARG(0));
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry serializer_proto[] = {
  JS_CFUNC_DEF("writeHeader", 0, ser_write_header),
  JS_CFUNC_DEF("writeValue", 1, ser_write_value),
  JS_CFUNC_DEF("releaseBuffer", 0, ser_release_buffer),
  JS_CFUNC_DEF("transferArrayBuffer", 2, ser_transfer_array_buffer),
  JS_CFUNC_DEF("writeUint32", 1, ser_write_uint32),
  JS_CFUNC_DEF("writeUint64", 2, ser_write_uint64),
  JS_CFUNC_DEF("writeDouble", 1, ser_write_double),
  JS_CFUNC_DEF("writeRawBytes", 1, ser_write_raw_bytes),
  JS_CFUNC_DEF("_setTreatArrayBufferViewsAsHostObjects", 1, ser_set_treat_views),
};

static JSValue deserializer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj;
  DesState *d;
  size_t len;
  uint8_t *p;
  if (JS_IsUndefined(nt))
    return node_throw_type_error(ctx, "ERR_CONSTRUCT_CALL_REQUIRED",
                                 "Class constructor Deserializer cannot be invoked without 'new'");
  if (!JS_NodeIsArrayBufferView(ARG(0)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "buffer must be a TypedArray or a DataView");
  obj = nb_new_instance(ctx, nt, deserializer_class_id);
  if (JS_IsException(obj))
    return obj;
  d = calloc(1, sizeof(*d));
  p = JS_NodeGetBufferBytes(ctx, ARG(0), &len);
  /* a copy: what the reads see cannot change under them */
  d->data = malloc(len ? len : 1);
  if (len)
    memcpy(d->data, p, len);
  d->len = len;
  d->self = JS_UNDEFINED;
  d->h = JS_UNDEFINED;
  JS_SetOpaque(obj, d);
  JS_DefinePropertyValueStr(ctx, obj, "buffer", JS_DupValue(ctx, ARG(0)), JS_PROP_C_W_E);
  return obj;
}

static DesState *des_of(JSContext *ctx, JSValueConst v) {
  DesState *d = JS_GetOpaque2(ctx, v, deserializer_class_id);
  if (d)
    d->ctx = ctx;
  return d;
}

static JSValue des_read_header(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  if (!d)
    return JS_EXCEPTION;
  return read_header(d) ? JS_TRUE : JS_EXCEPTION;
}

static JSValue des_read_value(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  JSValueConst prev_self;
  JSValue prev_h, v;
  if (!d)
    return JS_EXCEPTION;
  prev_self = d->self;
  prev_h = d->h;
  d->self = this_val;
  d->h = get_helpers(ctx);
  if (JS_IsException(d->h)) {
    d->h = prev_h;
    d->self = prev_self;
    return JS_EXCEPTION;
  }
  v = read_value(d);
  JS_FreeValue(ctx, d->h);
  d->h = prev_h;
  d->self = prev_self;
  return v;
}

static JSValue des_transfer_array_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  uint32_t id;
  if (!d)
    return JS_EXCEPTION;
  if (JS_ToUint32(ctx, &id, ARG(0)) < 0)
    return JS_EXCEPTION;
  if (!JS_IsArrayBuffer(ARG(1)) && !JS_NodeIsSharedArrayBuffer(ARG(1)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "arrayBuffer must be an ArrayBuffer or SharedArrayBuffer");
  d->xfer = realloc(d->xfer, sizeof(Transfer) * (d->nxfer + 1));
  d->xfer[d->nxfer].id = id;
  d->xfer[d->nxfer].ab = JS_DupValue(ctx, ARG(1));
  d->nxfer++;
  return JS_UNDEFINED;
}

static JSValue des_get_wire_format_version(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  if (!d)
    return JS_EXCEPTION;
  return JS_NewUint32(ctx, d->version);
}

static JSValue des_read_uint32(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  uint32_t v;
  if (!d)
    return JS_EXCEPTION;
  if (!r_u32(d, &v))
    return JS_ThrowPlainError(ctx, "ReadUint32() failed");
  return JS_NewUint32(ctx, v);
}

static JSValue des_read_uint64(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  uint64_t v;
  JSValue arr;
  if (!d)
    return JS_EXCEPTION;
  if (!r_varint(d, &v, 64))
    return JS_ThrowPlainError(ctx, "ReadUint64() failed");
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewUint32(ctx, (uint32_t)(v >> 32)));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewUint32(ctx, (uint32_t)v));
  return arr;
}

static JSValue des_read_double(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  double v;
  if (!d)
    return JS_EXCEPTION;
  if (!r_double(d, &v))
    return JS_ThrowPlainError(ctx, "ReadDouble() failed");
  return JS_NewFloat64(ctx, v);
}

static JSValue des_read_raw_bytes(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  DesState *d = des_of(ctx, this_val);
  int64_t n;
  size_t at;
  const uint8_t *p;
  if (!d)
    return JS_EXCEPTION;
  if (JS_ToInt64(ctx, &n, ARG(0)) < 0)
    return JS_EXCEPTION;
  at = d->pos;
  if (n < 0 || !r_raw(d, (size_t)n, &p))
    return JS_ThrowPlainError(ctx, "ReadRawBytes() failed");
  return JS_NewUint32(ctx, (uint32_t)at);
}

static const JSCFunctionListEntry deserializer_proto[] = {
  JS_CFUNC_DEF("readHeader", 0, des_read_header),
  JS_CFUNC_DEF("readValue", 0, des_read_value),
  JS_CFUNC_DEF("getWireFormatVersion", 0, des_get_wire_format_version),
  JS_CFUNC_DEF("transferArrayBuffer", 2, des_transfer_array_buffer),
  JS_CFUNC_DEF("readUint32", 0, des_read_uint32),
  JS_CFUNC_DEF("readUint64", 0, des_read_uint64),
  JS_CFUNC_DEF("readDouble", 0, des_read_double),
  JS_CFUNC_DEF("_readRawBytes", 1, des_read_raw_bytes),
};

JSValue binding_init_serdes(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef sdef = { .name = "Serializer", .class_id = &serializer_class_id,
                        .ctor = serializer_ctor, .ctor_length = 0,
                        .finalizer = serializer_finalizer, .gc_mark = serializer_mark,
                        .proto_funcs = serializer_proto,
                        .proto_funcs_count = countof(serializer_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef ddef = { .name = "Deserializer", .class_id = &deserializer_class_id,
                        .ctor = deserializer_ctor, .ctor_length = 1,
                        .finalizer = deserializer_finalizer, .gc_mark = deserializer_mark,
                        .proto_funcs = deserializer_proto,
                        .proto_funcs_count = countof(deserializer_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &sdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &ddef));
  return t;
}

/* ---------------------------------------------------------------------- */
/* internalBinding('ipc_serdes'): child_process's serialization: 'advanced' */

/* serialize(value, Buffer): a big-endian uint32 length, then the data */
static JSValue ipc_serialize(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  SerState s;
  JSValue r;
  uint32_t payload;
  int ok;
  memset(&s, 0, sizeof(s));
  s.ctx = ctx;
  s.ipc = true;
  s.host_views = true;
  s.self = JS_UNDEFINED;
  s.buffer_ctor = ARG(1);
  s.h = get_helpers(ctx);
  if (JS_IsException(s.h))
    return JS_EXCEPTION;
  ok = w_raw(&s, "\0\0\0\0", 4) == 0 && w_byte(&s, T_VERSION) == 0 &&
       w_varint(&s, LATEST_VERSION) == 0 && write_value(&s, ARG(0)) == 0;
  JS_FreeValue(ctx, s.h);
  s.h = JS_UNDEFINED;
  if (!ok) {
    ser_free_state(JS_GetRuntime(ctx), &s);
    return JS_EXCEPTION;
  }
  payload = (uint32_t)(s.len - 4);
  s.buf[0] = payload >> 24;
  s.buf[1] = payload >> 16;
  s.buf[2] = payload >> 8;
  s.buf[3] = payload;
  r = nb_new_buffer_owned(ctx, s.buf, s.len);
  s.buf = NULL;
  ser_free_state(JS_GetRuntime(ctx), &s);
  return r;
}

static JSValue ipc_deserialize(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  DesState d;
  JSValue v;
  size_t len;
  uint8_t *p;
  if (!JS_NodeIsArrayBufferView(ARG(0)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "The \"data\" argument must be an ArrayBufferView");
  memset(&d, 0, sizeof(d));
  d.ctx = ctx;
  d.ipc = true;
  d.self = JS_UNDEFINED;
  p = JS_NodeGetBufferBytes(ctx, ARG(0), &len);
  d.data = malloc(len ? len : 1);
  if (len)
    memcpy(d.data, p, len);
  d.len = len;
  d.h = get_helpers(ctx);
  if (JS_IsException(d.h)) {
    des_free_state(JS_GetRuntime(ctx), &d);
    return JS_EXCEPTION;
  }
  v = read_header(&d) ? read_value(&d) : JS_EXCEPTION;
  JS_FreeValue(ctx, d.h);
  des_free_state(JS_GetRuntime(ctx), &d);
  return v;
}

JSValue binding_init_ipc_serdes(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "serialize", ipc_serialize, 2);
  nb_set_method(ctx, t, "deserialize", ipc_deserialize, 1);
  return t;
}
