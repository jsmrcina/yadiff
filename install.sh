#!/usr/bin/env bash
# Build yadiff, run the tests, and install a snapshot of it:
#   ~/.local/opt/yadiff-<version>/   (version from `git describe`, e.g. v0.1.0 or v0.1.0-3-gabc1234)
#   ~/.local/bin/yadiff  ->  that snapshot's bin/yadiff
# Development builds in build/ never change what is installed until this is run again.
# Also installs the icon and a (hidden) desktop entry, which KDE/Wayland uses for the window icon.
set -euo pipefail
cd "$(dirname "$0")"
version=$(git describe --tags --always --dirty 2>/dev/null || echo dev)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure

prefix="$HOME/.local/opt/yadiff-$version"
cmake --install build --prefix "$prefix" >/dev/null
mkdir -p "$HOME/.local/bin"
ln -sfn "$prefix/bin/yadiff" "$HOME/.local/bin/yadiff"

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

echo "Installed yadiff $version: $HOME/.local/bin/yadiff -> $prefix/bin/yadiff"
echo "Older snapshots stay in ~/.local/opt/yadiff-* until you delete them."
