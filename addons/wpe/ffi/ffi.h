/*
 * libffi for wasm32, the part GLib uses (gobject's generic marshaller: g_cclosure_marshal_generic
 * calls a signal handler whose signature it learns at run time).
 *
 * WebAssembly cannot build a call frame at run time: a call_indirect names its signature
 * statically. So ffi_prep_cif() finds, by the call's wasm signature, one of the call stubs
 * gen-calls.py writes (ffi_calls.c): every signature of up to 6 arguments of i32, i64 and f64 (and
 * of up to 3 with f32), and up to 12 arguments of i32 only. A signature without a stub makes
 * ffi_prep_cif() fail (FFI_BAD_TYPEDEF), and ffi_call() of it abort with its name. Structs by
 * value and closures are not supported.
 */
#ifndef WPE_WASM_FFI_H
#define WPE_WASM_FFI_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FFI_TYPE_VOID       0
#define FFI_TYPE_INT        1
#define FFI_TYPE_FLOAT      2
#define FFI_TYPE_DOUBLE     3
#define FFI_TYPE_LONGDOUBLE 4
#define FFI_TYPE_UINT8      5
#define FFI_TYPE_SINT8      6
#define FFI_TYPE_UINT16     7
#define FFI_TYPE_SINT16     8
#define FFI_TYPE_UINT32     9
#define FFI_TYPE_SINT32     10
#define FFI_TYPE_UINT64     11
#define FFI_TYPE_SINT64     12
#define FFI_TYPE_STRUCT     13
#define FFI_TYPE_POINTER    14
#define FFI_TYPE_COMPLEX    15

typedef unsigned long ffi_arg;
typedef signed long ffi_sarg;

typedef struct _ffi_type {
  size_t size;
  unsigned short alignment;
  unsigned short type;
  struct _ffi_type **elements;
} ffi_type;

extern ffi_type ffi_type_void, ffi_type_uint8, ffi_type_sint8, ffi_type_uint16, ffi_type_sint16,
    ffi_type_uint32, ffi_type_sint32, ffi_type_uint64, ffi_type_sint64, ffi_type_float,
    ffi_type_double, ffi_type_longdouble, ffi_type_pointer;
#define ffi_type_uchar  ffi_type_uint8
#define ffi_type_schar  ffi_type_sint8
#define ffi_type_ushort ffi_type_uint16
#define ffi_type_sshort ffi_type_sint16
#define ffi_type_uint   ffi_type_uint32
#define ffi_type_sint   ffi_type_sint32
#define ffi_type_ulong  ffi_type_uint32
#define ffi_type_slong  ffi_type_sint32

typedef enum { FFI_OK = 0, FFI_BAD_TYPEDEF, FFI_BAD_ABI, FFI_BAD_ARGTYPE } ffi_status;
typedef enum { FFI_FIRST_ABI = 0, FFI_WASM32 = 1, FFI_DEFAULT_ABI = FFI_WASM32, FFI_LAST_ABI } ffi_abi;

typedef void (*ffi_wasm_call_t)(void (*fn)(void), void *rvalue, void **avalue);

typedef struct {
  ffi_abi abi;
  unsigned nargs;
  ffi_type **arg_types;
  ffi_type *rtype;
  unsigned bytes;
  unsigned flags;
  ffi_wasm_call_t call;
  char sig[24];
} ffi_cif;

ffi_status ffi_prep_cif(ffi_cif *cif, ffi_abi abi, unsigned int nargs, ffi_type *rtype, ffi_type **atypes);
ffi_status ffi_prep_cif_var(ffi_cif *cif, ffi_abi abi, unsigned int nfixedargs, unsigned int ntotalargs,
                            ffi_type *rtype, ffi_type **atypes);
void ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue);

/* wasm only: the signature ("r_pp...": i i32, j i64, f f32, d f64, v no result) of the function a
 * pointer names, read from the program's own function table; NULL if unknown (signatures.c) */
const char *ffi_wasm_signature(void (*fn)(void));
/* Calls fn with the arguments of signature sig (avalue: pointers to them) as what fn really
 * takes: extra arguments left out, missing ones zero; the result as sig has it (rvalue may be
 * NULL). For calls through a pointer of another type than the function's. */
void ffi_wasm_call(void (*fn)(void), const char *sig, void *rvalue, void **avalue);
#define FFI_FN(f) ((void (*)(void))(f))

/* closures: not supported on wasm (no code at run time) */
typedef struct {
  ffi_cif *cif;
  void (*fun)(ffi_cif *, void *, void **, void *);
  void *user_data;
} ffi_closure;
void *ffi_closure_alloc(size_t size, void **code);
void ffi_closure_free(void *closure);
ffi_status ffi_prep_closure_loc(ffi_closure *closure, ffi_cif *cif,
                                void (*fun)(ffi_cif *, void *, void **, void *), void *user_data, void *codeloc);
#define FFI_CLOSURES 0

#ifdef __cplusplus
}
#endif
#endif
