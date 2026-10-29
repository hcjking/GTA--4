#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# The game's shaders (LibertyRecompLib's SPIR-V cache) to WGSL for the page.
set -u
B=$GTA4_ROOT/web/build; SH=$GTA4_ROOT/web/shaders; R=$GTA4_ROOT/glue/rexglue-sdk-main
L=$GTA4_ROOT/LibertyRecompLib/shader
SM=$GTA4_ROOT/tools/XenosRecomp/thirdparty/smol-v/source
D=$B/shaders; mkdir -p $D/spv $D/spv-ubo $D/spv-opt $D/spv-web $B/link/wgsl $D/errors
echo "== tools $(date +%T)"
[ -x $D/extract_spirv ] || clang++ -std=c++20 -O1 $SH/extract_spirv.cpp $L/shader_cache.cpp $SM/smolv.cpp -I$L -I$SM -I/opt/homebrew/include -L/opt/homebrew/lib -lzstd -o $D/extract_spirv || exit 1
clang++ -std=c++20 -O2 -I$R/src/graphics/gta4_native -I$R/src/graphics -I$R/include $SH/rewrite_spirv.cpp -o $D/rewrite_spirv || exit 1
(cd $SH/spv2wgsl && cargo build --release -q --target-dir $D/spv2wgsl-target) || exit 1
echo "== extract $(date +%T)"
$D/extract_spirv $D/spv
$D/rewrite_spirv rewrite $D/spv $D/spv-ubo
cp $D/spv/index.tsv $D/spv-ubo/
for f in $D/spv-ubo/*.spv; do n=$(basename $f)
  spirv-opt --skip-validation --freeze-spec-const --fold-spec-const-op-composite --eliminate-dead-code-aggressive --eliminate-dead-variables $f -o $D/spv-opt/$n 2>/dev/null || cp $f $D/spv-opt/$n
done
cp $D/spv/index.tsv $D/spv-opt/
$D/rewrite_spirv strip $D/spv-opt $D/spv-web
echo "== wgsl $(date +%T)"
ok=0; bad=0
for f in $D/spv-web/*.spv; do n=$(basename $f .spv)
  if $D/spv2wgsl-target/release/spv2wgsl $f $B/link/wgsl/$n.wgsl > /dev/null 2> $D/errors/$n.txt; then ok=$((ok+1)); rm -f $D/errors/$n.txt; else bad=$((bad+1)); fi
done
echo "== done $(date +%T): $ok converted, $bad failed"
