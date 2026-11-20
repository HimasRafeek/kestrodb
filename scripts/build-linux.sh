#!/usr/bin/env bash
# Linux release binaries for linux-x64 and linux-arm64 (cross-compiled), built in the
# packaging/linux-builder image (Ubuntu 22.04: the binaries need glibc 2.34+):
#   docker build -t kestrodb-linux-builder packaging/linux-builder
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src kestrodb-linux-builder bash scripts/build-linux.sh [x64] [arm64]
# Output: dist/build/linux-<arch>/bin (kestro.node, libkestrodb.so, kestro-server, kestro-worker
# and the test programs). Then: node scripts/package.mjs linux-x64 --bin dist/build/linux-x64/bin
# libstdc++ and libcrypto are linked statically with hidden symbols (CMake options
# KESTRO_STATIC_RUNTIME, KESTRO_STATIC_CRYPTO); kestro-server alone uses the system's OpenSSL 3.
# Every file then needs only glibc 2.34+ (libc, libm, libgcc_s), checked below.
# The C++ suites run natively for x64 and under qemu-user for arm64 (ARM64_TESTS=0 skips them).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCHES=${*:-x64 arm64}
JOBS=${JOBS:-$(nproc)}
fail() { echo "FAILED: $1"; exit 1; }
# Highest GLIBC_/GLIBCXX_ symbol version each file needs, and its shared library dependencies.
report() {
  local dir=$1 objdump=$2 readelf=$3
  for f in kestro.node libkestrodb.so kestro-server kestro-worker; do
    local glibc cxx needed
    glibc=$("$objdump" -T "$dir/$f" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)
    cxx=$("$objdump" -T "$dir/$f" | grep -o 'GLIBCXX_[0-9.]*\|CXXABI_[0-9.]*' | sort -Vu | tail -1 || true)
    needed=$("$readelf" -d "$dir/$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
    printf '  %-16s needs %-12s %-14s libs: %s\n' "$f" "$glibc" "${cxx:-no-libstdc++}" "$needed"
    [ -z "$cxx" ] || fail "$f still needs the shared libstdc++ ($cxx)"
    [ "$(printf '%s\n' "$glibc" "GLIBC_${MAX_GLIBC:-2.34}" | sort -V | tail -1)" = "GLIBC_${MAX_GLIBC:-2.34}" ] || fail "$f needs $glibc (more than ${MAX_GLIBC:-2.34})"
  done
}
for arch in $ARCHES; do
  out="$ROOT/dist/build/linux-$arch/bin"
  build="$ROOT/build-linux-$arch"
  echo "=== linux-$arch ==="
  common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTRO_STATIC_RUNTIME=ON -DKESTRO_STATIC_CRYPTO=ON -DKESTRO_OUTPUT_DIR="$out"
    -DCMAKE_EXE_LINKER_FLAGS=-s -DCMAKE_SHARED_LINKER_FLAGS=-s -DCMAKE_MODULE_LINKER_FLAGS=-s)
  case $arch in
    x64)
      cmake -S "$ROOT" -B "$build" "${common[@]}" >/dev/null || fail "configure $arch"
      cmake --build "$build" -j "$JOBS" >/dev/null || fail "build $arch"
      ctest --test-dir "$build" --output-on-failure || fail "C++ suites ($arch)"
      report "$out" objdump readelf ;;
    arm64)
      cmake -S "$ROOT" -B "$build" "${common[@]}" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64-linux-gnu.cmake" \
        -DKESTRO_CROSS_GCC_SUFFIX="${CROSS_GCC_SUFFIX--12}" -DOPENSSL_INCLUDE_DIR="${ARM64_OPENSSL_INCLUDE:-/opt/arm64-openssl/include}" >/dev/null || fail "configure $arch"
      cmake --build "$build" -j "$JOBS" >/dev/null || fail "build $arch"
      if [ "${ARM64_TESTS:-1}" = 1 ]; then
        # Under qemu the suites run several times slower; the POSIX suite's timing tests need room.
        ctest --test-dir "$build" --output-on-failure --timeout 3600 || fail "C++ suites under qemu ($arch)"
      fi
      report "$out" aarch64-linux-gnu-objdump aarch64-linux-gnu-readelf ;;
    *) fail "unknown arch $arch (x64, arm64)" ;;
  esac
  file "$out/kestro-server" | sed 's/^/  /'
done
echo "ALL BUILT: $ARCHES"
