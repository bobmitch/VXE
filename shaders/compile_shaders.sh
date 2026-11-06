#!/usr/bin/env bash
# Compiles every terrain shader entry point to DXIL (D3D12) and SPIR-V (Vulkan 1.3).
# Usage: DXC=/path/to/dxc ./compile_shaders.sh [out_dir]
set -euo pipefail
DXC=${DXC:-dxc}
OUT=${1:-build/shaders}
cd "$(dirname "$0")"
mkdir -p "$OUT"
fail=0
compile() {
  local file=$1 profile=$2 entry=$3
  for target in dxil spirv; do
    local extra=()
    [[ $target == spirv ]] && extra=(-spirv -fspv-target-env=vulkan1.3)
    if "$DXC" -HV 2021 -WX -O3 -T "$profile" -E "$entry" "${extra[@]}" "$file" -Fo "$OUT/$entry.$target" >"$OUT/$entry.$target.log" 2>&1; then
      echo "ok    $target  $file : $entry"
    else
      echo "FAIL  $target  $file : $entry"; cat "$OUT/$entry.$target.log"; fail=1
    fi
  done
}
compile TerrainMesh.as.hlsl      as_6_5 ASMain
compile TerrainMesh.ms.hlsl      ms_6_5 MSMain
compile TerrainTess.hlsl         vs_6_1 VSMain
compile TerrainTess.hlsl         hs_6_1 HSMain
compile TerrainTess.hlsl         ds_6_1 DSMain
compile TerrainDisplace.cs.hlsl  cs_6_5 CSDisplace
compile TerrainDisplace.cs.hlsl  cs_6_5 CSWriteArgs
compile TerrainNearField.hlsl    vs_6_1 VSNearField
compile TerrainNearField.hlsl    vs_6_1 VSClassic
compile Terrain.ps.hlsl          ps_6_1 PSMain
exit $fail
