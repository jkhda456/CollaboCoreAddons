/* What the guest's libc declares but does not have (no mmap in the guest
 * ABI).  Nothing calls these on a working path: libuv's io_uring setup is
 * off for wasm; they fail like a kernel without the call would. */
#ifdef __wasm__
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  errno = ENOSYS;
  return MAP_FAILED;
}

int munmap(void *addr, size_t len) {
  errno = ENOSYS;
  return -1;
}
#endif
