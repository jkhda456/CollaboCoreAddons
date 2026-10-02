# bash for the guest

`tools/fetch-bash.sh` unpacks GNU bash 5.3 with the official patches 001–009 and applies the
wasm Linux port from `third_party/distro/distro/bash` (tombl's distro, the same source
`nettools/` takes curl, dropbear and git patches from). `tools/build-bash.sh` cross-builds it
with `userspace/bin/wasm-cc` into `build/bash/bash.wasm`.

How the port runs children without fork(): every `make_child()` caller hands bash a child
continuation (a function plus a heap-allocated argument block) instead of returning twice;
`make_child()` starts it with the kernel's callback `clone(fn, stack, SIGCHLD, arg)`, which
gives the child a private copy of the parent's memory and enters `fn` on a fresh 8 MiB stack.
So subshells, command substitution, pipeline elements, `&` jobs, coprocs and process
substitution all run bash's own code with the parent's full state, as after a fork. A
`fork()` that is still reached is a loud trap (`fork-shim.c`), never a silent failure.

The one build difference from the distro: the guest's base image has no `/dev/fd` link
(devtmpfs is mounted over `/dev` at boot), so process substitution names `/proc/self/fd/N`
(`bash_cv_dev_fd=whacky`).

Patches of our own go here as `NNNN-*.patch` (`-p1`); `fetch-bash.sh` applies them after the
distro's. There are none at the moment.
