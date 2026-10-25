#!/usr/bin/env bash
# Builds yadiff-linux-x64.zip: yadiff with its own Qt and KSyntaxHighlighting, so it runs on
# any x86-64 distribution as new as Ubuntu 22.04 (glibc 2.35, libstdc++ from GCC 12) without
# installing Qt. Only git is needed on the machine that runs it.
#
# Runs as root on a fresh Ubuntu 22.04: the CI job's container, or locally
#   docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 packaging/linux/package.sh
# The zip is written to the source folder.
set -euo pipefail

QT_VERSION=${QT_VERSION:-6.8.3}
KF_VERSION=${KF_VERSION:-6.10.0}
LINUXDEPLOY_TAG=1-alpha-20251107-1
PLUGIN_QT_TAG=1-alpha-20250213-1

src=$(cd "$(dirname "$0")/../.." && pwd)
work=${WORK:-/tmp/yadiff-package}
mkdir -p "$work"

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
    g++-12 make ninja-build git perl python3-venv wget ca-certificates file zip unzip patchelf \
    libgl1-mesa-dev libegl1 libglib2.0-0 libfontconfig1 libfreetype6 libdbus-1-3 \
    libxkbcommon0 libxkbcommon-x11-0 libx11-xcb1 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
    libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
    libxcb-sync1 libxcb-xfixes0 libxcb-shm0 libwayland-client0 libwayland-cursor0 libwayland-egl1 >/dev/null
export CC=gcc-12 CXX=g++-12

# Qt from the official binaries (Ubuntu 22.04's own Qt 6 is too old), and a recent CMake.
python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q aqtinstall==3.3.0 cmake==3.31.6
export PATH="$work/venv/bin:$PATH"
qt="$work/qt/$QT_VERSION/gcc_64"
if [ ! -x "$qt/bin/qmake" ]; then
    aqt install-qt -O "$work/qt" linux desktop "$QT_VERSION" linux_gcc_64 -m qtimageformats qtwaylandcompositor
fi

# KSyntaxHighlighting (and extra-cmake-modules to build it).
kf="$work/kf6"
if [ ! -e "$kf/lib/libKF6SyntaxHighlighting.so.6" ] && [ ! -e "$kf/lib/x86_64-linux-gnu/libKF6SyntaxHighlighting.so.6" ]; then
    rm -rf "$work/ecm" "$work/ksh"
    git clone -q --depth 1 --branch "v$KF_VERSION" https://github.com/KDE/extra-cmake-modules.git "$work/ecm"
    cmake -S "$work/ecm" -B "$work/ecm/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$kf" \
        -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF >/dev/null
    cmake --build "$work/ecm/build" >/dev/null && cmake --install "$work/ecm/build" >/dev/null
    git clone -q --depth 1 --branch "v$KF_VERSION" https://github.com/KDE/syntax-highlighting.git "$work/ksh"
    cmake -S "$work/ksh" -B "$work/ksh/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$kf" \
        -DCMAKE_PREFIX_PATH="$qt;$kf" -DBUILD_TESTING=OFF -DBUILD_QCH=OFF -DQT_MAJOR_VERSION=6 >/dev/null
    cmake --build "$work/ksh/build" >/dev/null && cmake --install "$work/ksh/build" >/dev/null
fi
kflib=$(dirname "$(find "$kf" -name 'libKF6SyntaxHighlighting.so.6' | head -1)")

build="$work/build"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt;$kf" -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build"

# Bundle the Qt libraries and plugins (X11, Wayland, offscreen) next to the program.
for tool in "linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage" \
            "linuxdeploy-plugin-qt/releases/download/$PLUGIN_QT_TAG/linuxdeploy-plugin-qt-x86_64.AppImage"; do
    [ -x "$work/$(basename "$tool")" ] || { wget -q -O "$work/$(basename "$tool")" "https://github.com/linuxdeploy/$tool"; chmod +x "$work/$(basename "$tool")"; }
done
appdir="$work/AppDir"
rm -rf "$appdir"
export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1
export QMAKE="$qt/bin/qmake" LD_LIBRARY_PATH="$kflib:$qt/lib"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="waylandcompositor;wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
"$work/linuxdeploy-x86_64.AppImage" --appdir "$appdir" --executable "$build/yadiff" \
    --desktop-file "$src/resources/yadiff.desktop" --icon-file "$src/resources/yadiff.svg" --plugin qt >/dev/null

# The zip: one folder, started by its `yadiff` script (which follows a symlink to it).
dist="$work/dist"
rm -rf "$dist" && mkdir -p "$dist/yadiff"
cp -a "$appdir/usr" "$dist/yadiff/"
cp "$src/resources/yadiff.desktop" "$src/resources/yadiff.svg" "$dist/yadiff/"
cp "$src/README.md" "$src/LICENSE" "$src/THIRD-PARTY-NOTICES.md" "$dist/yadiff/"
cat > "$dist/yadiff/yadiff" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/yadiff" "$@"
EOF
chmod +x "$dist/yadiff/yadiff"
rm -f "$src/yadiff-linux-x64.zip"
(cd "$dist" && zip -qry "$src/yadiff-linux-x64.zip" yadiff)

# Smoke test: the unzipped copy, with the build's Qt out of the way, writes a report.
smoke="$work/smoke"
rm -rf "$smoke" && mkdir -p "$smoke"
mv "$qt" "$qt.hidden" && mv "$kf" "$kf.hidden"
trap 'mv "$qt.hidden" "$qt"; mv "$kf.hidden" "$kf"' EXIT
(cd "$smoke" && unzip -q "$src/yadiff-linux-x64.zip")
mkdir -p "$smoke/repo"
git -C "$smoke/repo" init -q
printf 'one\n' > "$smoke/repo/a.txt"
git -C "$smoke/repo" add a.txt
git -C "$smoke/repo" -c user.name=ci -c user.email=ci@example.com commit -qm one
printf 'TWO\n' > "$smoke/repo/a.txt"
env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen "$smoke/yadiff/yadiff" --version
env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen "$smoke/yadiff/yadiff" -C "$smoke/repo" --log off --export "$smoke/report.html"
grep -q 'TWO' "$smoke/report.html" || { echo "smoke test: the report is missing the diff" >&2; exit 1; }

echo "Wrote $src/yadiff-linux-x64.zip ($(du -h "$src/yadiff-linux-x64.zip" | cut -f1))"
