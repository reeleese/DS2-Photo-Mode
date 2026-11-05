#!/usr/bin/env bash
# Cross-compiles the photo-mode DLL on Linux with zig's clang (from PyPI, no sudo).
# Needs bash and uv (https://docs.astral.sh/uv/); uv fetches zig on the first run.
#   ./build.sh [--dev] [BUILD_NAME]
# Writes $PM_BUILD_ROOT/<BUILD_NAME>/xinput1_3.dll (PM_BUILD_ROOT defaults to
# ~/.cache/ds2-photo-mode), or straight into $RELEASE_OUT when that is set, laid out like
# the game folder (the DLL goes next to DarkSoulsII.exe). BUILD_NAME defaults to a
# timestamp. --dev adds the file command channel (src/devchan.cpp) used to find the camera.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dev=0
if [[ "${1:-}" == "--dev" ]]; then dev=1; shift; fi
name="${1:-$(date +%Y%m%d-%H%M%S)}"
root="${PM_BUILD_ROOT:-$HOME/.cache/ds2-photo-mode}"
out="${RELEASE_OUT:-$root/$name}"
obj="$root/.obj-$dev"
mkdir -p "$out" "$obj"

zig=(uv tool run --from "ziglang==0.17.0" python -m ziglang)
tp="$here/third_party"
flags=(-target x86_64-windows-gnu -O2 -g0 -DNDEBUG -DUNICODE -D_UNICODE
       -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
       -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS
       -I"$here/src" -I"$tp/imgui" -I"$tp/minhook/include")
[[ $dev == 1 ]] && flags+=(-DPM_DEV)

srcs=("$here"/src/*.cpp "$tp"/imgui/imgui.cpp "$tp"/imgui/imgui_draw.cpp "$tp"/imgui/imgui_tables.cpp
      "$tp"/imgui/imgui_widgets.cpp "$tp"/imgui/backends/imgui_impl_dx11.cpp)
csrcs=("$tp"/minhook/src/hook.c "$tp"/minhook/src/buffer.c "$tp"/minhook/src/trampoline.c "$tp"/minhook/src/hde/hde64.c)
[[ $dev == 0 ]] && srcs=("${srcs[@]/$here\/src\/devchan.cpp}")

objs=()
pids=()
for s in "${srcs[@]}" "${csrcs[@]}"; do
  [[ -z "$s" ]] && continue
  o="$obj/$(basename "${s%.*}").o"
  objs+=("$o")
  stale=0
  [[ ! -f "$o" || "$s" -nt "$o" ]] && stale=1
  for h in "$here"/src/*.h; do [[ "$h" -nt "$o" ]] && stale=1; done
  if [[ $stale == 1 ]]; then
    if [[ "$s" == *.c ]]; then
      "${zig[@]}" cc "${flags[@]}" -c "$s" -o "$o" & pids+=($!)
    else
      "${zig[@]}" c++ -std=c++17 "${flags[@]}" -c "$s" -o "$o" & pids+=($!)
    fi
  fi
done
for p in "${pids[@]}"; do wait "$p"; done

"${zig[@]}" c++ -target x86_64-windows-gnu -shared -o "$out/xinput1_3.dll" "${objs[@]}" "$here/src/xinput1_3.def" \
  -ld3d11 -ldxgi -ld3dcompiler_47 -luser32 -lkernel32
rm -f "$out"/*.pdb "$out"/*.lib
# lld stamps the link time and a random GUID into the PE; make the output reproducible.
python3 "$here/normalize_pe.py" "$out/xinput1_3.dll"
# The licences of the code inside the DLL travel with it (ImGui: MIT, MinHook: BSD-2).
{
  echo "DS2 photo mode (xinput1_3.dll) contains the following third-party code."
  echo
  echo "== Dear ImGui v1.92.9b (https://github.com/ocornut/imgui) =="
  cat "$tp/imgui/LICENSE.txt"
  echo
  echo "== MinHook v1.3.4 (https://github.com/TsudaKageyu/minhook) =="
  cat "$tp/minhook/LICENSE.txt"
} > "$out/photo-mode-licenses.txt"
echo "$out"
