/* libffi for wasm32: see ffi.h. The call stubs and their table are in ffi_calls.c (gen-calls.py). */
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
  char sig[sizeof cif->sig];
  unsigned n = 0, i;
  if (abi != FFI_DEFAULT_ABI) return FFI_BAD_ABI;
  if (nargs + 3 > sizeof sig) return FFI_BAD_ARGTYPE;
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
  memcpy(cif->sig, sig, n + 1);
  cif->call = find_stub(sig);
  return cif->call ? FFI_OK : FFI_BAD_TYPEDEF;
}

ffi_status ffi_prep_cif_var(ffi_cif *cif, ffi_abi abi, unsigned int nfixedargs, unsigned int ntotalargs,
                            ffi_type *rtype, ffi_type **atypes) {
  (void)nfixedargs;  /* variadic calls pass their arguments in memory on wasm: not supported */
  (void)cif; (void)abi; (void)ntotalargs; (void)rtype; (void)atypes;
  return FFI_BAD_ABI;
}

/*
 * Calls fn, whose arguments for the wasm signature `sig` are avalue (pointers to the values) and
 * whose result goes to rvalue (for a result in sig; may be NULL), as what fn really is
 * (ffi_wasm_signature): extra arguments are left out, missing ones are zero, a result fn does
 * not have is zero. A cast-free call when the two agree.
 */
void ffi_wasm_call(void (*fn)(void), const char *sig, void *rvalue, void **avalue) {
  const char *real = ffi_wasm_signature(fn);
  if (!real || !strcmp(real, sig)) {
    ffi_wasm_call_t call = find_stub(sig);
    if (!call) {
      fprintf(stderr, "ffi: no call stub for the wasm signature %s (addons/wpe/ffi/gen-calls.py)\n", sig);
      abort();
    }
    uint64_t ignored;
    call(fn, rvalue ? rvalue : &ignored, avalue);
    return;
  }
  ffi_wasm_call_t call = find_stub(real);
  if (!call) {
    fprintf(stderr, "ffi: no call stub for %s (called as %s; addons/wpe/ffi/gen-calls.py)\n", real, sig);
    abort();
  }
  /* the arguments fn takes, from those given where the type agrees */
  static const uint64_t zero[2];
  void *args[24];
  uint64_t widened[24];
  const char *have = strchr(sig, '_') + 1, *want = strchr(real, '_') + 1;
  size_t n_have = strlen(have), n_want = strlen(want);
  if (n_want > 24) abort();
  for (size_t k = 0; k < n_want; k++) {
    if (k < n_have && have[k] == want[k])
      args[k] = avalue[k];
    else if (k < n_have && have[k] == 'i' && want[k] == 'j') {
      widened[k] = *(uint32_t *)avalue[k];
      args[k] = &widened[k];
    } else if (k < n_have && have[k] == 'j' && want[k] == 'i')
      args[k] = avalue[k]; /* little-endian: the low half */
    else
      args[k] = (void *)zero;
  }
  uint64_t result = 0;
  call(fn, &result, args);
  if (rvalue && sig[0] != 'v') {
    switch (sig[0]) {
    case 'i': *(uint32_t *)rvalue = real[0] == 'v' ? 0 : (uint32_t)result; break;
    case 'j': *(uint64_t *)rvalue = real[0] == 'v' ? 0 : real[0] == 'i' ? (uint64_t)(uint32_t)result : result; break;
    case 'f': *(float *)rvalue = real[0] == 'f' ? *(float *)&result : 0; break;
    case 'd': *(double *)rvalue = real[0] == 'd' ? *(double *)&result : 0; break;
    }
  }
}

void ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue) {
  if (!cif->call) {
    fprintf(stderr, "ffi_call: no call stub for the wasm signature %s (addons/wpe/ffi/gen-calls.py)\n", cif->sig);
    abort();
  }
  /* the function may take another signature than the call's (a signal handler that leaves out
   * parameters): called as it is */
  const char *real = ffi_wasm_signature(fn);
  if (real && strcmp(real, cif->sig)) {
    ffi_arg wide_result = 0;
    int small = cif->rtype->type != FFI_TYPE_VOID && cif->rtype->size < sizeof(ffi_arg);
    ffi_wasm_call(fn, cif->sig, small ? (void *)&wide_result : rvalue, avalue);
    if (small)
      *(ffi_arg *)rvalue = wide_result;
    return;
  }
  /* small integer results are widened to ffi_arg, as libffi does */
  ffi_arg wide;
  void *rv = rvalue;
  if (cif->rtype->type != FFI_TYPE_VOID && cif->rtype->size < sizeof(ffi_arg)) rv = &wide;
  cif->call(fn, rv, avalue);
  if (rv == &wide) {
    switch (cif->rtype->type) {
    case FFI_TYPE_SINT8: *(ffi_sarg *)rvalue = (int8_t)wide; break;
    case FFI_TYPE_UINT8: *(ffi_arg *)rvalue = (uint8_t)wide; break;
    case FFI_TYPE_SINT16: *(ffi_sarg *)rvalue = (int16_t)wide; break;
    case FFI_TYPE_UINT16: *(ffi_arg *)rvalue = (uint16_t)wide; break;
    default: *(ffi_arg *)rvalue = wide; break;
    }
  }
}

void *ffi_closure_alloc(size_t size, void **code) { (void)size; if (code) *code = 0; return 0; }
void ffi_closure_free(void *closure) { (void)closure; }
ffi_status ffi_prep_closure_loc(ffi_closure *closure, ffi_cif *cif,
                                void (*fun)(ffi_cif *, void *, void **, void *), void *user_data, void *codeloc) {
  (void)closure; (void)cif; (void)fun; (void)user_data; (void)codeloc;
  return FFI_BAD_ABI;
}
