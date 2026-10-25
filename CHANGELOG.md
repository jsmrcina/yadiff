# Changelog

## 1.0.0

The first public release.

- **A Linux zip.** `yadiff-linux-x64.zip` brings its own Qt and KSyntaxHighlighting, so it runs on
  any x86-64 distribution as new as Ubuntu 22.04 with only git installed. Unzip it and run
  `yadiff/yadiff`, or link that script into a folder on `PATH`.
- **MIT license**, with the parts the zips ship listed in `THIRD-PARTY-NOTICES.md` (also in both
  zips).
- Releases attach their zips even when CI's artifact storage is full.

## 0.20.0 (2026-10-24)

- **Next / Previous change start where you click.** Click a line, or move there with the cursor keys. Next then goes to the first change below it and Previous to the last one above. Clicking inside a change makes it the current one. Between changes, the label shows "Between changes 2 and 3 of 5".
- **`--help` on Windows.** `yadiff --help`, `--version` and error messages now print in the console yadiff was started from. The prompt comes back first. In PowerShell, `yadiff --help | Out-Host` waits for the text.

## 0.19.1 (2026-10-21)

### Changed

- **Next and Previous change** look like the rest of the toolbar again: an arrow and the word, without 0.19.0's accent fill, with "Change 2 of 5" between them.

## 0.19.0 (2026-10-21)

### New

- **Options → Settings and keyboard shortcuts (Ctrl+,).**
  - **Options tab:** every option from the menus in one place, applied at once. Options with their own dialog (fonts, colours, the editor, patterns) are buttons.
  - **Keyboard shortcuts tab:** every command by menu, with its shortcut and the default. Set a shortcut and an alternative, remove one with None, put one back with Default, or Reset all. A key another command has is moved after asking.
  - Shortcuts apply with OK or Apply, to every open window, and are saved.
  - The search box filters both tabs.
- **Next and Previous change stand out.** They are filled buttons with words, with "Change 2 of 5" between them.

### Fixed

- **Line numbers follow a new colour theme.** They kept the previous theme's colours.
- **Find goes round.** Find next and previous wrap within the file, and searching starts at the cursor.
  - With a word selected, Ctrl+F searches the file for it, with that instance as the current match.
  - Find opens on This file. It searches just the selection only when whole lines are selected.
- **Re-approving on Azure DevOps works.** Azure DevOps ignores a repeated vote, so voting the vote you already have (approving again after a push, say) now resets it, waits for the reset to show, then votes.

## 0.18.0 (2026-10-20)

### New

- **Thread status.**
  - Review threads show their state, and resolved ones are greyed with a ✓.
  - Right-click a thread to resolve or reactivate it. The Thread status submenu has every state the host has: on Azure DevOps, active, pending, resolved, won't fix, by design and closed; on GitHub, unresolved and resolved.
  - View → Hide resolved review threads leaves only the open ones.
  - The file list shows each file's thread count (💬 2), bold while any is open.
- **Changes since your last review.** A toolbar picker next to Review switches between all of a PR's changes, those since your last review, and those since any earlier push. Review → Compare pushes compares any two.
  - Your last review is the push you last voted on, from Azure DevOps' own vote records, so votes cast in the browser count. On GitHub it's the commit of your last review.
  - A PR with new pushes since then opens at just those. Review → Open at the changes since my last review turns that off.
- **File → Open pull request (Ctrl+Shift+O)** lists the PRs waiting for your review:
  - On Azure DevOps, active PRs where you or one of your teams is a reviewer, in every project you've opened a PR from, or added by hand.
  - On GitHub, when `gh` is installed, PRs that request your review.

  Each row shows your vote and whether the PR is new or has a new push since you last opened it. You can also paste any PR's link.
- **Pull request overview** above the file list:
  - the description, and the reviewers with their votes;
  - policies and checks, and work items;
  - the PR-level comments, with reply and resolve links.

  Its header sums it up ("2 approved · 1 check failed") when folded. View → Pull request overview turns it off.

### Faster in large repositories

- The Azure CLI token is fetched once and reused until shortly before it expires, instead of separately for comments, votes and the rest.
- Staging, reverting, marking files, Copy commit hash, Save as patch and the Compare commits merge base no longer block the window.
- **Freeze watchdog:** when the window stops responding for a second, the log records `FROZEN: …` with the command it was waiting for and your last action, and records again when the window recovers.


## 0.17.1 (2026-10-19)

### Fixes

- **Compare commits no longer freezes the window in large repositories.** The commit list used to load before the dialog appeared. In a very large repository that took minutes.
  - The dialog now opens at once and shows "Loading commits…" while git works in the background. The same goes for Load more and the branches, tags and stashes lists.
  - Closing the dialog, or switching lists while one is loading, stops that git command instead of leaving it running.


## 0.17.0 (2026-10-19)

### The overview strip is the scrollbar

- **No more vertical scrollbar** beside the overview strip in the diff views, like most editors.
- **The strip is three times as wide**, so changes on each side are easier to see and aim at.
- **It scrolls like a scrollbar:** turn the wheel over it, drag the outlined visible area like a thumb, or click anywhere else on it to jump there.


## 0.16.1 (2026-10-19)

### Fixes

- **Azure DevOps pull requests open without a clone, even in huge repositories.**
  - yadiff used to fetch the PR's commits into a cache clone. For a very large repository that meant downloading the commits' whole history, so it never finished.
  - Without a clone, yadiff now asks the REST API for the PR's changed files and downloads only those, both versions.
  - The PR shows as two read-only folders, with comments and votes as before.
  - A download is reused until the PR gets new pushes, and removed after a week.
  - Inside a clone, yadiff still fetches into it, as before.
- **Closing the window stops git commands still running for it,** so an abandoned fetch no longer keeps yadiff and git running in the background.


## 0.16.0 (2026-10-19)

### Voting on pull requests

- **See your vote.** The status bar shows your vote on the PR, such as "Your vote: Approved", with a coloured dot. Clicking it, or using the Review menu, changes it.
- **GitHub:** Approve, or Request changes (which needs a message).
- **Azure DevOps:** Approve, Approve with suggestions, Wait for author, Reject, or Reset vote.
- **Messages:** each vote asks for an optional message. On Azure DevOps it's posted as a comment on the PR.
- **Voted in the browser?** "Check my vote again" re-reads it from the host.

### Marking

- Marked files get a stripe of the mark colour down their left edge. A marked file that is also selected gets a frame of it, so it no longer looks unmarked while highlighted.


## 0.15.0 (2026-10-19)

### Opening slow comparisons

- **The window opens at once for a pull request link.** The PR is fetched in the window, which shows each step (signing in, reading the PR, fetching) and a progress bar. An Azure DevOps sign-in, if needed, appears over the window.
- **The file list appears as soon as git has the names.** For commits and pull requests, it no longer waits for the "+12 −3" line counts. In a large virtualized repository, counting lines took 19 s for a 15-file PR that listed in 0.6 s.
- **Every file's diff loads in the background** right after that, four at a time, in list order:
  - In the file list, a hollow dot means waiting, a spinner means loading, and the line counts appear once a file is loaded. A red "!" marks one that failed.
  - The status bar shows "Loading diffs 4 of 36".
  - Picking a file switches to it immediately and shows it the moment its diff arrives. Loaded files show instantly.
- **Any other slow file** shows "Loading…" after 0.15 s instead of leaving the previous file on screen.
- **Fix:** a window opened from File → New comparison could show the previous pull request instead of the one asked for.


## 0.14.1 (2026-10-19)

### Fixes

- **Pull requests in large repositories open again.** Since 0.9.0, the commit bar walked the repository's history before the window appeared. For a one-commit pull request, that meant the target branch's last 300 commits plus the path to HEAD. In a very large repository that takes minutes, so the window never showed. The bar is now built in the background, and a one-commit pull request no longer gets one, since its neighbours are just the target branch's history.
- **No more surprise downloads into yadiff's cache.** Checking whether the cache already had a PR's commit made git fetch it on the spot, with your own git credentials and with its whole history. yadiff now does that fetch itself, with the pull request's credential, and logs it.
- **Hangs show up in the log.** A command that runs long now logs `still running after N s` at 10 s, 30 s, 60 s and then every minute, instead of appearing only when it ends.


## 0.14.0 (2026-10-19)

### Detailed logging

yadiff now keeps a log of what it does, so problems can be traced after the fact.

- **Where:** `~/.cache/yadiff/yadiff.log` on Linux, `%LOCALAPPDATA%\cache\yadiff\yadiff.log` on Windows. **Help > Open log file** opens it, and **Help > About** shows the path. Every yadiff process appends to it, and each line names its process.
- **What, by default:**
  - startup details (version, command line, OS, Qt and git versions);
  - every git, gh, az, secret-tool and git apply command, with exit status, time and error output when it fails;
  - every HTTP request, with status and time. A failed one also gets the Azure DevOps/GitHub request ids and the error body;
  - each step of opening a GitHub or Azure DevOps pull request: the credential source, your clone or the cache, fetch attempts, and where it failed;
  - saves, merges, staging, reverts and deletions;
  - message boxes and status messages, and slow diffs;
  - crashes, with a backtrace on Linux.
- **More detail:** **Help > Detailed logging** or `--log-level debug` adds error output from commands that worked, all response headers, every diff's timing and the actions you take.
- **Elsewhere:** `--log FILE` or `YADIFF_LOG` writes the log somewhere else. Use `-` for the terminal, or `off` for no log.
- **No secrets:** a command's input and output are never logged. Every line is scrubbed of the credentials yadiff used and of anything that looks like a token. The log does name organizations, repositories, branches and paths, so read it before sharing it.


## 0.13.0 (2026-10-19)

### Three-way merge

**File → Resolve conflict…** (Ctrl+Shift+R), or right-clicking a conflicted (U) file, opens the new merge window. It shows ours, the base and theirs side by side, with the editable result below.

- Changes made on only one side, or the same on both sides, are merged automatically.
- Each conflict starts as git-style markers, including the base (diff3 style), on a red background.

To resolve a conflict:

- Take ours (Ctrl+1), the base (Ctrl+2) or theirs (Ctrl+3).
- Take both, ours first (Ctrl+4) or theirs first (Ctrl+5).
- Or edit the result by hand. Ctrl+0 puts the conflict markers back.

Alt+Up and Alt+Down step between conflicts, and taking a side moves on to the next one. The Merge menu can also take ours or theirs for every remaining conflict.

Saving with nothing unresolved stages the file, as `git add` would. The result keeps our side's line endings.

### git mergetool

`yadiff --register-mergetool`, or **Help → Register as git mergetool…**, sets `mergetool.yadiff.cmd` to `yadiff --merge "$BASE" "$LOCAL" "$REMOTE" "$MERGED"` with `trustExitCode`, and makes yadiff the default merge tool.

yadiff exits 0 only when the result was saved with every conflict resolved, so `git mergetool` knows whether the file is done. `--unregister-mergetool` removes the registration.


## 0.12.0 (2026-10-19)

### Structural diff

A new diff algorithm, **Structural (tokens)**. Pick it from the Algorithm drop-down or Options → Diff algorithm, or pass `--algorithm structural`.

It compares the code's tokens instead of whole lines: names, numbers, strings, operators and the words in comments.

- Lines with the same tokens match, whatever their whitespace.
- In a changed region, lines whose tokens all reappear on the other side are greyed as unimportant. That covers calls re-wrapped over several lines, braces moved to their own line, and re-indented code.
- Only the tokens that actually changed are highlighted.

It doesn't use a parser, so it works for any language; the file type only tells it the comment and string syntax. It is fast: a 60,000-line file with 6,000 edits takes about 0.4 s.


## 0.11.0 (2026-10-19)

### Binary files as hex

Changed binary files that aren't images now show as a hex dump: 16 bytes a row, with offsets and ASCII.

- Changed bytes are highlighted.
- When bytes are inserted or removed, the rows after them line up again, and each side shows its own offsets.
- Files over 8 MB still show the "binary file differs" message.
- **View → Show binary files as hex** turns it off.

### CSV and TSV files as tables

`.csv`, `.tsv`, `.tab` and `.psv` files now show as a table:

- Added rows are green, removed rows are red, and changed cells show as "old → new".
- Columns are matched by their header, so reordered columns still line up.
- Rows are matched by order, or by a key column you pick from the drop-down.
- **Only changed rows** hides the rest.
- Change navigation steps through the changed rows.

**Text view**, or **View → Show CSV and TSV files as tables**, switches back to the text diff.

### Fix

The file manager menu now runs yadiff by its full path. Without that, Dolphin couldn't find it, because the desktop session's PATH may not include `~/.local/bin`.


## 0.10.0 (2026-10-19)

### Compare text and the clipboard

- **File → Compare text…** opens two boxes to paste or type into. The file name you give sets the syntax.
- **Edit → Compare clipboard with left (or right) file** compares the clipboard with the file on screen.
- Both sides are saved under `~/.cache/yadiff/scratch`, so they can be edited like files. They are removed after a week.

### Patch files

Open a patch with `yadiff --patch FILE`, **File → Open patch…**, or by dropping a `.patch` or `.diff` file on the window. It reads output from `git diff`, `git format-patch` and `diff -u`.

- If the files the patch was made from are there, each file shows in full with the patch applied, even when the file has changed a little since.
- Otherwise only the patch's hunks show, and the lines in between are marked as not shown.
- **File → Apply patch…** runs `git apply` after checking that the patch applies cleanly. It can also stage the changes, or apply the patch in reverse.
- **File → Save as patch…** writes any comparison as a patch that `git apply` accepts, including binary and untracked files.

### Recent comparisons and sessions

- **File → Recent comparisons** reopens any of the last 20 comparisons.
- **File → Save session…** writes a `.yadiff` file with the comparison, the file shown, the view and the diff options.
- Reopen a session with **File → Open session…**, `yadiff --session FILE`, or by dropping the file on the window.

### Drag and drop

Drop two files or two folders on the window to compare them, or drop one and then a second. In the empty window the comparison opens in place; otherwise it opens in a new window.

### File manager menu

**Help → Add to the file manager's menu**, or `yadiff --add-file-manager-menu`, adds yadiff to Dolphin's right-click menu:

- **Compare with yadiff** when two items are selected.
- **Select as left side for yadiff**, then **Compare with left side (yadiff)**, to compare one item at a time.

On Windows, Explorer gets the left-side pair for files and folders. On Windows 11 they are under "Show more options".

### Also

`--title TEXT` sets the window title.


## 0.9.0 (2026-10-19)

### Stage, unstage or discard single changes and lines

In a working-tree comparison, right-click a change and choose **Stage this change**. To stage only part of a change, select some lines first (side by side, either side works) and choose **Stage selected lines**.

- Staged files offer **Unstage** instead.
- **Discard** reverts the chosen lines in the working tree. It asks first.
- From the keyboard: **Edit → Stage current change or selected lines** (Ctrl+Alt+S) and **Unstage…** (Ctrl+Alt+Shift+S).
- A partly staged edit works the way git's own line staging does: it keeps the old lines and adds the chosen new ones after them.

### Step through commits

A comparison of a range, such as `yadiff main feature` or a pull request, now has a commit bar above the diff.

- Its drop-down lists every commit in the range.
- The arrows, or Ctrl+Alt+PgDown and Ctrl+Alt+PgUp, step from the whole range to each commit in turn.
- A single commit, opened from the commit picker or from blame, steps through its neighbours instead: older commits along the first parent, and newer ones on the way to HEAD.
- Hover the author line to see the full commit message.

### Branches, tags and stashes in Compare commits

The commit picker has a drop-down to switch from the history to:

- **Branches and tags**, where **Changes since HEAD** shows what a branch has that HEAD doesn't.
- **Stashes**, where **Show stash** shows a stash's changes.


## 0.8.0 (2026-10-19)

### Ignore comments

**Options → Ignore comments** takes comments out of both sides before comparing. The comment syntax comes from the file type. It covers C-style (`//`, `/* */`), `#`, `--`, `;`, `%`, `'` and `<!-- -->` languages, and skips string literals, so `"http://…"` is not a comment.

### Unimportant text

**Options → Unimportant text rules…** takes a list of regular expressions for text that shouldn't count, such as timestamps, `$Id$` keywords or GUIDs. **Options → Ignore unimportant text** switches the rules on and off.

### How ignored differences look

- A line that differs only in ignored text, case or whitespace shows as unchanged on a grey background. Each side keeps its own text.
- An added or removed line that is nothing but ignored text, such as a new comment, is greyed with a `·` in the gutter.
- Neither kind counts as a change for navigation or the +/− counts.

### Manual alignment

When the diff lines up the wrong lines:

1. Right-click a line and choose **Align this line with…**.
2. Right-click the line on the other side it belongs with and choose **Align with … line N**.

yadiff compares the stretches above and below the pair separately, and the two lines share a row, marked `↔`. You can add as many alignment points as you need. **Remove manual alignment** clears them for the file.


## 0.7.2 (2026-10-18)

### Fixes

- In the empty window that plain `yadiff` opens outside a repository, **File → New comparison** now shows the comparison in that same window. In 0.7.1 it still opened a separate window. From a window that already shows a comparison, New comparison still opens a new window, so the current one stays as it is.


## 0.7.1 (2026-10-18)

### Fixes

- Running `yadiff` with no arguments outside a git repository no longer fails with "not a git repository". It opens an empty window with a hint to use **File → New comparison** (Ctrl+N), and the comparison you pick there replaces the empty window. Arguments given outside a repository still report the error.


## 0.7.0 (2026-10-17)

### Updates

- **Help → Check for update now** looks up the latest GitHub release. If there's a newer one, it shows the release notes and asks before installing.
- **Help → Automatically install updates** (off by default) checks once a day and installs without asking. A status-bar button restarts into the new version; otherwise it starts the next time you open yadiff.

Releases are read with the `gh` CLI, which handles sign-in for the private repository. Without `gh`, yadiff uses the REST API with `GH_TOKEN` or `GITHUB_TOKEN`.

How the update installs depends on how yadiff was installed:

- **Linux, installed with `install.sh`:** yadiff builds the release's source and runs its tests. It then installs the build as a new `~/.local/opt/yadiff-<tag>` snapshot and moves the `~/.local/bin/yadiff` link to it. The previous snapshot is kept.
- **Windows, an unzipped folder:** yadiff downloads `yadiff-windows-x64.zip` and replaces its files in place. Files in use are renamed to `*.old` and removed on the next start.
- **Anything else** only gets a link to the release.


## 0.6.0 (2026-10-17)

### yadiff's own diff engine

Two new algorithms, available from the toolbar drop-down, Options → Diff algorithm, or `--algorithm`:

- **Myers LCS** (`myers-lcs`) is exact Myers, a longest common subsequence of lines, so it always gives the fewest changes. It is slower on files with very many changes.
- **Heckel isolation** (`heckel`) uses Heckel's 1978 algorithm. Lines that occur once on each side anchor the match, and matches grow from there. It is fast and keeps blocks together.

### New ignore options

- **Options → Ignore leading/trailing whitespace**
- **Options → Ignore case**

With either option on, yadiff compares the lines itself, using whichever algorithm is selected (git's included). Lines that differ only in ignored ways count as unchanged, and the left side still shows its own text. The file title's +/− counts come from that comparison.

These two options don't change which files are listed. A file whose only differences are ignored says so when you open it.


## 0.5.0 (2026-10-17)

### Colour themes

View → Color theme lists System (the desktop's own style), Dark+ and Light+ (VS Code), Monokai, Solarized Dark and Light, One Dark and Light, GitHub Light and Dark, Dracula and Vibrant Ink. A theme sets the window palette, the syntax colours and the default added, removed and moved colours. File → Switch light / dark (Ctrl+Shift+D) swaps a theme for its counterpart.

### Fonts

View → Fonts sets three fonts: the diff font, the file list font, and the font used for rich-text copies.

### Git blame

View → Git blame (Ctrl+Shift+B) adds a column showing each line's commit, date and author, on both sides of the diff. Newer commits get a stronger tint, and lines that haven't been committed yet are labelled that way. Hover a line to see the commit summary. Click it to open that commit against its parent in a new window.

### Fixes

- The toolbar's up and down arrows are now visible in dark themes.


## 0.4.0 (2026-10-17)

The first part of the yadiff improvement list: new menus, marking, file operations and viewer options.

### Changed shortcuts

- **Find:** F3 (and Ctrl+F) opens find, F8 is Find next and F7 is Find previous.
- **Go to line:** Ctrl+G.
- **Changes:** Alt+Down and Alt+Up go to the next and previous change. Alt+PgDown and Alt+PgUp move between files.

### Marking

Marking replaces the review checkboxes. Ctrl+M, or right-click → Mark file, highlights the file name, and you can change the colour under Marking → Mark highlight color. A mark still clears itself when either side of the file changes. The Marking menu adds Mark by pattern, Mark identical files, Hide marked files, Select marked files and Unmark all.

### Menus

- **File:**
  - New comparison, which opens in a new window: a repository, two revisions, two folders, two files, or a PR.
  - Change window title.
  - Globbing ignore patterns (`.gitignore` syntax), with a switch to turn them off.
  - Stage, unstage or revert the selected files. Revert asks first and moves deleted files to the trash.
  - Rescan, Refresh list, and Download files again, which re-fetches a PR.
  - Swap right and left, and Exit.
- **Edit:**
  - Copy (with colours), Copy without markup, Copy as Markdown, and Copy for code review.
  - Select all, plus copying the left or right file's path.
  - Find, Find next and Find previous. Highlight clicked word, and Go to line.
  - Set editor, and Edit left or right file in that editor at the current line.
- **View:**
  - Show both panes, left only or right only.
  - A Language menu, syntax highlighting on or off, and show whitespace or line numbers.
  - Tab width, and zoom (Ctrl++, Ctrl+-, Ctrl+0 for 100%).
  - Trim the common prefix from folder names. They are now fully qualified otherwise.
- **Options:**
  - Show left-only, right-only, different, identical or blanks-only files, plus Show all and Show no files.
  - Ignore all whitespace, ignore extra whitespace, ignore blank lines, and ignore CR.
  - Diff algorithm and move detection.
  - Remember scroll positions for files.
- **Help:** Register and Unregister as git difftool, and Usage.

### File list

Several files can be selected. Right-click for:
- Copy, Select all and Copy commit hash.
- View differences (Enter) and View filelog in gitk.
- Mark file, Stage file and Revert file.
- Download file again and Refresh list (F5).

### Still to come

- **0.5.0:** colour themes, including light/dark, plus fonts and Git blame.
- **0.6.0:** Ignore case and Ignore leading/trailing whitespace, plus the Myers LCS and Heckel algorithms. These need yadiff's own diff engine.
- **0.7.0:** updates.


## 0.3.0 (2026-10-16)

### Review comments

- **Collapsed by default.** Each review thread, on GitHub or Azure DevOps, starts as one line under the code line it belongs to. That line shows the author, the local date and time, the reply count, and the first substantial sentence. Click it, or the ▸ in the gutter, to read the thread, and click ▾ to fold it again. View → Expand all review comments (Ctrl+Alt+O) and Collapse all (Ctrl+Alt+Shift+O) work on every thread at once.
- **Rendered properly.** Expanded threads show Markdown and the HTML that review bots use as rich text. Links work, and code is shown in monospace on a shaded band. Hidden HTML comments are dropped, `<details>` sections fold to their summary as on Azure DevOps, and badge-style tags stay separate words.
- **Author links.** The author's name links to the comment on GitHub or Azure DevOps.
- **Date and time.** Comments show local date and time, not just the date.

### Fixes

- **Syntax highlighting.** Review comments no longer break highlighting for the rest of the file. Comment text such as `**/*.{cpp,h}` used to open a C++ block comment that ran to the end of the file. Comment rows, collapsed regions and alignment rows now count as blank lines to the highlighter.
- **Editing.** Text typed into a former alignment row while editing is now highlighted.


## 0.2.0 (2026-10-16)

### Azure DevOps pull requests

- **`yadiff -ado URL`** opens an Azure DevOps pull request. Links can be `dev.azure.com`, `ORG.visualstudio.com`, or Azure DevOps Server. yadiff shows what the PR's Files tab shows: the source branch against its merge base with the target. If the current repository has a remote for that Azure Repo, the PR is fetched into it. Otherwise yadiff keeps a lightweight cached clone.
- **Review threads** show under the lines they belong to. You can add comments, including on several lines at once, and reply. The Review menu approves (vote 10), requests changes (vote −5, "waiting for author"), or comments on the PR.
- **Sign-in.** yadiff uses the first of these that works: environment variables (`YADIFF_ADO_PAT`, `AZURE_DEVOPS_EXT_PAT`, `SYSTEM_ACCESSTOKEN`), a token it saved in the system keyring, the Azure CLI, or Git Credential Manager. If none works, or the token is rejected, a sign-in dialog opens. It checks a personal access token with Azure DevOps before saving it. On Linux the token goes in the Secret Service (KDE Wallet or GNOME Keyring), and on Windows in the Credential Manager. Use `yadiff --ado-login ORG` to sign in ahead of time, and `yadiff --ado-logout ORG` to forget the token.
- **Credential handling.** Tokens are never put on a command line, and yadiff refuses to send them over plain HTTP.

### Other changes

- **Pinned installs.** `install.sh` installs a versioned snapshot in `~/.local/opt/yadiff-<version>` instead of linking to the build folder. Rebuilding while you develop no longer changes the installed yadiff or your `git difftool`.
- **Tests.** A new `test_ado` suite runs the whole Azure DevOps flow against a fake Azure DevOps server, including the real `yadiff -ado` command line. It runs on Linux and Windows in CI.

### Known limitations

- Azure DevOps support is tested against a simulated server only. It hasn't been tried against a real organization yet.
- Browser sign-in needs the Azure CLI. Without it, sign in with a personal access token that has the **Code: Read & write** scope.


## 0.1.0 (2026-10-16)

First release of yadiff, a graphical diff viewer for Git built with C++20 and Qt 6, for Linux and Windows.

### Install

- **Windows:** download `yadiff-windows-x64.zip`, unzip it anywhere, and put the folder on `PATH`. Git for Windows must be installed. The Visual C++ runtime is bundled.
- **Linux:** clone the repository and run `./install.sh`. You need Qt 6.5 or newer, CMake and Ninja. KSyntaxHighlighting is optional and adds syntax colours.

### What it does

- **Compare** staged and unstaged changes (`yadiff`), all changes against HEAD (`yadiff -a`), single files or directories (`yadiff PATH`), revisions (`yadiff A B`, `A..B`, `A...B`), any two directories or files (`yadiff --no-index A B`), and GitHub pull requests (`yadiff -ghpr URL`).
- **View** inline or side by side, with line numbers and an overview strip. It has word-level highlights, detection of moved blocks, syntax highlighting, collapsible unchanged regions, word wrap, whitespace display, and an image diff with swipe, onion-skin and difference modes.
- **Diff options** include the Patience (default), Histogram, Myers or Minimal algorithms, plus options to ignore whitespace and blank lines.
- **Navigate** with F3 and Shift+F3 across changes and files. You can search a selection, a file or all changes, by substring or regex. A file filter takes globs and exclusions, and a commit history picker chooses what to compare.
- **Review** with checkboxes that clear when a file changes again. For PRs, review threads show inline, and you can comment, reply, approve or request changes through the `gh` CLI.
- **Change files** by editing the working-tree side, or applying a change to either side on disk. You can copy a change as a colour-coded `git diff` hunk.
- **Share** a self-contained HTML report from the File menu or with `--export FILE`.
- **Integrate** with Git via `yadiff --register-difftool`, which makes `git difftool` and `git difftool -d` open yadiff.

### Known limitations

- The Windows build is tested in CI, with a build, the full test suite, and a smoke test of the zip. It hasn't yet been used on a Windows desktop.
- Posting PR comments and reviews is untested against GitHub. Reading comments has been checked against a real PR.
- Editing works on the working-tree side only, in side-by-side view.

