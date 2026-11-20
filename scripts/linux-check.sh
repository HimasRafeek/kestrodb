#!/usr/bin/env bash
# The Linux checks of .github/workflows/platforms.yml, run locally on Linux (or under WSL2 on
# Windows, for example `wsl -- bash scripts/linux-check.sh` from the repository root).
# It syncs a copy under $HOME (KESTRO_WORK, default ~/kestrodb) to the last commit of the
# repository that contains this script (KESTRO_REPO overrides it), then runs the steps given
# (default all):
#   build    release build (Ninja) and the C++ suites (portable, POSIX, power loss)
#   node     every Node suite (npm test), PHP/Python/Java clients included
#   asan     C++ suites under AddressSanitizer + UndefinedBehaviorSanitizer
#   tsan     C++ suites under ThreadSanitizer
#   fuzz     the four libFuzzer targets, FUZZ_SECONDS each (default 60)
# Stops at the first failure and says which step failed.
set -euo pipefail
export PATH="$PATH:$HOME/.cargo/bin:/usr/local/go/bin"  # the Rust and Go client suites
REPO=${KESTRO_REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${KESTRO_WORK:-$HOME/kestrodb}
JOBS=${JOBS:-$(nproc)}
STEPS=${*:-build node asan tsan fuzz}
step() { echo; echo "=== $1 ==="; }
fail() { echo; echo "FAILED: $1"; exit 1; }

step "sync $WORK to $(git -C "$REPO" log --oneline -1)"
[ -d "$WORK/.git" ] || git clone -q "$REPO" "$WORK"
git -C "$WORK" fetch -q "$REPO" HEAD && git -C "$WORK" reset -q --hard FETCH_HEAD && git -C "$WORK" clean -qfdx -e build -e build-asan -e build-tsan -e build-fuzz -e bin -e node_modules
cd "$WORK"

for s in $STEPS; do case $s in
  build)
    step "release build and C++ suites"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTRO_OUTPUT_DIR="$WORK/bin" >/dev/null || fail "configure"
    cmake --build build -j "$JOBS" >/dev/null || fail "build"
    ctest --test-dir build --output-on-failure || fail "C++ suites" ;;
  node)
    step "Node suites"
    ok=0; node scripts/test.mjs >/tmp/kestro-node-tests.log 2>&1 || ok=$?
    grep -E "^ℹ (tests|pass|fail|skipped)|^✖" /tmp/kestro-node-tests.log | head -20
    [ $ok = 0 ] || fail "Node suites (log: /tmp/kestro-node-tests.log)" ;;
  asan|tsan)
    san=$([ "$s" = asan ] && echo address || echo thread)
    # ThreadSanitizer (clang 18) aborts with "unexpected memory mapping" on kernels that
    # randomize addresses over more bits (WSL2 6.18): run it without address randomization.
    norand=$([ "$s" = tsan ] && echo "setarch $(uname -m) -R" || true)
    # Its lock-order (deadlock) detector tracks at most 64 locks held at once and aborts above
    # that; commits and compactions hold all 64 index shard locks plus readers_. Data races are
    # still detected.
    step "C++ suites under $san sanitizer"
    cmake -S . -B "build-$s" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKESTRO_SANITIZER=$san >/dev/null || fail "$s configure"
    cmake --build "build-$s" -j "$JOBS" --target kestro-portable-tests kestro-posix-tests kestro-powerloss-tests >/dev/null || fail "$s build"
    ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=0 $norand ctest --test-dir "build-$s" --output-on-failure || fail "$s suites" ;;
  fuzz)
    secs=${FUZZ_SECONDS:-60}
    step "fuzzing, $secs s per target"
    [ -f bin/kestro.node ] || { cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTRO_OUTPUT_DIR="$WORK/bin" >/dev/null && cmake --build build -j "$JOBS" >/dev/null; }
    node fuzz/seed.mjs >/dev/null
    mkdir -p build-fuzz/crashes
    for c in blake3 blake3_dispatch blake3_portable; do
      clang -O1 -g -fsanitize=address,undefined -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -Ivendor/blake3 -c vendor/blake3/$c.c -o build-fuzz/$c.o
    done
    for t in fuzz_parse fuzz_protocol fuzz_sql_exec fuzz_open; do
      clang++ -std=c++17 -O1 -g -fsanitize=fuzzer,address,undefined -Isrc -Ivendor/blake3 fuzz/$t.cpp build-fuzz/blake3.o build-fuzz/blake3_dispatch.o build-fuzz/blake3_portable.o -lcrypto -lpthread -o build-fuzz/$t &
    done; wait
    for t in parse protocol sql_exec open; do
      len=$([ $t = open ] && echo 262144 || echo 2048); corpus=fuzz/corpus-${t/sql_exec/sql}
      build-fuzz/fuzz_$t -max_total_time=$secs -timeout=10 -max_len=$len -artifact_prefix=build-fuzz/crashes/$t- -print_final_stats=1 "$corpus" 2>&1 | grep -E "stat::number_of_executed_units|ERROR|SUMMARY" || true
      [ "${PIPESTATUS[0]}" = 0 ] || fail "fuzz_$t (inputs in $WORK/build-fuzz/crashes)"
    done ;;
  *) fail "unknown step $s" ;;
esac; done
echo; echo "ALL PASSED: $STEPS"
