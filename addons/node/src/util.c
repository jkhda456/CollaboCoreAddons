/* Small helpers shared by the bindings. */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

void nb_set_method(JSContext *ctx, JSValueConst obj, const char *name,
                   JSCFunction *fn, int length) {
  JS_DefinePropertyValueStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, length),
                            JS_PROP_C_W_E);
}

void nb_set(JSContext *ctx, JSValueConst obj, const char *name, JSValue v) {
  JS_DefinePropertyValueStr(ctx, obj, name, v, JS_PROP_C_W_E);
}

void nb_set_int(JSContext *ctx, JSValueConst obj, const char *name, int64_t v) {
  nb_set(ctx, obj, name, JS_NewInt64(ctx, v));
}

void nb_set_double(JSContext *ctx, JSValueConst obj, const char *name, double v) {
  nb_set(ctx, obj, name, JS_NewFloat64(ctx, v));
}

void nb_set_str(JSContext *ctx, JSValueConst obj, const char *name, const char *v) {
  nb_set(ctx, obj, name, JS_NewString(ctx, v));
}

void nb_set_bool(JSContext *ctx, JSValueConst obj, const char *name, bool v) {
  nb_set(ctx, obj, name, JS_NewBool(ctx, v));
}

void nb_define_readonly(JSContext *ctx, JSValueConst obj, const char *name, JSValue v) {
  JS_DefinePropertyValueStr(ctx, obj, name, v, JS_PROP_ENUMERABLE);
}

/* a typed array over memory the caller keeps (never freed by QuickJS) */
JSValue nb_new_typed_array(JSContext *ctx, JSTypedArrayEnum type, void *data,
                           size_t count, size_t elem_size) {
  JSValue ab = JS_NewArrayBuffer(ctx, data, count * elem_size, 0, NULL, NULL, false);
  JSValue args[1], ta;
  if (JS_IsException(ab))
    return ab;
  args[0] = ab;
  ta = JS_NewTypedArray(ctx, 1, (JSValueConst *)args, type);
  JS_FreeValue(ctx, ab);
  return ta;
}

void *nb_alloc_shared_array(JSContext *ctx, JSTypedArrayEnum type, size_t count,
                            size_t elem_size, JSValue *out) {
  void *p = calloc(count ? count : 1, elem_size);
  *out = nb_new_typed_array(ctx, type, p, count, elem_size);
  return p;
}

char *nb_to_cstring_dup(JSContext *ctx, JSValueConst v) {
  size_t len;
  const char *s = JS_ToCStringLen(ctx, &len, v);
  char *r;
  if (!s)
    return NULL;
  r = malloc(len + 1);
  memcpy(r, s, len + 1);
  JS_FreeCString(ctx, s);
  return r;
}

int64_t nb_int64(JSContext *ctx, JSValueConst v, int64_t def) {
  int64_t r;
  if (JS_IsUndefined(v))
    return def;
  if (JS_IsBigInt(v)) {
    if (JS_ToBigInt64(ctx, &r, v) < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return def;
    }
    return r;
  }
  if (JS_ToInt64(ctx, &r, v) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return def;
  }
  return r;
}

int32_t nb_int32(JSContext *ctx, JSValueConst v, int32_t def) {
  int32_t r;
  if (JS_IsUndefined(v))
    return def;
  if (JS_ToInt32(ctx, &r, v) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return def;
  }
  return r;
}

uint32_t nb_uint32(JSContext *ctx, JSValueConst v, uint32_t def) {
  uint32_t r;
  if (JS_IsUndefined(v))
    return def;
  if (JS_ToUint32(ctx, &r, v) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return def;
  }
  return r;
}

double nb_double(JSContext *ctx, JSValueConst v, double def) {
  double r;
  if (JS_IsUndefined(v))
    return def;
  if (JS_ToFloat64(ctx, &r, v) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return def;
  }
  return r;
}

int nb_to_int64(JSContext *ctx, int64_t *pres, JSValueConst v) {
  if (JS_IsBigInt(v))
    return JS_ToBigInt64(ctx, pres, v);
  return JS_ToInt64(ctx, pres, v);
}

bool nb_is_array_buffer_view(JSContext *ctx, JSValueConst v) {
  return JS_NodeIsArrayBufferView(v);
}

uint8_t *nb_buffer_data(JSContext *ctx, JSValueConst v, size_t *plen) {
  return JS_NodeGetBufferBytes(ctx, v, plen);
}

static void free_ab(JSRuntime *rt, void *opaque, void *ptr) {
  free(ptr);
}

static void *realloc_ab(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

JSValue node_buffer_prototype(Env *env) {
  return env->buffer_prototype;
}

/* a Buffer (a Uint8Array with Node's FastBuffer prototype) owning data */
JSValue nb_new_buffer_owned(JSContext *ctx, void *data, size_t len) {
  Env *env = env_get(ctx);
  JSValue u8;
  (void)free_ab;
  if (!data)
    data = malloc(1);
  u8 = JS_NewUint8Array(ctx, data, len, realloc_ab, NULL, false);
  if (JS_IsException(u8)) {
    free(data);
    return u8;
  }
  if (env && JS_IsObject(env->buffer_prototype))
    JS_SetPrototype(ctx, u8, env->buffer_prototype);
  return u8;
}

JSValue nb_new_buffer(JSContext *ctx, const void *data, size_t len) {
  void *p = malloc(len ? len : 1);
  if (!p)
    return JS_ThrowOutOfMemory(ctx);
  if (len)
    memcpy(p, data, len);
  return nb_new_buffer_owned(ctx, p, len);
}

JSValue nb_new_array_buffer_copy(JSContext *ctx, const void *data, size_t len) {
  return JS_NewArrayBufferCopy(ctx, data, len);
}

JSValue nb_new_uint8_array_copy(JSContext *ctx, const void *data, size_t len) {
  return JS_NewUint8ArrayCopy(ctx, data, len);
}

JSValue nb_call(JSContext *ctx, JSValueConst fn, JSValueConst this_val, int argc,
                JSValueConst *argv) {
  return JS_Call(ctx, fn, this_val, argc, argv);
}

JSValue nb_get(JSContext *ctx, JSValueConst obj, const char *name) {
  return JS_GetPropertyStr(ctx, obj, name);
}

JSValue nb_new_string_utf8(JSContext *ctx, const char *s, size_t len) {
  return node_new_utf8_string(ctx, (const uint8_t *)s, len);
}

JSValue nb_array_from_strings(JSContext *ctx, char **strs, int n) {
  JSValue arr = JS_NewArray(ctx);
  int i;
  for (i = 0; i < n; i++)
    JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, strs[i]));
  return arr;
}

uint64_t nb_hrtime(void) {
  return uv_hrtime();
}

/* ---------------------------------------------------------------------- */
/* classes */

JSValue nb_new_instance(JSContext *ctx, JSValueConst new_target, JSClassID id) {
  JSValue proto, obj;
  if (JS_IsUndefined(new_target))
    return JS_ThrowTypeError(ctx, "Class constructor cannot be invoked without 'new'");
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(proto))
    return proto;
  if (!JS_IsObject(proto)) {
    JS_FreeValue(ctx, proto);
    proto = JS_GetClassProto(ctx, id);
  }
  obj = JS_NewObjectProtoClass(ctx, proto, id);
  JS_FreeValue(ctx, proto);
  return obj;
}

JSValue nb_define_class(JSContext *ctx, JSValueConst target, const NodeClassDef *def) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  JSValue proto, ctor;
  if (def->class_id) {
    /* class ids are process-wide; each runtime (worker) registers its own */
    JS_NewClassID(rt, def->class_id);
    if (!JS_IsRegisteredClass(rt, *def->class_id)) {
      JSClassDef cd;
      memset(&cd, 0, sizeof(cd));
      cd.class_name = def->name;
      cd.finalizer = def->finalizer;
      cd.gc_mark = def->gc_mark;
      JS_NewClass(rt, *def->class_id, &cd);
    }
  }
  if (!JS_IsUndefined(def->parent_ctor) && JS_IsObject(def->parent_ctor)) {
    JSValue parent_proto = JS_GetPropertyStr(ctx, def->parent_ctor, "prototype");
    proto = JS_NewObjectProto(ctx, parent_proto);
    JS_FreeValue(ctx, parent_proto);
  } else {
    proto = JS_NewObject(ctx);
  }
  if (def->proto_funcs_count)
    JS_SetPropertyFunctionList(ctx, proto, def->proto_funcs, def->proto_funcs_count);
  ctor = JS_NewCFunction2(ctx, def->ctor, def->name, def->ctor_length,
                          JS_CFUNC_constructor_or_func, 0);
  JS_SetConstructor(ctx, ctor, proto);
  if (!JS_IsUndefined(def->parent_ctor) && JS_IsObject(def->parent_ctor))
    JS_SetPrototype(ctx, ctor, def->parent_ctor);
  if (def->static_funcs_count)
    JS_SetPropertyFunctionList(ctx, ctor, def->static_funcs, def->static_funcs_count);
  if (def->class_id)
    JS_SetClassProto(ctx, *def->class_id, JS_DupValue(ctx, proto));
  JS_FreeValue(ctx, proto);
  if (!JS_IsUndefined(target))
    JS_DefinePropertyValueStr(ctx, target, def->name, JS_DupValue(ctx, ctor),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  return ctor;
}

JSValue nb_construct(JSContext *ctx, JSValueConst ctor, int argc, JSValueConst *argv) {
  return JS_CallConstructor(ctx, ctor, argc, argv);
}
