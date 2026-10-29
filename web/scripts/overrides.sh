#!/bin/bash
# (the repository: two folders up from this script, in web/scripts or web/build)
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
# The native renderer's always-on shader overrides (LibertyRecompLib/
# shader_overrides: the ones modern_shader_policy.h AllowModernShader keeps
# with gta4_modern_shaders off - the motion-blur composites, Bink video and
# the 0xF6AEB9A606561C54 final composite) to WGSL for the page, as
# link/wgsl/override/<hash>.wgsl. The page uses one in place of the stock
# shader of that hash when it is there (scene3d.js kOverrides).
# Needs dxc (the manifest's HLSL; glslangValidator for its GLSL) and spirv-val,
# or OVR_SPV=<dir> of already compiled modules; and the tools shaders.sh builds.
set -u
B=$GTA4_ROOT/web/build; R=$GTA4_ROOT/glue/rexglue-sdk-main
O=$GTA4_ROOT/LibertyRecompLib/shader_overrides
D=$B/shaders/ovr; rm -rf $D; mkdir -p $D/spv $D/spv-ubo $D/spv-opt $D/spv-web $B/link/wgsl/override
[ -x $B/shaders/rewrite_spirv ] && [ -x $B/shaders/spv2wgsl-target/release/spv2wgsl ] || { echo "run shaders.sh first"; exit 1; }
if [ -n "${OVR_SPV:-}" ]; then
  # Already compiled: <hash>.spv files and an index.tsv, e.g. pulled out of a
  # release eboot.bin's g_shaderOverrideEntries table (same source commit).
  cp "$OVR_SPV"/*.spv "$OVR_SPV"/index.tsv $D/spv/ || exit 1
else
DXC=${DXC:-$(command -v dxc)}
[ -n "$DXC" ] || { echo "dxc not found (set DXC=/path/to/dxc, or OVR_SPV=<dir of compiled .spv>)"; exit 1; }
# The always-on entries only.
python3 -I - "$O/manifest.json" "$D/manifest.json" <<'EOF'
import json, sys
keep = {0xF6AEB9A606561C54, 0xEE75C9F6AA1AB16A, 0xE51D9DD95A333D92, 0xD3B2B2125BC24911, 0x94CC5AF0E2FF5B08,
        0x535ACDAB8AE84D82, 0x156BAD4A9EE62726, 0x9E76B68B60127349, 0xA6C9E2B8B2A59D7A}
m = json.load(open(sys.argv[1]))
m["overrides"] = [e for e in m["overrides"] if int(e["hash"], 16) in keep]
json.dump(m, open(sys.argv[2], "w"), indent=2)
print(len(m["overrides"]), "overrides")
EOF
python3 -I $R/tools/gta4_shader_override_compiler.py --manifest $D/manifest.json --source-root $O --output $D/table.cpp \
  --dxc "$DXC" --glslang-validator "$(command -v glslangValidator)" --spirv-val "$(command -v spirv-val)" || exit 1
# The table's word arrays back to .spv files, in manifest order.
python3 -I - "$D/manifest.json" "$D/table.cpp" "$D/spv" <<'EOF'
import json, re, struct, sys, os
entries = json.load(open(sys.argv[1]))["overrides"]
table = open(sys.argv[2]).read()
with open(os.path.join(sys.argv[3], "index.tsv"), "w") as index:
    for i, e in enumerate(entries):
        m = re.search(r"kShaderOverrideSpirv%d\[\] = \{([^}]*)\}" % i, table)
        words = [int(w, 0) for w in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))]
        name = "%016X.spv" % int(e["hash"], 16)
        open(os.path.join(sys.argv[3], name), "wb").write(struct.pack("<%dI" % len(words), *words))
        index.write("%s\t%s\n" % (name, e["source"]))
EOF
fi
$B/shaders/rewrite_spirv rewrite $D/spv $D/spv-ubo && cp $D/spv/index.tsv $D/spv-ubo/
for f in $D/spv-ubo/*.spv; do n=$(basename $f)
  spirv-opt --skip-validation --freeze-spec-const --fold-spec-const-op-composite --eliminate-dead-code-aggressive --eliminate-dead-variables $f -o $D/spv-opt/$n 2>/dev/null || cp $f $D/spv-opt/$n
done
cp $D/spv/index.tsv $D/spv-opt/
$B/shaders/rewrite_spirv strip $D/spv-opt $D/spv-web
for f in $D/spv-web/*.spv; do n=$(basename $f .spv)
  $B/shaders/spv2wgsl-target/release/spv2wgsl $f $B/link/wgsl/override/$n.wgsl > /dev/null 2> $D/$n.err && rm -f $D/$n.err || echo "failed: $n"
done
ls $B/link/wgsl/override
