#!/usr/bin/env bash
# Build yadiff and put it on PATH as ~/.local/bin/yadiff (a symlink into the build
# directory, so later rebuilds are picked up automatically). Also installs the
# icon and a (hidden) desktop entry, which KDE/Wayland uses for the window icon.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure

mkdir -p "$HOME/.local/bin"
ln -sfn "$PWD/build/yadiff" "$HOME/.local/bin/yadiff"

icon_dir="$HOME/.local/share/icons/hicolor/scalable/apps"
apps_dir="$HOME/.local/share/applications"
mkdir -p "$icon_dir" "$apps_dir"
cp resources/yadiff.svg "$icon_dir/yadiff.svg"
# Absolute Exec/Icon paths, per this machine's .desktop conventions.
sed -e "s|^Exec=.*|Exec=$HOME/.local/bin/yadiff %F|" \
    -e "s|^Icon=.*|Icon=$icon_dir/yadiff.svg|" \
    resources/yadiff.desktop > "$apps_dir/yadiff.desktop"
if command -v update-desktop-database >/dev/null; then update-desktop-database "$apps_dir" || true; fi
if command -v kbuildsycoca6 >/dev/null; then kbuildsycoca6 --noincremental >/dev/null 2>&1 || true; fi

echo "Installed: $HOME/.local/bin/yadiff -> $PWD/build/yadiff"
