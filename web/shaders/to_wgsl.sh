#!/bin/bash
# GTA IV's recompiled shaders (SPIR-V) to WGSL for the WebGPU renderer:
# rewrite constant reads to uniform buffers, remove dead code, drop the
# buffer-pointer capability, then translate with naga.
# Usage: to_wgsl.sh <shaders dir with spv/ and index.tsv>
set -u
D=$1
export PATH="$HOME/.cargo/bin:$PATH"
rm -rf "$D/spv-ubo" "$D/spv-dce" "$D/spv-web" "$D/wgsl" "$D/errors"
mkdir -p "$D/spv-dce" "$D/wgsl" "$D/errors"
"$D/rewrite_spirv" rewrite "$D/spv" "$D/spv-ubo"
cp "$D/index.tsv" "$D/spv-ubo/index.tsv" 2>/dev/null
for f in "$D"/spv-ubo/*.spv; do
  n=$(basename "$f")
  spirv-opt --skip-validation --freeze-spec-const --fold-spec-const-op-composite --eliminate-dead-code-aggressive --eliminate-dead-variables \
    "$f" -o "$D/spv-dce/$n" 2>/dev/null || cp "$f" "$D/spv-dce/$n"
done
cp "$D/index.tsv" "$D/spv-dce/index.tsv"
"$D/rewrite_spirv" strip "$D/spv-dce" "$D/spv-web"
ok=0
bad=0
for f in "$D"/spv-web/*.spv; do
  n=$(basename "$f" .spv)
  if naga "$f" "$D/wgsl/$n.wgsl" > "$D/errors/$n.txt" 2>&1; then
    ok=$((ok + 1))
    rm -f "$D/errors/$n.txt"
  else
    bad=$((bad + 1))
  fi
done
echo "converted $ok / failed $bad"
cat "$D"/errors/*.txt 2>/dev/null | grep -vE '^\s*$|^\s*[│┌└╰╭]|^ *=|NoContraction' |
  sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -12
