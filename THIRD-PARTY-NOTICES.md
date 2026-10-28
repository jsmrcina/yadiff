# Third-party notices

yadiff itself is under the MIT license (see `LICENSE`). It uses, and the release zips ship,
the following.

## Qt 6

yadiff links Qt 6 (Core, Gui, Widgets, Network, Concurrent) dynamically. Qt is available under
the GNU Lesser General Public License v3 (LGPL-3.0); see https://www.qt.io/licensing/. The Qt
libraries in the zips (DLLs on Windows, `usr/lib` on Linux) are unmodified builds of Qt (the
log's startup lines name the version), and can be replaced with your own build of the same
version. Qt's source code is at
https://download.qt.io/official_releases/qt/.

## KDE Frameworks: KSyntaxHighlighting

Syntax highlighting comes from KSyntaxHighlighting (https://invent.kde.org/frameworks/syntax-highlighting),
under the MIT license. Its syntax definitions carry their own (mostly MIT or LGPL) licenses.

## LLVM C++ runtime

The Windows zip is built with clang (llvm-mingw) and includes LLVM's C++ runtime, `libc++.dll`
and `libunwind.dll`, under the Apache License v2.0 with LLVM Exceptions
(https://llvm.org/LICENSE.txt). The Windows parts come from the MinGW-w64 runtime
(https://www.mingw-w64.org), under permissive licenses.

## Colour themes

The colours of the bundled themes follow well-known editor themes. Only colour values are
used; no code or files were copied.

- VS Code Light+ and Dark+: the default themes of Visual Studio Code, © Microsoft
  Corporation, MIT license (https://github.com/microsoft/vscode).
- Dracula: https://draculatheme.com, MIT license.
- GitHub Light and Dark: GitHub's colour palettes (https://primer.style).
- Vibrant Ink: the TextMate theme of that name.

Product names are trademarks of their owners; yadiff is not affiliated with them.
