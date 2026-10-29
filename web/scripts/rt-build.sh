#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Compile the ReXGlue runtime for WebAssembly (memory64 + pthreads) into objects.
# Usage: rt-build.sh <rexglue root> <source list> <out dir> <thirdparty build dir>
set -u
ROOT=$1
LIST=$2
OUT=$3
TPB=$4
S=$(dirname "$0")
source ${EMSDK_DIR:-$GTA4_ROOT/../emsdk}/emsdk_env.sh >/dev/null 2>&1
T=$ROOT/thirdparty
export FLAGS="-std=c++23 -O2 -sMEMORY64 -pthread -msimd128 -fno-char8_t -fno-strict-aliasing \
  -ffp-model=strict -DNDEBUG -DSPDLOG_FMT_EXTERNAL -DSPDLOG_COMPILED_LIB -DFMT_USE_CONSTEVAL=0 \
  -DREXGLUE_BUILD_CONFIG=\"Release\" -I$S/rt-gen -I$ROOT -I$ROOT/include -I$ROOT/src \
  -I$ROOT/src/filesystem/devices -I$T -I$T/simde -I$T/fmt/include -I$T/spdlog/include \
  -I$T/tomlplusplus/include -I$T/xxHash -I$T/utfcpp/source -I$T/cli11/include -I$T/o1heap/o1heap \
  -I$T/snappy -I$TPB/tp/snappy -I$T/libmspack/cabextract/mspack -I$T/imgui -I$T/sdl3/include \
  -I$T/vulkan-headers/include -I$T/disruptorplus/include -I$T/inja/include -I$T/stb \
  -I$T/tiny-aes-c -I$T/renderdoc -I$T/FFmpeg -I$TPB/tp/ffmpeg-overlay ${EXTRA:-}"
export ROOT OUT
mkdir -p "$OUT"
build() {
  local src=$1 name
  name=$(echo "$src" | tr '/' '_' | sed 's/\.cpp$//')
  if em++ $FLAGS -c "$ROOT/$src" -o "$OUT/$name.o" 2> "$OUT/$name.err"; then
    echo "OK $src"
  else
    echo "FAIL $src"
  fi
}
export -f build
start=$(date +%s)
xargs -P ${JOBS:-8} -I{} bash -c 'build {}' < "$LIST" | sort > "$OUT/summary.txt"
echo "$(grep -c '^OK' "$OUT/summary.txt") ok, $(grep -c '^FAIL' "$OUT/summary.txt") failed in $(( $(date +%s) - start ))s"
