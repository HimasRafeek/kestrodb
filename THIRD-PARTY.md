# Third-party components

KestroDB's own code is under the Apache License 2.0 (LICENSE, NOTICE).

BLAKE3 1.8.7 C implementation and assembly are vendored under vendor/blake3.
Its accompanying Apache 2.0 / Apache 2.0 with LLVM exception license files are
included. The hash vectors in src/blake3_vectors.inc derive from the upstream
BLAKE3 test vectors (https://github.com/BLAKE3-team/BLAKE3/blob/1.8.7/test_vectors/test_vectors.json).

The Node-API headers under vendor/node-api/include (js_native_api.h,
js_native_api_types.h, node_api.h, node_api_types.h) are copied unmodified from
Node.js 24.14.0 and are used under the MIT license in
third-party-notices/node-api-headers-LICENSE.txt. cmake/node_api.def lists the
Node-API function names the addon imports.

Binaries are not committed to the repository: `npm run build` writes them to bin/, and
the prebuilt ones are published as GitHub release assets and in the npm package.

The released Windows x64 binaries are built with Microsoft Visual C++ 2022 and link the
Microsoft C/C++ runtime statically. They import only Windows system DLLs (such as
bcrypt, Kernel32 and WS2_32), plus node.exe for kestro.node. Builds made with MinGW-w64
instead statically link GCC and MinGW runtime libraries; the GCC runtime exception
(third-party-notices/gcc-runtime-copyright.txt) and the MinGW license/copyright notices
(third-party-notices/mingw-w64-copyright.txt) cover those builds.

macOS builds (from source) use CommonCrypto from the SDK.

The Linux release packages (linux-x64 and linux-arm64 archives, the npm package's
prebuilds/linux-*, the manylinux wheels and the Composer package) are built with GCC 12 on
Ubuntu 22.04 (scripts/build-linux.sh). They link the GCC C++ runtime (libstdc++) statically,
under the GCC Runtime Library Exception (third-party-notices/gcc-runtime-copyright.txt).
kestro.node, libkestrodb.so and kestro-worker also link OpenSSL 3.0's libcrypto statically
(Apache License 2.0, third-party-notices/openssl-LICENSE.txt); kestro-server uses the
system's OpenSSL 3 shared libraries and includes no OpenSSL code. libgcc_s and glibc stay
shared system libraries.

The benchmark's comparison databases (bench/competitors: better-sqlite3, lmdb,
classic-level and their dependencies) are installed by npm from the public registry
under their own licenses. They are not part of KestroDB and are not distributed with it.
