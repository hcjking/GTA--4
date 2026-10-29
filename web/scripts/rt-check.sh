#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Syntax-check the ReXGlue runtime sources for WebAssembly (memory64 + pthreads).
# Usage: rt-check.sh <rexglue root> <source list> <out dir>
set -u
ROOT=$1
LIST=$2
OUT=$3
source ${EMSDK_DIR:-$GTA4_ROOT/../emsdk}/emsdk_env.sh >/dev/null 2>&1
T=$ROOT/thirdparty
export FLAGS="-std=c++23 -sMEMORY64 -pthread -msimd128 -fsyntax-only -ferror-limit=8 \
  -I$ROOT/include -I$ROOT/src -I$ROOT/src/filesystem/devices \
  -I$T/simde -I$T/fmt/include -I$T/spdlog/include -I$T/tomlplusplus/include -I$T/xxHash \
  -I$T/utfcpp/source -I$T/cli11/include -I$T/o1heap/o1heap -I$T/snappy -I$T/libmspack/libmspack/mspack \
  -I$T/imgui -I$T/sdl3/include -I$T/vulkan-headers/include -I$T/disruptorplus/include -I$T/inja/include \
  -I$T/stb -I$T/tiny-aes-c -I$T/picosha2 -I$T -I$T/../../../thirdparty/json/include \
  -fno-char8_t -fno-strict-aliasing -I$GTA4_ROOT/web/scripts/rt-gen -I$ROOT -I$T/renderdoc -I$T/glslang -DREXGLUE_BUILD_CONFIG=\"Release\" -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY"
export ROOT OUT
rm -rf "$OUT"; mkdir -p "$OUT"
check() {
  local src=$1 name
  name=$(echo "$src" | tr '/' '_')
  if em++ $FLAGS "$ROOT/$src" 2> "$OUT/$name.err"; then
    echo "OK $src"
  else
    echo "FAIL $src"
  fi
}
export -f check
xargs -P 14 -I{} bash -c 'check {}' < "$LIST" | sort > "$OUT/summary.txt"
echo "$(grep -c '^OK' "$OUT/summary.txt") ok, $(grep -c '^FAIL' "$OUT/summary.txt") failed"
