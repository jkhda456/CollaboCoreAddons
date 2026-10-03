/* libffi for wasm32: see ffi.h. The call stubs and their table are in ffi_calls.c (generated). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffi.h"

#define T(name, sz, al, ty) ffi_type ffi_type_##name = { sz, al, ty, 0 };
T(void, 1, 1, FFI_TYPE_VOID)
T(uint8, 1, 1, FFI_TYPE_UINT8)
T(sint8, 1, 1, FFI_TYPE_SINT8)
T(uint16, 2, 2, FFI_TYPE_UINT16)
T(sint16, 2, 2, FFI_TYPE_SINT16)
T(uint32, 4, 4, FFI_TYPE_UINT32)
T(sint32, 4, 4, FFI_TYPE_SINT32)
T(uint64, 8, 8, FFI_TYPE_UINT64)
T(sint64, 8, 8, FFI_TYPE_SINT64)
T(float, 4, 4, FFI_TYPE_FLOAT)
T(double, 8, 8, FFI_TYPE_DOUBLE)
T(longdouble, 16, 16, FFI_TYPE_LONGDOUBLE)
T(pointer, 4, 4, FFI_TYPE_POINTER)
#undef T

struct ffi_wasm_stub { const char *sig; ffi_wasm_call_t call; };
/* sorted by sig (strcmp); in ffi_calls.c */
extern const struct ffi_wasm_stub ffi_wasm_stubs[];
extern const unsigned ffi_wasm_stub_count;

/* the wasm value type of an argument or result: i (i32), j (i64), f (f32), d (f64), v (none) */
static int wasm_type(const ffi_type *t) {
  switch (t->type) {
  case FFI_TYPE_VOID: return 'v';
  case FFI_TYPE_INT: case FFI_TYPE_UINT8: case FFI_TYPE_SINT8: case FFI_TYPE_UINT16:
  case FFI_TYPE_SINT16: case FFI_TYPE_UINT32: case FFI_TYPE_SINT32: case FFI_TYPE_POINTER:
    return 'i';
  case FFI_TYPE_UINT64: case FFI_TYPE_SINT64: return 'j';
  case FFI_TYPE_FLOAT: return 'f';
  case FFI_TYPE_DOUBLE: return 'd';
  default: return 0;
  }
}

static ffi_wasm_call_t find_stub(const char *sig) {
  unsigned lo = 0, hi = ffi_wasm_stub_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    int c = strcmp(ffi_wasm_stubs[mid].sig, sig);
    if (c == 0) return ffi_wasm_stubs[mid].call;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return 0;
}

ffi_status ffi_prep_cif(ffi_cif *cif, ffi_abi abi, unsigned int nargs, ffi_type *rtype,
                        ffi_type **atypes) {
  char sig[300];
  unsigned n = 0, i;
  if (abi != FFI_DEFAULT_ABI) return FFI_BAD_ABI;
  if (nargs > 256) return FFI_BAD_ARGTYPE;
  cif->abi = abi;
  cif->nargs = nargs;
  cif->arg_types = atypes;
  cif->rtype = rtype;
  cif->bytes = 0;
  cif->flags = 0;
  if (!(sig[n++] = wasm_type(rtype))) return FFI_BAD_TYPEDEF;
  sig[n++] = '_';
  for (i = 0; i < nargs; i++) {
    int t = wasm_type(atypes[i]);
    if (!t || t == 'v') return FFI_BAD_TYPEDEF;
    sig[n++] = t;
  }
  sig[n] = 0;
  cif->call = find_stub(sig);
  snprintf(cif->sig, sizeof cif->sig, "%s", sig);
  return FFI_OK;
}

void ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue) {
  if (!cif->call) {
    fprintf(stderr, "libffi (wasm32): no call stub for the signature %s%s\n", cif->sig,
            strlen(cif->sig) >= sizeof cif->sig - 1 ? "..." : "");
    abort();
  }
  cif->call(fn, rvalue, avalue);
}
