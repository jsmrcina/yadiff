#!/usr/bin/env bash
# Builds yadiff-windows-x64.zip on Linux: yadiff cross-compiled with clang (llvm-mingw)
# against Qt's official llvm-mingw binaries, with its own Qt, KSyntaxHighlighting and C++
# runtime, so it runs on Windows 10 and 11 without installing anything.
#
# Runs as root on a fresh Ubuntu 22.04: the CI job's container, or locally
#   docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 packaging/windows/package.sh
# The zip is written to the source folder. The smoke test runs the packaged exe under Wine.
set -euo pipefail

QT_VERSION=${QT_VERSION:-6.8.3}
KF_VERSION=${KF_VERSION:-6.10.0}
# The llvm-mingw release with the LLVM that Qt's win64_llvm_mingw binaries were built with
# (17.0.6 for Qt 6.8), so yadiff and Qt share one libc++.
LLVM_MINGW=${LLVM_MINGW:-20231128}
# git for the smoke test: yadiff runs it even with --no-index.
MINGIT_URL=https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip

src=$(cd "$(dirname "$0")/../.." && pwd)
work=${WORK:-/tmp/yadiff-package-windows}
mkdir -p "$work"
cd "$work" # aqt writes its log to the current folder

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
    g++-12 ninja-build git python3-venv wget ca-certificates xz-utils zip unzip gnupg \
    libgl1-mesa-dev libglib2.0-0 libfontconfig1 libfreetype6 libdbus-1-3 libxkbcommon0 >/dev/null
export CC=gcc-12 CXX=g++-12

python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q aqtinstall==3.3.0 cmake==3.31.6
export PATH="$work/venv/bin:$PATH"

# The cross compiler.
llvm="$work/llvm-mingw-$LLVM_MINGW-ucrt-ubuntu-20.04-x86_64"
if [ ! -x "$llvm/bin/x86_64-w64-mingw32-clang++" ]; then
    wget -q -O "$work/llvm-mingw.tar.xz" \
        "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/$(basename "$llvm").tar.xz"
    tar -xf "$work/llvm-mingw.tar.xz" -C "$work"
    rm "$work/llvm-mingw.tar.xz"
fi
toolchain="$work/toolchain.cmake"
cat > "$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $llvm/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $llvm/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $llvm/bin/x86_64-w64-mingw32-windres)
EOF

# Qt for Windows, and the same Qt for Linux for its build tools (moc, rcc, uic).
qtwin="$work/qt/$QT_VERSION/llvm-mingw_64"
qthost="$work/qt/$QT_VERSION/gcc_64"
[ -x "$qthost/bin/qmake" ] || aqt install-qt -O "$work/qt" linux desktop "$QT_VERSION" linux_gcc_64 >/dev/null
[ -e "$qtwin/bin/Qt6Core.dll" ] || aqt install-qt -O "$work/qt" windows desktop "$QT_VERSION" win64_llvm_mingw -m qtimageformats >/dev/null
qtwin=$(dirname "$(dirname "$(find "$work/qt/$QT_VERSION" -path '*/bin/Qt6Core.dll' | head -1)")")
cross=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DQT_HOST_PATH="$qthost")

# KSyntaxHighlighting for Windows. Its build runs katehighlightingindexer, so build that for
# Linux first. It also runs rcc itself, so tell rcc that this Qt can't read zstd (Qt's own
# CMake functions do that for yadiff).
ecm="$work/ecm-install"
if [ ! -e "$ecm/share/ECM/cmake/ECMConfig.cmake" ]; then
    rm -rf "$work/ecm"
    git clone -q --depth 1 --branch "v$KF_VERSION" https://github.com/KDE/extra-cmake-modules.git "$work/ecm"
    cmake -S "$work/ecm" -B "$work/ecm/build" -G Ninja -DCMAKE_INSTALL_PREFIX="$ecm" \
        -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF >/dev/null
    cmake --build "$work/ecm/build" >/dev/null
    cmake --install "$work/ecm/build" >/dev/null
fi
kf="$work/kf6"
if [ ! -e "$kf/bin/libKF6SyntaxHighlighting.dll" ]; then
    rm -rf "$work/ksh" "$work/ksh-host"
    git clone -q --depth 1 --branch "v$KF_VERSION" https://github.com/KDE/syntax-highlighting.git "$work/ksh"
    sed -i 's/COMMAND Qt6::rcc --name syntax_data/COMMAND Qt6::rcc --no-zstd --name syntax_data/' "$work/ksh/data/CMakeLists.txt"
    cmake -S "$work/ksh" -B "$work/ksh-host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qthost;$ecm" \
        -DBUILD_TESTING=OFF -DBUILD_QCH=OFF -DQT_MAJOR_VERSION=6 >/dev/null
    cmake --build "$work/ksh-host" --target katehighlightingindexer >/dev/null
    cmake -S "$work/ksh" -B "$work/ksh/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$kf" \
        "${cross[@]}" -DCMAKE_PREFIX_PATH="$qtwin;$ecm" \
        -DKATEHIGHLIGHTINGINDEXER_EXECUTABLE="$work/ksh-host/bin/katehighlightingindexer" \
        -DBUILD_TESTING=OFF -DBUILD_QCH=OFF -DQT_MAJOR_VERSION=6 >/dev/null
    cmake --build "$work/ksh/build" > "$work/ksh-build.log" || { tail -40 "$work/ksh-build.log"; exit 1; }
    cmake --install "$work/ksh/build" >/dev/null
fi

build="$work/build"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${cross[@]}" \
    -DCMAKE_PREFIX_PATH="$qtwin;$kf" \
    -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build"

# The zip: yadiff.exe, the Qt plugins windeployqt would pick, and every DLL they import that
# Windows doesn't provide (Qt, KSyntaxHighlighting, libc++ and libunwind).
dist="$work/dist"
rm -rf "$dist" && mkdir -p "$dist/yadiff"
cp "$build/yadiff.exe" "$dist/yadiff/"
for plugin in platforms/qwindows styles/qmodernwindowsstyle iconengines/qsvgicon generic/qtuiotouchplugin \
              networkinformation/qnetworklistmanager tls/qschannelbackend tls/qcertonlybackend \
              imageformats/qgif imageformats/qicns imageformats/qico imageformats/qjpeg imageformats/qsvg \
              imageformats/qtga imageformats/qtiff imageformats/qwbmp imageformats/qwebp; do
    mkdir -p "$dist/yadiff/$(dirname "$plugin")"
    cp "$qtwin/plugins/$plugin.dll" "$dist/yadiff/$plugin.dll"
done
dllpath=("$qtwin/bin" "$kf/bin" "$llvm/x86_64-w64-mingw32/bin")
pending=$(find "$dist/yadiff" -name '*.exe' -o -name '*.dll')
while [ -n "$pending" ]; do
    next=""
    for binary in $pending; do
        for dll in $("$llvm/bin/llvm-objdump" -p "$binary" | sed -n 's/^ *DLL Name: //p'); do
            [ -e "$dist/yadiff/$dll" ] && continue
            for dir in "${dllpath[@]}"; do
                if [ -e "$dir/$dll" ]; then cp "$dir/$dll" "$dist/yadiff/"; next+=" $dist/yadiff/$dll"; break; fi
            done
        done
    done
    pending=$next
done
"$llvm/bin/llvm-strip" --strip-unneeded $(find "$dist/yadiff" -name '*.exe' -o -name '*.dll')
cp "$src/README.md" "$src/LICENSE" "$src/THIRD-PARTY-NOTICES.md" "$dist/yadiff/"
rm -f "$src/yadiff-windows-x64.zip"
(cd "$dist" && zip -qr "$src/yadiff-windows-x64.zip" yadiff)

# Smoke test: the unzipped copy runs under Wine with nothing else on its path and writes a
# report. The offscreen platform plugin is added to the test copy only. Wine is 64-bit only:
# wine-stable holds wine and wineserver but depends on the 32-bit half, so it's unpacked.
wine=/opt/wine-stable/bin/wine
if [ ! -x "$wine" ]; then
    mkdir -p /etc/apt/keyrings
    wget -q -O /etc/apt/keyrings/winehq.asc https://dl.winehq.org/wine-builds/winehq.key
    echo "deb [signed-by=/etc/apt/keyrings/winehq.asc] https://dl.winehq.org/wine-builds/ubuntu jammy main" \
        > /etc/apt/sources.list.d/winehq.list
    apt-get update -q
    apt-get install -y -q --no-install-recommends wine-stable-amd64 >/dev/null
    version=$(dpkg-query -W -f '${Version}' wine-stable-amd64)
    (cd /tmp && apt-get download -q "wine-stable=$version" >/dev/null && dpkg -x wine-stable_*.deb / && rm wine-stable_*.deb)
fi
mingit="$work/$(basename "$MINGIT_URL" .zip)"
if [ ! -x "$mingit/cmd/git.exe" ]; then
    wget -q -O "$work/mingit.zip" "$MINGIT_URL"
    rm -rf "$mingit"
    unzip -q "$work/mingit.zip" -d "$mingit"
    rm "$work/mingit.zip"
fi
smoke="$work/smoke"
rm -rf "$smoke" && mkdir -p "$smoke"
(cd "$smoke" && unzip -q "$src/yadiff-windows-x64.zip")
cp "$qtwin/plugins/platforms/qoffscreen.dll" "$smoke/yadiff/platforms/"
printf 'one\ntwo\n' > "$smoke/a.txt"
printf 'one\nTWO\n' > "$smoke/b.txt"
export WINEPREFIX="$work/wineprefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    WINEPATH="$mingit/cmd" XDG_RUNTIME_DIR=/tmp QT_QPA_PLATFORM=offscreen
(cd "$smoke" && timeout 300 "$wine" yadiff/yadiff.exe --no-index a.txt b.txt --log off --export report.html)
grep -q 'TWO' "$smoke/report.html" || { echo "smoke test: the report is missing the diff" >&2; exit 1; }

echo "Wrote $src/yadiff-windows-x64.zip ($(du -h "$src/yadiff-windows-x64.zip" | cut -f1))"
