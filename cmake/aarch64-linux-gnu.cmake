# Cross-compiles for Linux arm64 (aarch64) from an x86-64 Debian/Ubuntu host:
#   apt install g++-aarch64-linux-gnu            (or g++-12-aarch64-linux-gnu: -DKESTRO_CROSS_GCC_SUFFIX=-12)
#   cmake -S . -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
# OpenSSL for the target comes from the multiarch package (dpkg --add-architecture arm64;
# apt install libssl-dev:arm64). Its headers are split between /usr/include/openssl and
# /usr/include/aarch64-linux-gnu/openssl, and the host's /usr/include must not be put on the
# cross compiler's path (it holds x86-64 glibc headers): give a merged copy with
# -DOPENSSL_INCLUDE_DIR=DIR (packaging/linux-builder/Dockerfile makes /opt/arm64-openssl/include).
# Tests run under qemu-user (apt install qemu-user-static) through CMAKE_CROSSCOMPILING_EMULATOR.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(KESTRO_CROSS_TRIPLE aarch64-linux-gnu CACHE STRING "Cross toolchain prefix")
set(KESTRO_CROSS_GCC_SUFFIX "" CACHE STRING "Compiler version suffix, for example -12")
set(CMAKE_C_COMPILER ${KESTRO_CROSS_TRIPLE}-gcc${KESTRO_CROSS_GCC_SUFFIX})
set(CMAKE_CXX_COMPILER ${KESTRO_CROSS_TRIPLE}-g++${KESTRO_CROSS_GCC_SUFFIX})
set(CMAKE_ASM_COMPILER ${KESTRO_CROSS_TRIPLE}-gcc${KESTRO_CROSS_GCC_SUFFIX})
set(CMAKE_LIBRARY_ARCHITECTURE ${KESTRO_CROSS_TRIPLE})
# Libraries: the cross toolchain's own (/usr/aarch64-linux-gnu) and multiarch ones (/usr/lib/aarch64-linux-gnu).
set(CMAKE_FIND_ROOT_PATH /usr/${KESTRO_CROSS_TRIPLE} /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
# The host's pkg-config would point FindOpenSSL at the x86-64 libraries.
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/${KESTRO_CROSS_TRIPLE}/pkgconfig)
find_program(KESTRO_QEMU NAMES qemu-aarch64-static qemu-aarch64)
if(KESTRO_QEMU)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${KESTRO_QEMU} -L /usr/${KESTRO_CROSS_TRIPLE})
endif()
