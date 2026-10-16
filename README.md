# yadiff

A graphical diff viewer for Git, written in C++20 with Qt 6 Widgets. It runs on Linux and Windows.

```
yadiff                      staged and unstaged changes, compared against HEAD
yadiff PATH...              just these files or directories, compared against HEAD
yadiff REV                  REV compared against the working tree
yadiff REV1 REV2            REV1 compared against REV2
yadiff REV1..REV2           same as REV1 REV2
yadiff REV1...REV2          merge-base of REV1 and REV2 compared against REV2
yadiff [REV...] -- PATH...  limit any of the above to PATHs
yadiff -ghpr URL            a GitHub pull request, e.g. https://github.com/OWNER/REPO/pull/12
yadiff --no-index A B       two directories or two files, in or outside a repository
                           (automatic when A and B aren't both inside the current repository)
yadiff --register-difftool  make yadiff git's difftool (--unregister-difftool undoes it)
yadiff ... --export FILE    write an HTML report of the comparison instead of opening a window

Options: -C <dir>             run as if started in <dir>
         -a, --all            one list of all changes against HEAD, not staged/unstaged
         --algorithm <name>   patience (default), histogram, myers or minimal
         --wait               stay attached to the terminal until the window closes
         --inline / --side-by-side   initial view mode
```

`yadiff` returns to the prompt immediately and the window opens on its own, like `gitk &`.
Arguments that resolve as commits are revisions, and the rest are paths. Use `--` when a
path looks like a revision.

## Features

- **File pane** on the left. Files are grouped by directory, with directory paths relative to the
  repository root (`.` is the root) and bare file names. A coloured badge shows the status: M, A,
  D, R, C, T, U, or ? for untracked.
- **Staged and unstaged, or all changes.** The toolbar's left drop-down switches between separate
  staged and unstaged sections and one list of the working tree against HEAD. Untracked files are
  included in both. `yadiff PATH` starts in the single-list mode.
- **Review checkboxes.** Tick a file once you've reviewed it, or tick a directory to tick all its
  files. Ctrl+M toggles the current file, and Ctrl+Shift+M marks it and moves to the next one.
  Marks are saved per repository in `~/.config/yadiff/reviewed.ini`. A mark is tied to both
  versions of the file, so it clears itself when either side changes. Switching diff options
  doesn't clear it. The status bar shows how many files are reviewed.
- **Diff pane** on the right, inline or side by side. One toolbar button toggles between them, and
  Ctrl+T does the same. Both views show editor-style line numbers, and inline shows old and new.
- **Diff algorithm and options.** The algorithm can be Patience (the default), Histogram, Myers or
  Minimal, set from the toolbar drop-down or the Diff menu. The Diff menu also ignores whitespace changes, all
  whitespace, blank lines or CR at end of line. It can also turn move detection off.
- **Syntax highlighting** comes from KDE's KSyntaxHighlighting, which covers about 460 languages.
  The language is detected from the file name. The toolbar's Syntax drop-down overrides it for
  files with the same extension, and "None" turns highlighting off. Builds without the library
  still work, just without highlighting.
- **Collapse unchanged regions** with View → Collapse unchanged regions (Ctrl+Shift+C). It's off
  by default. Long unchanged runs shrink to one row with 3 lines of context kept. Click a row to
  expand it, or use View → Expand all.
- **Overview strip** beside the scrollbar shows every change in the file, search hits, and the
  visible area. Click or drag it to scroll.
- **Find** with Ctrl+F searches the current Selection, This file, or All changes, by substring or
  regex, optionally case-sensitive. Enter and Shift+Enter, or Ctrl+G and Ctrl+Shift+G, step
  through matches. With All changes, stepping past the last match moves to the next file that has
  matches, and those files are shown in bold.
- **Per-file line counts** such as "+12 −3" appear in the file list when there is room, and totals
  appear in the status bar.
- **Edit the working-tree side** with Ctrl+E or the Edit button, then save with Ctrl+S. Editing
  happens in side-by-side view. The file keeps its line endings and final newline, and the diff
  refreshes on save. You're asked to save or discard before switching files or closing.
- **Apply a change to the other side** from the right-click menu, or with Ctrl+Alt+Right and
  Ctrl+Alt+Left. This rewrites the file on disk. That works for the working-tree side in a Git
  comparison, and both sides in a directory or file comparison. It refuses if the file changed
  since it was diffed.
- **Compare commits** with File → Compare commits (Ctrl+L) or the toolbar button. It lists recent
  history, optionally from all branches, with a filter. Pick one commit to see its changes, two to
  compare them, or one to compare with the working tree.
- **Show whitespace characters** with Ctrl+Shift+8, and **word wrap** with Alt+Z. Side-by-side
  rows stay aligned when one side wraps more than the other.
- **Reload automatically when files change on disk** is in the Preferences menu and off by default.
  yadiff checks every 2 seconds and keeps your place. It won't reload over unsaved edits.
- **Directory and file comparison** works outside Git too, using `git diff --no-index`. Every diff
  option applies. Both sides can be edited, and changes can be applied in either direction.
- **File filter** sits above the file list (Ctrl+P). Terms are separated by spaces. Text matches
  anywhere in the path, globs like `*.cpp` match names, and `!` excludes, as in `src !*.lock`. The
  Show menu hides reviewed files or whole statuses, such as deleted or untracked. Next and previous
  skip hidden files.
- **Image diff** applies to changed PNG, JPEG, GIF, BMP, WebP, ICO and TIFF files. It has four
  modes: side by side, swipe with a draggable divider, onion skin with an opacity slider, and
  difference with changed pixels in red. It shows dimensions and file sizes, and switches between
  fit and 100%.
- **HTML report** comes from File → Export HTML report (Ctrl+Shift+E) or `--export FILE`. It's one
  self-contained page with a summary table, every file's diff with 3 lines of context, word
  highlights, moved lines, review ticks and embedded images. It follows the browser's light or
  dark mode.
- **GitHub review comments.** With `-ghpr`, review threads show under the lines they belong to.
  Right-click a line, or a selection for several lines, to add a comment. Right-click a thread to
  reply. The Review menu approves, requests changes, or comments on the PR. Everything goes
  through `gh` and posts straight away. GitHub only accepts comments on or near changed lines.
  Outdated comments, whose line no longer exists, aren't shown.
- **Background loading.** Git runs off the UI thread, so big repositories and PRs don't freeze the
  window, and switching files quickly only shows the last one. "All changes" searches show
  progress and can be stopped. Diffs over 100,000 lines ask before showing, and files over 30,000
  lines skip syntax highlighting.
- **git difftool:** `yadiff --register-difftool`, or File → Register as git difftool, sets
  `difftool.yadiff.cmd` in `~/.gitconfig` and makes yadiff the default tool. Then
  `git difftool` opens yadiff file by file, and `git difftool -d` compares whole directories.
- **Ignore whitespace** is a toolbar button next to the view toggle, also on Ctrl+Shift+W. It is
  the same switch as Diff → Ignore all whitespace (`git diff -w`). Lines that differ only in
  whitespace stop counting as changes, and files with only whitespace changes leave the list.
- **Word-level highlights** within edited lines. Similar removed and added lines are paired, so
  side-by-side shows an edited line on one row.
- **Moved blocks** are detected and coloured separately. They get dashed borders, a ⇄ marker in
  the gutter, and a label such as "↓ moved to new lines 120–135". Double-click a moved block to
  jump to its other location.
- **Right-click to copy** a change as a `git diff` hunk. The copy includes the header and 3 lines
  of context, as plain text and as colour-coded rich text for mail, docs or chat. The same menu
  copies just the old or new text of a change, or the whole file as a diff.
- **Navigation:** F3 goes to the next change and moves to the next file after the last change.
  Shift+F3 goes back. The arrows at the top right do the same. Ctrl+F3 and Ctrl+Shift+F3 step
  between files.
- **GitHub pull requests:** `yadiff -ghpr URL` shows what GitHub's "Files changed" tab shows, which
  is the PR head against the merge base with its base branch. Metadata comes from the `gh` CLI,
  so private repos work when `gh` is logged in. Without `gh`, yadiff falls back to GitHub's pull
  refs. If the current repository has a remote for that GitHub repo, the PR is fetched into it
  without creating branches. Otherwise yadiff keeps a blob-less partial clone in
  `~/.cache/yadiff/github/OWNER/REPO.git`, so file contents are downloaded only when viewed.
- **Preferences → Colors and Font** sets the added, removed and moved colours, the font and the
  tab width. Default colours follow the light or dark theme until you override them.
  **Compare unstaged changes against HEAD** is on by default. Turn it off to compare against the
  index, like `git diff`.
- F5 reloads. Settings live in `~/.config/yadiff/yadiff.conf` on Linux and the registry on Windows.

Diffs come from `git diff` with full-file context. Moves, line pairing and word highlights are
computed by yadiff.

## Building

### Linux

Needs Qt 6.5 or newer (`qt6-base`), CMake, Ninja and a C++20 compiler. Syntax highlighting also
needs KSyntaxHighlighting (`syntax-highlighting` on Arch), which is optional.

```
./install.sh
```

This builds into `build/`, runs the tests, and symlinks `~/.local/bin/yadiff` to the built binary. It also
installs the icon and a hidden desktop entry, which KDE on Wayland uses to show the window icon.

### Windows

A ready-to-run build is attached to every CI run: the `yadiff-windows-x64` artifact. Version tags
(`v*`) also attach it to a GitHub release. Unzip it anywhere and put the folder on `PATH`. Git for
Windows must be installed. To build it yourself:

Install Qt 6 (MSVC 2022 64-bit kit) with the Qt online installer, plus CMake, Ninja and Git for
Windows. For syntax highlighting, also build KSyntaxHighlighting, with KDE Craft or vcpkg, and add it
to `CMAKE_PREFIX_PATH`. Without it, CMake prints a note and yadiff builds without highlighting. From a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.x.x\msvc2022_64
cmake --build build
C:\Qt\6.x.x\msvc2022_64\bin\windeployqt.exe build\yadiff.exe
```

Put the `build` folder on `PATH`. `yadiff.exe` is a GUI-subsystem program, so the console returns
immediately and errors are shown in a dialog. The Windows build has not been tested yet.

## Continuous integration

`.github/workflows/ci.yml` builds and runs every test on Arch Linux, the same packages as the
development machine, and on Windows with MSVC and Qt 6.8. The Windows job builds
KSyntaxHighlighting from source and caches it. It then packages `yadiff.exe` with `windeployqt`,
and smoke-tests the zip on a clean `PATH` with a headless `--export`.

## Tests

```
ctest --test-dir build --output-on-failure
```

- `test_diffmodel` covers the diff parser, moves, pairing and word highlights.
- `test_format` covers the copied diff text and HTML, PR link parsing, the review store, search,
  applying a change to a file, line-ending preservation, and difftool registration. That last one
  uses a temporary git config.
- `test_gitrepo` runs against a temporary repository. It covers staged, unstaged and untracked
  files, a repository with no commits, revision mode, path filters, all-changes mode, diff
  options, review fingerprints, line counts, directory and file comparison, and `/dev/null` sides.
- `test_navigation` drives the real window offscreen. It checks F3 and Shift+F3 across files, the
  view toggle, review marks, right-click copy, and that unchanged lines never inherit a highlight.
  It also covers folds, find in all three scopes, editing and saving (with highlighting on), applying
  a change to disk, wrap alignment, auto-reload, the commit picker, and that no two actions share a
  keyboard shortcut. Background loading is tested too, including switching files quickly. So are
  the file filter, image diffs, review-thread placement, and the large-diff notice. The other GUI
  tests run with `YADIFF_SYNCHRONOUS=1`, so loads finish before the next line.

To look at the UI without a display, render every file in both modes to PNG:

```
QT_QPA_PLATFORM=offscreen YADIFF_GRAB=/tmp/shot yadiff --wait
```

## Layout

| Path | What |
|---|---|
| `src/DiffModel.*` | Unified-diff parser, chunks, move detection, line pairing, word highlights |
| `src/GitRepo.*` | Runs `git`, lists changed files, loads one file's diff, fingerprints file versions |
| `src/GitHubPr.*` | Parses PR links, resolves them with `gh`, fetches them |
| `src/DiffFormat.*` | Unified-diff text and coloured HTML for copying |
| `src/Search.*`, `src/SearchBar.*` | Find: query matching, and the find bar |
| `src/TextFile.*` | Reading and writing on-disk sides, applying a change to a file |
| `src/Syntax.*` | KSyntaxHighlighting wrapper (no-op without it) |
| `src/OverviewBar.*` | The change overview strip |
| `src/HistoryDialog.*` | Commit picker |
| `src/Difftool.*` | git difftool registration |
| `src/FileFilter.*` | File list filter terms |
| `src/ImageDiffView.*` | Image comparison modes |
| `src/Report.*` | HTML report |
| `src/GitHubReview.*` | PR review comments through `gh` |
| `src/ReviewStore.*` | Persisted review marks |
| `src/CodeView.*` | Text view: gutter, move annotations, folds, search hits, wrap padding, editing |
| `src/DiffPane.*` | Inline and side-by-side layout, change navigation |
| `src/FileList.*` | Directory-grouped file pane |
| `src/MainWindow.*` | Menus, toolbar, F3 logic |
| `src/PreferencesDialog.*`, `src/Settings.*` | Colours, font, persisted options |
| `resources/` | Icon (SVG source, PNG sizes, Windows `.ico`/`.rc`) and desktop entry |
