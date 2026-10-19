# yadiff

A graphical diff viewer for Git, written in C++20 with Qt 6 Widgets. It runs on Linux and Windows.

```
yadiff                      staged and unstaged changes, compared against HEAD
                           (outside a repository: an empty window; use File > New comparison)
yadiff PATH...              just these files or directories, compared against HEAD
yadiff REV                  REV compared against the working tree
yadiff REV1 REV2            REV1 compared against REV2
yadiff REV1..REV2           same as REV1 REV2
yadiff REV1...REV2          merge-base of REV1 and REV2 compared against REV2
yadiff [REV...] -- PATH...  limit any of the above to PATHs
yadiff -ghpr URL            a GitHub pull request, e.g. https://github.com/OWNER/REPO/pull/12
yadiff -ado URL             an Azure DevOps pull request,
                           e.g. https://dev.azure.com/ORG/PROJECT/_git/REPO/pullrequest/12
yadiff --ado-login [ORG]    sign in to Azure DevOps (--ado-logout ORG forgets the saved token)
yadiff --no-index A B       two directories or two files, in or outside a repository
                           (automatic when A and B aren't both inside the current repository)
yadiff --register-difftool  make yadiff git's difftool (--unregister-difftool undoes it)
yadiff ... --export FILE    write an HTML report of the comparison instead of opening a window

Options: -C <dir>             run as if started in <dir>
         -a, --all            one list of all changes against HEAD, not staged/unstaged
         --algorithm <name>   patience (default), histogram, myers, minimal, myers-lcs or heckel
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
- **Marking.** Mark a file once you've dealt with it: Ctrl+M, or right-click → Mark file. Marked files
  get a highlight colour, which you can change under Marking → Mark highlight color. Marks are saved
  per repository in `~/.config/yadiff/reviewed.ini`. A mark is tied to both versions of the file, so
  it clears itself when either side changes, but survives option changes and swapping sides. The
  Marking menu also has:
  - Mark by pattern, with the same syntax as the filter box.
  - Mark identical files, meaning no differences with the current options.
  - Hide marked files and Select marked files.
  - Ctrl+Shift+M marks a file and moves to the next one.
- **Diff pane** on the right, inline or side by side. One toolbar button toggles between them, and
  Ctrl+T does the same. Both views show editor-style line numbers, and inline shows old and new.
- **Diff algorithm and options.** Set the algorithm from the toolbar drop-down or Options → Diff
  algorithm. Patience (the default), Histogram, Myers and Minimal are git's. Myers LCS and Heckel
  isolation are yadiff's own:
  - Myers LCS is exact Myers (a longest common subsequence), so it gives the fewest changes. It is
    slower on files with very many changes.
  - Heckel isolation anchors on lines that occur once on each side and grows matches from there.
    It is fast and keeps blocks together.

  The Options menu also ignores whitespace changes, all whitespace, leading and trailing
  whitespace, blank lines, CR at end of line, or case. Ignoring case or leading and trailing
  whitespace makes yadiff compare the lines itself, with the chosen algorithm. The left side still
  shows its own text. These two options don't change which files are listed. A file whose only
  differences are ignored says so when you open it. The Options menu can also turn move
  detection off.
- **Comments and unimportant text.** Options → Ignore comments takes comments out before
  comparing. The comment syntax comes from the file type: C-style, `#`, `--`, `;`, `%`, `'` and
  `<!-- -->` languages, with string literals skipped. Options → Unimportant text rules takes a
  list of regular expressions, such as dates, version-control keywords or GUIDs, and Ignore
  unimportant text switches them on or off. Lines that differ only in ignored text show as
  unchanged on a grey background, with each side's own text. Added or removed lines that are
  nothing but ignored text (a new comment, for instance) are greyed with a · in the gutter.
  Neither kind counts as a change for navigation or the +/− counts.
- **Manual alignment.** When the diff lines up the wrong lines, right-click a line, choose Align
  this line with…, then right-click the line on the other side it belongs with and choose Align.
  yadiff compares the stretches above and below separately, so the two lines share a row (marked
  ↔). Add as many alignment points as you like; Remove manual alignment clears them for the file.
- **Syntax highlighting** comes from KDE's KSyntaxHighlighting, which covers about 460 languages.
  The language is detected from the file name. The toolbar's Syntax drop-down overrides it for
  files with the same extension, and "None" turns highlighting off. Builds without the library
  still work, just without highlighting.
- **Collapse unchanged regions** with View → Collapse unchanged regions (Ctrl+Shift+C). It's off
  by default. Long unchanged runs shrink to one row with 3 lines of context kept. Click a row to
  expand it, or use View → Expand all.
- **Overview strip** beside the scrollbar shows every change in the file, search hits, and the
  visible area. Click or drag it to scroll.
- **Find** with F3 or Ctrl+F searches the current Selection, This file, or All changes, by substring
  or regex, optionally case-sensitive. F8 and F7 step to the next and previous match, as do Enter
  and Shift+Enter in the find bar. With All changes, stepping past the last match moves to the next file that has
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
  compare them, or one to compare with the working tree. The drop-down at the top switches to the
  tips of branches and tags, where Changes since HEAD shows what a branch has that HEAD doesn't,
  or to stashes, where Show stash shows a stash's changes.
- **Step through commits.** A comparison of a range, such as `yadiff main feature` or a pull
  request, gets a commit bar above the diff. Its drop-down lists every commit in the range, and the
  arrows (or Ctrl+Alt+PgDown and Ctrl+Alt+PgUp) step from the whole range to each commit in turn.
  A single commit, from the commit picker or blame, steps through its neighbours instead: older
  commits along the first parent, and newer ones on the way to HEAD. Hover the author line for the
  full commit message. While a pull request is on one commit, review comments are hidden.
- **Stage, unstage or discard single changes and lines.** Right-click a change in a working-tree
  comparison for Stage this change, or select some lines (on either side, side by side) for Stage
  selected lines. Staged files offer Unstage instead, and Discard reverts the lines in the working
  tree after asking. Edit → Stage current change or selected lines (Ctrl+Alt+S) and Unstage
  (Ctrl+Alt+Shift+S) do the same from the keyboard. Like git's own line staging, a partly staged
  edit keeps the old lines and adds the chosen new ones after them.
- **Show whitespace characters** with Ctrl+Shift+8, and **word wrap** with Alt+Z. Side-by-side
  rows stay aligned when one side wraps more than the other.
- **Reload automatically when files change on disk** is in the Preferences menu and off by default.
  yadiff checks every 2 seconds and keeps your place. It won't reload over unsaved edits.
- **Compare text.** File → Compare text opens two boxes to paste or type into; the file name you
  give sets the syntax. Edit → Compare clipboard with left (or right) file compares the clipboard
  with the file on screen. The texts are saved under `~/.cache/yadiff/scratch` (removed after a
  week), so both sides can be edited like files.
- **Patch files.** `yadiff --patch FILE`, File → Open patch, or dropping a `.patch` / `.diff` on the
  window shows a patch from `git diff`, `git format-patch` or `diff -u`. When the files it was
  made from are there (in the repository the patch is in, or the current one), each file shows in
  full with the patch applied, even if the file has moved on a little; otherwise just the patch's
  hunks show, with the lines in between marked as not shown. File → Apply patch runs `git apply`
  (optionally also staging, or in reverse) after checking that it applies cleanly. File → Save as
  patch writes any comparison as a patch that `git apply` takes, binary files and untracked files
  included.
- **Recent comparisons and sessions.** File → Recent comparisons reopens the last 20 comparisons.
  File → Save session writes a `.yadiff` file with the comparison, the file shown, the view and the
  diff options; open it with File → Open session, `yadiff --session FILE`, or by dropping it on the
  window. A session's options apply to that window only.
- **Drag and drop.** Drop two files or two folders on the window to compare them, or one, then a
  second. In the empty window the comparison opens in place; otherwise in a new window.
- **File manager menu.** Help → Add to the file manager's menu (or `yadiff --add-file-manager-menu`)
  adds "Compare with yadiff" to Dolphin's right-click menu for two selected items, and "Select as
  left side for yadiff" / "Compare with left side (yadiff)" for one at a time. On Windows, Explorer
  gets the left-side pair for files and folders (under "Show more options" on Windows 11). Help →
  Remove from the file manager's menu takes it out again.
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
- **Review threads**, on GitHub and Azure DevOps PRs, start collapsed to one line under the line they
  belong to. That line shows the author, the local date and time, the reply count, and the gist.
  Click it, or the ▸ in the gutter, to read the thread. The ▾ link collapses it again. View →
  Expand all review comments (Ctrl+Alt+O) and Collapse all (Ctrl+Alt+Shift+O) work on every
  thread at once. An expanded thread is rendered as rich text, with Markdown, the HTML that review
  bots use, links, and code blocks in monospace. The author's name links to the comment on the
  website. HTML comments are hidden, and `<details>` sections fold to their summary. Comment rows
  never affect syntax highlighting, so a `/*` inside a comment can't turn the rest of the file
  into a comment.
- **GitHub review comments.** With `-ghpr`, review threads show under the lines they belong to.
  Right-click a line, or a selection for several lines, to add a comment. Right-click a thread to
  reply. The Review menu approves, requests changes, or comments on the PR. Everything goes
  through `gh` and posts straight away. GitHub only accepts comments on or near changed lines.
  Outdated comments, whose line no longer exists, aren't shown.
- **Azure DevOps pull requests:** `yadiff -ado URL` shows what the PR's Files tab shows, which is the
  source branch against its merge base with the target. Links can be `dev.azure.com`,
  `ORG.visualstudio.com`, or Azure DevOps Server collection URLs. If the current repository has a
  remote for that Azure Repo, the PR is fetched into it, trying your own git credentials first.
  Otherwise yadiff keeps a blob-less partial clone in `~/.cache/yadiff/ado/`. Review threads,
  comments, replies and votes work as for GitHub. Approve votes 10, and Request changes votes −5
  ("waiting for author"). A message becomes a PR-level comment. Signing in is covered in the
  next section.
- **Background loading.** Git runs off the UI thread, so big repositories and PRs don't freeze the
  window, and switching files quickly only shows the last one. "All changes" searches show
  progress and can be stopped. Diffs over 100,000 lines ask before showing, and files over 30,000
  lines skip syntax highlighting.
- **git difftool:** `yadiff --register-difftool`, or File → Register as git difftool, sets
  `difftool.yadiff.cmd` in `~/.gitconfig` and makes yadiff the default tool. Then
  `git difftool` opens yadiff file by file, and `git difftool -d` compares whole directories.
- **Ignore whitespace** is a toolbar button next to the view toggle, also on Ctrl+Shift+W. It is
  the same switch as Options → Ignore all whitespace (`git diff -w`). Lines that differ only in
  whitespace stop counting as changes, and files with only whitespace changes leave the list.
- **Word-level highlights** within edited lines. Similar removed and added lines are paired, so
  side-by-side shows an edited line on one row.
- **Moved blocks** are detected and coloured separately. They get dashed borders, a ⇄ marker in
  the gutter, and a label such as "↓ moved to new lines 120–135". Double-click a moved block to
  jump to its other location.
- **Right-click to copy** a change as a `git diff` hunk. The copy includes the header and 3 lines
  of context, as plain text and as colour-coded rich text for mail, docs or chat. The same menu
  copies just the old or new text of a change, or the whole file as a diff.
- **Navigation:** Alt+Down goes to the next change, and on to the next file after the last one.
  Alt+Up goes back. The arrows at the top right do the same. Alt+PgDown and Alt+PgUp step between
  files, and Ctrl+G goes to a line number (prefix it with L for the left side).
- **GitHub pull requests:** `yadiff -ghpr URL` shows what GitHub's "Files changed" tab shows, which
  is the PR head against the merge base with its base branch. Metadata comes from the `gh` CLI,
  so private repos work when `gh` is logged in. Without `gh`, yadiff falls back to GitHub's pull
  refs. If the current repository has a remote for that GitHub repo, the PR is fetched into it
  without creating branches. Otherwise yadiff keeps a blob-less partial clone in
  `~/.cache/yadiff/github/OWNER/REPO.git`, so file contents are downloaded only when viewed.
- **Colour themes:** View → Color theme has System (the desktop's own style), Dark+ and Light+
  (VS Code), Monokai, Solarized Dark and Light, One Dark and Light, GitHub Light and Dark, Dracula
  and Vibrant Ink. A theme sets the window palette, the syntax colours and the default diff
  colours. File → Switch light / dark (Ctrl+Shift+D) swaps a theme for its counterpart.
- **Fonts:** View → Fonts sets the diff font, the file list font, and the font used for rich-text
  copies. View → Diff colors and tab width overrides the added, removed and moved colours; until
  you do, they follow the theme.
- **Git blame:** View → Git blame (Ctrl+Shift+B) adds a column with the commit, date and author
  of each line, on both sides. Newer commits are tinted more strongly, and lines that aren't
  committed yet say so. Hover for the commit summary; click to open that commit against its
  parent in a new window. Blame runs in the background with the file.
- **Options → Compare unstaged changes against HEAD** is on by default. Turn it off to compare
  against the index, like `git diff`.
- F5 reloads. Settings live in `~/.config/yadiff/yadiff.conf` on Linux and the registry on Windows.

Diffs come from `git diff` with full-file context. Moves, line pairing and word highlights are
computed by yadiff.

## Menus

- **File:**
  - New comparison: a repository, two revisions, two folders, two files, or a PR link. It opens
    in a new window, except in the empty window plain `yadiff` opens outside a repository, which
    shows it in place.
  - Recent comparisons, Compare text, Open patch, Apply patch, Save as patch, Open session and
    Save session.
  - Compare commits, Change window title, and Export HTML report.
  - Globbing ignore patterns (`.gitignore` syntax), which can be turned off.
  - Stage, unstage or revert the selected files. Revert asks first and moves deleted files to the
    trash.
  - Rescan files (Ctrl+F5), Refresh list (F5), and Download files again. For PRs, that fetches
    the PR again and picks up new pushes.
  - Swap right and left (Ctrl+Shift+X). For working-tree comparisons, this puts the working tree
    on the left.
  - Switch light / dark (Ctrl+Shift+D).
- **Edit:**
  - Copy, which keeps syntax colours and diff backgrounds. Copy without markup, Copy as Markdown,
    and Copy for code review, which gives the path and line range, then the code.
  - Select all, copying the left or right file's path, and comparing the clipboard with either
    file.
  - Find (F3), Find next (F8) and Find previous (F7). Highlight clicked word, and Go to line
    (Ctrl+G).
  - Set editor, and Edit left or right file in it. `{file}` and `{line}` are filled in; a commit's
    side opens as a read-only copy.
  - Editing the right side in place, applying a change to either side, staging, unstaging or
    discarding the current change or selected lines, and change, file and commit navigation.
- **View:**
  - Inline or side by side, and Show both, left only or right only.
  - Language, syntax highlighting on or off, show whitespace, show line numbers, word wrap, tab
    width, and zoom (Ctrl++, Ctrl+-, and Ctrl+0 for 100%).
  - Collapse unchanged regions, expand or collapse review comments, and trim the common prefix
    from folder names, which are fully qualified otherwise.
  - Git blame, Color theme, Fonts, and Diff colors and tab width.
- **Options:**
  - Show left-only, right-only, different, identical or blanks-only files, plus Show all and Show
    no files. Identical files are listed only while that option is on.
  - Ignore all whitespace, extra whitespace, leading/trailing whitespace, blank lines, CR at end
    of line, case, comments, and unimportant text (with its rules).
  - Diff algorithm and move detection.
  - All changes in one list, and comparing unstaged changes against HEAD.
  - Automatic reload, and remembering scroll positions per file.
- **Marking:** see above.
- **Review:** for pull requests only.
- **Help:**
  - Register and Unregister as git difftool, and add yadiff to (or remove it from) the file
    manager's menu.
  - Automatically install updates, and Check for update now (see Updates below).
  - Usage, which is the command-line summary, and About.

Right-click files in the list for: Copy (paths), Select all, Copy commit hash, View differences
(Enter), View filelog in gitk, Mark file, Stage file, Revert file, Download file again, and Refresh
list. Several files can be selected.

## Updates

Help → Check for update now looks up the latest GitHub release. If it is newer, yadiff shows the
release notes and asks before installing it. Help → Automatically install updates (off by
default) checks once a day, a few seconds after start-up, and installs without asking. Either way,
the new version starts the next time yadiff does. A "Restart now" button in the status bar starts
it straight away.

Releases are read with the `gh` CLI, which handles sign-in for the private repository. Without
`gh`, yadiff uses GitHub's REST API, sending `GH_TOKEN` or `GITHUB_TOKEN` when one is set.
Installing depends on how yadiff was installed:

- **Linux, installed with `install.sh`:** releases have no Linux binaries, so yadiff builds the
  release's source, runs its tests, and installs it as a new `~/.local/opt/yadiff-<tag>` snapshot.
  Then it moves the `~/.local/bin/yadiff` link to that snapshot. This needs the same build tools
  as `install.sh`. Building against this machine's own Qt keeps yadiff working across Qt updates.
  The old snapshot stays, so going back is a matter of moving the link.
- **Windows, an unzipped folder:** yadiff downloads `yadiff-windows-x64.zip` and replaces its files
  in place. Files in use are renamed to `*.old` first and deleted on the next start.
- **Anything else,** such as a package manager or a build tree: yadiff says a release is available
  and links to it, but doesn't install it.

## Signing in to Azure DevOps

yadiff looks for a credential in this order and uses the first that works:

1. **Environment variables.** `YADIFF_ADO_PAT` or `AZURE_DEVOPS_EXT_PAT` hold a personal access
   token. `YADIFF_ADO_TOKEN` or `SYSTEM_ACCESSTOKEN` hold a bearer token, such as the one in a
   pipeline.
2. **A personal access token saved by yadiff** in the system keyring. On Linux that's the Secret
   Service, through `secret-tool`, so KDE Wallet or GNOME Keyring. On Windows it's the Credential
   Manager.
3. **The Azure CLI** (`az login`), if it's installed. Its Microsoft Entra token is used.
4. **Git's credential helper** for the organization, such as Git Credential Manager. It's only
   asked when one is configured for that URL.

If none works, or Azure DevOps rejects the credential, `yadiff -ado` opens a sign-in dialog. Paste a
token created at `https://dev.azure.com/ORG/_usersSettings/tokens` with the **Code: Read & write**
scope. Read alone is enough to view; commenting and voting need write. yadiff checks the token with
Azure DevOps before saving it. With the Azure CLI installed, the dialog also offers a browser
sign-in. Run `yadiff --ado-login ORG-URL` to sign in ahead of time.

Tokens are never put on a command line. Git gets them through `GIT_CONFIG_*` environment variables
as an `http.extraHeader`. yadiff refuses to send credentials over plain HTTP, except to
`localhost`, which the tests use.

## Building

### Linux

Needs Qt 6.5 or newer (`qt6-base`), CMake, Ninja and a C++20 compiler. Syntax highlighting also
needs KSyntaxHighlighting (`syntax-highlighting` on Arch), which is optional.

```
./install.sh
```

This builds into `build/` and runs the tests. It then installs a snapshot to
`~/.local/opt/yadiff-<version>`, where the version comes from `git describe`, such as `v0.1.0`. It
points `~/.local/bin/yadiff` at that snapshot. Development builds in `build/` therefore never change
the installed yadiff, which is also what `git difftool` runs, until you run `./install.sh` again.
It also installs the icon and a hidden desktop entry, which KDE on Wayland uses to show the window
icon.

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

- `test_diffmodel` covers the diff parser, moves, pairing and word highlights. It also checks
  yadiff's own algorithms: Myers LCS against a brute-force LCS on random input, and Heckel. It also
  covers comment stripping, unimportant lines, alignment around manual anchors, and reading and
  applying patch files.
- `test_format` covers the copied diff text and HTML, PR link parsing, the review store, search,
  applying a change to a file, line-ending preservation, sessions, the file manager menu, and
  difftool registration. That last one uses a temporary git config.
- `test_gitrepo` runs against a temporary repository. It covers staged, unstaged and untracked
  files, a repository with no commits, revision mode, path filters, all-changes mode, diff
  options (including ignore case and leading/trailing whitespace with every algorithm, comments,
  unimportant text and manual alignment), review
  fingerprints, line counts, directory and file comparison, `/dev/null` sides, blame, staging,
  unstaging and discarding single lines, patch comparisons, and saving comparisons as patches.
- `test_ado` runs Azure DevOps PRs against a small fake Azure DevOps server and a local bare
  repository. It covers link and remote parsing, credential order, keyring storage, rejected
  credentials, fetching, threads, comments, replies, votes, and the real `yadiff -ado` command
  line. The keyring is redirected to a temporary file there.
- `test_update` covers version comparison, telling an `install.sh` snapshot from other installs,
  replacing files in use the Windows way, and reading the latest release from a fake GitHub API.
- `test_navigation` drives the real window offscreen. It checks change navigation across files, the
  view toggle, review marks, right-click copy, and that unchanged lines never inherit a highlight.
  It also covers folds, find in all three scopes, editing and saving (with highlighting on), applying
  a change to disk, wrap alignment, auto-reload, the commit picker, and that no two actions share a
  keyboard shortcut. Background loading is tested too, including switching files quickly. So are
  the file filter, image diffs, review-thread placement, the large-diff notice, themes, blame,
  ignoring comments, manual alignment, staging the current change, stepping through commits, and
  dropped files, patches, sessions and recent comparisons.
  The other GUI
  tests run with `YADIFF_SYNCHRONOUS=1`, so loads finish before the next line.

To look at the UI without a display, render every file in both modes to PNG:

```
QT_QPA_PLATFORM=offscreen YADIFF_GRAB=/tmp/shot yadiff --wait
```

## Layout

| Path | What |
|---|---|
| `src/DiffModel.*` | Unified-diff parser, chunks, move detection, line pairing, word highlights |
| `src/LineDiff.*` | yadiff's own line diff: Myers LCS, Heckel, diffs of normalised lines, manual anchors |
| `src/Comments.*` | Comment syntax by file type, and stripping comments for Ignore comments |
| `src/PatchFile.*` | Reading unified diffs and patch files, and applying them |
| `src/Session.*`, `src/Scratch.*` | Saved sessions; scratch files for pasted text and the clipboard |
| `src/ShellMenu.*` | Dolphin service menu / Explorer verbs, and the remembered left side |
| `src/GitRepo.*` | Runs `git`, lists changed files, loads one file's diff, fingerprints file versions |
| `src/GitHubPr.*` | Parses PR links, resolves them with `gh`, fetches them |
| `src/DiffFormat.*` | Unified-diff text and coloured HTML for copying |
| `src/Search.*`, `src/SearchBar.*` | Find: query matching, and the find bar |
| `src/TextFile.*` | Reading and writing on-disk sides, applying a change to a file |
| `src/Syntax.*` | KSyntaxHighlighting wrapper (no-op without it) |
| `src/Theme.*` | Colour themes: window palette, syntax theme, default diff colours |
| `src/OverviewBar.*` | The change overview strip |
| `src/HistoryDialog.*` | Commit picker |
| `src/Difftool.*` | git difftool registration |
| `src/FileFilter.*` | File list filter terms |
| `src/ImageDiffView.*` | Image comparison modes |
| `src/Report.*` | HTML report |
| `src/GitHubReview.*` | PR review comments through `gh` |
| `src/AdoPr.*`, `src/AdoAuth.*`, `src/AdoLoginDialog.*` | Azure DevOps PRs, credentials, sign-in |
| `src/PrReview.*` | Review operations for whichever host a PR is on |
| `src/Http.*`, `src/SecretStore.*` | HTTP client, system keyring |
| `src/Updater.*` | Update checks and installs from GitHub releases |
| `src/ReviewStore.*` | Persisted review marks |
| `src/CodeView.*` | Text view: gutter, move annotations, folds, search hits, wrap padding, editing |
| `src/DiffPane.*` | Inline and side-by-side layout, change navigation |
| `src/FileList.*` | Directory-grouped file pane |
| `src/MainWindow.*` | Menus, toolbar, file operations, background loading |
| `src/NewComparisonDialog.*` | File > New comparison |
| `src/ComparisonArgs.*` | Command-line arguments to a repository and comparison (also used by New comparison) |
| `src/PreferencesDialog.*`, `src/Settings.*` | Colours, font, persisted options |
| `resources/syntax-themes/` | Syntax themes KSyntaxHighlighting doesn't ship (Light+, Vibrant Ink) |
| `resources/` | Icon (SVG source, PNG sizes, Windows `.ico`/`.rc`) and desktop entry |
