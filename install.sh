#!/usr/bin/env bash
# Build yadiff and put it on PATH as ~/.local/bin/yadiff (a symlink into the build
# directory, so later rebuilds are picked up automatically).
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
mkdir -p "$HOME/.local/bin"
ln -sfn "$PWD/build/yadiff" "$HOME/.local/bin/yadiff"
echo "Installed: $HOME/.local/bin/yadiff -> $PWD/build/yadiff"
