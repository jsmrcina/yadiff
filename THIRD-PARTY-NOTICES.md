# Third-party notices

yadiff itself is under the MIT license (see `LICENSE`). It uses, and the Windows zip ships,
the following.

## Qt 6

yadiff links Qt 6 (Core, Gui, Widgets, Network, Concurrent) dynamically. Qt is available under
the GNU Lesser General Public License v3 (LGPL-3.0); see https://www.qt.io/licensing/. The Qt
DLLs in the Windows zip are unmodified builds of Qt (the log's startup lines name the
version), and can be replaced with your own build of the same version. Qt's source code is at
https://download.qt.io/official_releases/qt/.

## KDE Frameworks: KSyntaxHighlighting

Syntax highlighting comes from KSyntaxHighlighting (https://invent.kde.org/frameworks/syntax-highlighting),
under the MIT license. Its syntax definitions carry their own (mostly MIT or LGPL) licenses.

## Microsoft Visual C++ runtime

The Windows zip includes the Visual C++ runtime DLLs, redistributed app-locally as Microsoft's
Visual Studio license terms allow.

## Colour themes

The colours of the bundled themes follow well-known editor themes. Only colour values are
used; no code or files were copied.

- VS Code Light+ and Dark+: the default themes of Visual Studio Code, © Microsoft
  Corporation, MIT license (https://github.com/microsoft/vscode).
- Dracula: https://draculatheme.com, MIT license.
- GitHub Light and Dark: GitHub's colour palettes (https://primer.style).
- Vibrant Ink: the TextMate theme of that name.

Product names are trademarks of their owners; yadiff is not affiliated with them.
