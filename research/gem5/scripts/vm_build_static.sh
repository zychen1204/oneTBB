#!/usr/bin/env bash
# VM-side: builds static libtbb.a for both oneTBB branches (master=stock,
# caws=modified), then static PARSEC benchmarks and ws_bench for gem5 SE mode.
set -euo pipefail

TBB_SRC=$HOME/ControlStealing/oneTBB
PORTS=$HOME/parsec-ports
BENCH=$HOME/ControlStealing/bench
OUT=$HOME/static
FLAGS="-std=c++17 -O2 -DNDEBUG -D__TBB_BUILD=1 -fPIC -I$TBB_SRC/include -I$TBB_SRC/src"

build_tbb() { # $1 = branch, $2 = outdir
    git -C "$TBB_SRC" checkout -q "$1"
    rm -rf "$2" && mkdir -p "$2" && cd "$2"
    for f in "$TBB_SRC"/src/tbb/*.cpp; do
        g++ $FLAGS -c "$f" -o "$(basename "${f%.cpp}").o" &
    done
    wait
    ar rcs libtbb.a ./*.o
    echo "TBB($1) -> $2/libtbb.a"
}

build_tbb caws  "$OUT/tbb_caws"
build_tbb master "$OUT/tbb_stock"
git -C "$TBB_SRC" checkout -q caws

for v in stock caws; do
    STATIC=1 TBB_INC=$TBB_SRC/include TBB_LIB=$OUT/tbb_$v "$PORTS/build.sh" "$OUT/bin_$v"
    g++ -std=c++17 -O2 -static -I"$TBB_SRC/include" "$BENCH/ws_bench.cpp" \
        -Wl,--whole-archive "$OUT/tbb_$v/libtbb.a" -Wl,--no-whole-archive \
        -lpthread -ldl -o "$OUT/bin_$v/ws_bench"
done

echo STATIC_BUILD_OK
ls -la "$OUT"/bin_stock "$OUT"/bin_caws
