# yadiff

A graphical diff viewer for Git, written in C++20 with Qt 6 Widgets. It runs on Linux and Windows.

```
yadiff                 staged and unstaged changes, compared against HEAD
yadiff REV             REV compared against the working tree
yadiff REV1 REV2       REV1 compared against REV2
yadiff REV1..REV2      same as REV1 REV2
yadiff REV1...REV2     merge-base of REV1 and REV2 compared against REV2

Options: -C <dir>  run as if started in <dir>
         --wait    stay attached to the terminal until the window closes
         --inline / --side-by-side   initial view mode
```

`yadiff` returns to the prompt immediately and the window opens on its own, like `gitk &`.

## Features

- **File pane** on the left. Files are grouped by directory, with directory paths relative to the
  repository root (`.` is the root) and bare file names. Staged and unstaged files are listed in
  separate sections. A coloured badge shows the status: M, A, D, R, C, T, U, or ? for untracked.
- **Diff pane** on the right, inline (Ctrl+1) or side by side (Ctrl+2). Both show editor-style
  line numbers. Inline shows old and new numbers.
- **Word-level highlights** within edited lines. Similar removed and added lines are paired, so
  side-by-side shows an edited line on one row.
- **Moved blocks** are detected and coloured separately. They get dashed borders, a ⇄ marker in
  the gutter, and a label such as "↓ moved to new lines 120–135". Double-click a moved block to
  jump to its other location. Matching ignores leading and trailing whitespace, so re-indented
  code still counts as moved. A block needs at least 20 non-blank characters.
- **Navigation:** F3 goes to the next change and moves to the next file after the last change.
  Shift+F3 goes back. The arrows at the top right do the same. Ctrl+F3 and Ctrl+Shift+F3 step
  between files.
- **Preferences → Colors and Font** sets the added, removed and moved colours, the font and the
  tab width. Default colours follow the light or dark theme until you override them.
- **Preferences → Compare unstaged changes against HEAD** is on by default. Turn it off to see
  unstaged changes against the index, like `git diff`.
- F5 reloads. Settings live in `~/.config/yadiff/yadiff.conf` on Linux and the registry on Windows.

Diffs come from `git diff --histogram` with full-file context. Moves, line pairing and word
highlights are computed by yadiff.

## Building

### Linux

Needs Qt 6.5 or newer (`qt6-base`), CMake, Ninja and a C++20 compiler.

```
./install.sh
```

This builds into `build/`, runs the tests, and symlinks `~/.local/bin/yadiff` to the built binary.

### Windows

Install Qt 6 (MSVC 2022 64-bit kit) with the Qt online installer, plus CMake, Ninja and Git for
Windows. From a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.x.x\msvc2022_64
cmake --build build
C:\Qt\6.x.x\msvc2022_64\bin\windeployqt.exe build\yadiff.exe
```

Put the `build` folder on `PATH`. `yadiff.exe` is a GUI-subsystem program, so the console returns
immediately and errors are shown in a dialog. The Windows build has not been tested yet.

## Tests

```
ctest --test-dir build --output-on-failure
```

- `test_diffmodel` covers the diff parser, moves, pairing and word highlights.
- `test_gitrepo` runs against a temporary repository. It covers staged, unstaged and untracked
  files, a repository with no commits, and revision mode.
- `test_navigation` drives the real window offscreen and checks F3 and Shift+F3 across files.

To look at the UI without a display, render every file in both modes to PNG:

```
QT_QPA_PLATFORM=offscreen YADIFF_GRAB=/tmp/shot yadiff --wait
```

## Layout

| Path | What |
|---|---|
| `src/DiffModel.*` | Unified-diff parser, chunks, move detection, line pairing, word highlights |
| `src/GitRepo.*` | Runs `git`, lists changed files, loads one file's diff |
| `src/CodeView.*` | Read-only text view with line-number gutter and move annotations |
| `src/DiffPane.*` | Inline and side-by-side layout, change navigation |
| `src/FileList.*` | Directory-grouped file pane |
| `src/MainWindow.*` | Menus, toolbar, F3 logic |
| `src/PreferencesDialog.*`, `src/Settings.*` | Colours, font, persisted options |
