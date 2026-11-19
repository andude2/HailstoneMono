#!/usr/bin/env bash
set -euo pipefail
sdk=${1:?give the Hailstone sdk directory}
here=$(cd -- "$(dirname -- "$0")" && pwd)
version=$(sed -n 's/^version //p' "$here/../hailstone-addon.txt")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/managed" "$scratch/runtime/lib/mono/4.5" "$scratch/runtime/etc"
touch "$scratch/managed/Smoke.dll" "$scratch/runtime/lib/mono/4.5/mscorlib.dll"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
    "-DHAILSTONE_MONO_VERSION=\"$version\"" \
    -I "$sdk" -I "$here/.." -isystem "$here/../../Mono/include/mono-2.0" \
    "$here/SmokeHost.cpp" "$here/../Plugin.cpp" -o "$scratch/smoke-host"
"$scratch/smoke-host" "$scratch"
