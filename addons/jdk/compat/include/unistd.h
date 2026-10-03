/* <unistd.h> for the JDK on the guest: musl's, plus fork/vfork/daemon, which it leaves out on wasm.
 * They fail with ENOSYS (wasm_compat.c); processes are started with posix_spawn. */
#ifndef JDK_WASM_UNISTD_H
#define JDK_WASM_UNISTD_H
#include_next <unistd.h>
#ifdef __wasm__
#ifdef __cplusplus
extern "C" {
#endif
pid_t fork(void);
pid_t vfork(void);
int daemon(int, int);
#ifdef __cplusplus
}
#endif
#endif
#endif
