#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Compile LibertyRecomp's generated GTA IV code with Emscripten (memory64 + pthreads).
# Usage: wasm-build.sh <generated dir> <out dir>
set -u
GEN=$1
OUT=$2
source ${EMSDK_DIR:-$GTA4_ROOT/../emsdk}/emsdk_env.sh >/dev/null 2>&1
export FLAGS="-std=c++23 -fno-char8_t -fno-strict-aliasing -ffp-model=strict -DNDEBUG -DSPDLOG_FMT_EXTERNAL -DSPDLOG_COMPILED_LIB -I$GTA4_ROOT/web/build/rt-gen -sMEMORY64 -pthread -msimd128 -I$GEN -I$GEN/../../include \
  -I$GEN/../../thirdparty/simde -I$GEN/../../thirdparty/fmt/include -I$GEN/../../thirdparty/spdlog/include"
export OUT
mkdir -p "$OUT"
compile() {
  local src=$1 opt=$2 name
  name=$(basename "$src" .cpp)
  em++ $FLAGS "$opt" -c "$src" -o "$OUT/$name.o" 2> "$OUT/$name.err" || echo "FAIL $name"
}
export -f compile
start=$(date +%s)
ls "$GEN"/gta4_recomp.*.cpp | xargs -P ${JOBS:-8} -I{} bash -c 'compile {} -O2'
compile "$GEN/gta4_register.cpp" -O0
compile "$GEN/gta4_init.cpp" -O2
echo "took $(( $(date +%s) - start ))s, $(ls "$OUT"/*.o 2>/dev/null | wc -l) objects"
