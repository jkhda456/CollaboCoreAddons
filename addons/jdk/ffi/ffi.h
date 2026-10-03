/*
 * A libffi for wasm32, enough for HotSpot's Zero interpreter (its JNI calls).
 *
 * WebAssembly cannot build a call frame at run time: a call through a function pointer
 * (call_indirect) must name the callee's exact type. So ffi_prep_cif maps the cif to its wasm
 * signature (i32/i64/f32/f64 parameters and result) and picks a call stub generated for that
 * signature at build time (tools/gen-ffi-calls.py: every native method of the class library, and
 * every signature of up to a few parameters); ffi_call goes through it.
 */
#ifndef LIBFFI_H
#define LIBFFI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFI_CLOSURES 0
#define FFI_GO_CLOSURES 0
#define FFI_NATIVE_RAW_API 0

typedef enum { FFI_OK = 0, FFI_BAD_TYPEDEF, FFI_BAD_ABI, FFI_BAD_ARGTYPE } ffi_status;
typedef enum { FFI_FIRST_ABI = 0, FFI_WASM32 = 1, FFI_LAST_ABI, FFI_DEFAULT_ABI = FFI_WASM32 } ffi_abi;

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

typedef struct _ffi_type {
  size_t size;
  unsigned short alignment;
  unsigned short type;
  struct _ffi_type **elements;
} ffi_type;

typedef unsigned long ffi_arg;
typedef signed long ffi_sarg;

typedef void (*ffi_wasm_call_t)(void (*fn)(void), void *rvalue, void **avalue);

typedef struct {
  ffi_abi abi;
  unsigned nargs;
  ffi_type **arg_types;
  ffi_type *rtype;
  unsigned bytes;
  unsigned flags;
  ffi_wasm_call_t call;   /* the call stub for this signature, or null */
  char sig[24];           /* the signature, for the error when there is no stub */
} ffi_cif;

extern ffi_type ffi_type_void;
extern ffi_type ffi_type_uint8, ffi_type_sint8, ffi_type_uint16, ffi_type_sint16;
extern ffi_type ffi_type_uint32, ffi_type_sint32, ffi_type_uint64, ffi_type_sint64;
extern ffi_type ffi_type_float, ffi_type_double, ffi_type_longdouble, ffi_type_pointer;
#define ffi_type_uchar  ffi_type_uint8
#define ffi_type_schar  ffi_type_sint8
#define ffi_type_ushort ffi_type_uint16
#define ffi_type_sshort ffi_type_sint16
#define ffi_type_uint   ffi_type_uint32
#define ffi_type_sint   ffi_type_sint32
#define ffi_type_ulong  ffi_type_uint32
#define ffi_type_slong  ffi_type_sint32

ffi_status ffi_prep_cif(ffi_cif *cif, ffi_abi abi, unsigned int nargs, ffi_type *rtype,
                        ffi_type **atypes);
void ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue);

#define FFI_FN(f) ((void (*)(void))(f))

#ifdef __cplusplus
}
#endif

#endif
