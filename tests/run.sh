#!/usr/bin/env bash
# Builds and runs the native checks (the camera maths, the Pose panel's logic) with zig's
# clang (the same one build.sh uses).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for t in cammath posemath posnmath uilayout; do
    uv tool run --from "ziglang==0.17.0" python -m ziglang c++ -std=c++17 -O2 -I"$here/src" \
        "$here/tests/${t}_test.cpp" -o "$out/${t}_test"
    "$out/${t}_test"
done
