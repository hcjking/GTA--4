#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# Rebuilds the whole GTA IV web build into this folder (survives restarts,
# unlike /private/tmp): game code, runtime, glue, crypto, then the link.
# Usage: rebuild.sh   (JOBS=n for parallel compiles, default 8)
set -u
B=$(cd "$(dirname "$0")" && pwd)
R=$GTA4_ROOT/glue/rexglue-sdk-main
TPB=$GTA4_ROOT/web/build-tp
source ${EMSDK_DIR:-$GTA4_ROOT/../emsdk}/emsdk_env.sh >/dev/null 2>&1
export JOBS=${JOBS:-8}
echo "== game code $(date +%T)"
[ "$(ls $B/wasm/*.o 2>/dev/null | wc -l)" -ge 91 ] || bash $B/wasm-build.sh $R/gta4-recomp/generated $B/wasm
echo "== runtime $(date +%T)"
bash $B/rt-build.sh $R $B/rt-list.txt $B/rt-obj $TPB
grep FAIL $B/rt-obj/summary.txt
rm -f $B/librexruntime.a
emar rcs $B/librexruntime.a $B/rt-obj/*.o
echo "== glue $(date +%T)"
EXTRA="-I$R/gta4-recomp/src -I$R/gta4-recomp/generated -I$R/gta4-recomp -I$R/src/graphics -I$R/thirdparty/crypto -I$R/../../thirdparty/json/include -sUSE_ZLIB=1 -include $B/rt-gen/web_glue_paths.h" \
  bash $B/rt-build.sh $R $B/glue-list.txt $B/glue-obj $TPB
grep FAIL $B/glue-obj/summary.txt
echo "== crypto $(date +%T)"
for f in $(cat $B/crypto-list.txt); do
  f=$GTA4_ROOT/$f
  n=$(basename "$f"); n=${n%.*}
  case "$f" in *.c) emcc -O2 -sMEMORY64 -pthread -c "$f" -o $B/crypto-obj/$n.o ;; *) em++ -std=c++20 -O2 -sMEMORY64 -pthread -c "$f" -o $B/crypto-obj/$n.o ;; esac || echo "FAIL $f"
done
echo "== link $(date +%T)"
bash $B/link.sh $TPB -g2 -sASSERTIONS=1 -sWASMFS -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM \
  -sENVIRONMENT=web,worker -sPTHREAD_POOL_SIZE_STRICT=0 -sEXPORTED_RUNTIME_METHODS=FS,ENV,callMain -sOFFSCREENCANVAS_SUPPORT=1
grep -m 20 "undefined symbol\|error:" $B/link/link.log
echo "== done $(date +%T)"
