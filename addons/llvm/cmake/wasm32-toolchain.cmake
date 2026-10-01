# CMake toolchain for the cross stage of build.sh: programs for the guest (wasm32 Linux, musl),
# compiled by the host's clang-19. build.sh passes the compile and link flags itself (the ones
# userspace/bin/wasm-cc uses); this file names the tools and keeps CMake's checks and searches
# away from the build machine.
#
#   -DWASM_SYSROOT=userspace/sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(CMAKE_C_COMPILER   /usr/bin/clang-19)
set(CMAKE_CXX_COMPILER /usr/bin/clang++-19)
set(CMAKE_ASM_COMPILER /usr/bin/clang-19)
set(CMAKE_AR     /usr/bin/llvm-ar-19)
set(CMAKE_RANLIB /usr/bin/llvm-ranlib-19)
set(CMAKE_NM     /usr/bin/llvm-nm-19)

# Checks build static libraries, so they compile but never link: a link check cannot fail.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Programs (tblgen, python) come from the build machine; headers and libraries from the sysroot.
set(CMAKE_FIND_ROOT_PATH "${WASM_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
