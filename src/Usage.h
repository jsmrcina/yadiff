#pragma once

#include <QString>

// The command-line summary shown by --help and Help > Usage.
inline QString yadiffUsageText()
{
    return QStringLiteral(
        "Graphical diff viewer for Git.\n\n"
        "  yadiff                      staged and unstaged changes, compared against HEAD\n"
        "                             (outside a repository: an empty window; use File > New comparison)\n"
        "  yadiff PATH...              just these files or directories, compared against HEAD\n"
        "  yadiff REV                  REV compared against the working tree\n"
        "  yadiff REV1 REV2            REV1 compared against REV2\n"
        "  yadiff REV1..REV2           same as REV1 REV2\n"
        "  yadiff REV1...REV2          merge-base of REV1 and REV2 compared against REV2\n"
        "  yadiff [REV...] -- PATH...  limit any of the above to PATHs\n"
        "  yadiff -ghpr URL            a GitHub pull request (https://github.com/OWNER/REPO/pull/N)\n"
        "  yadiff -ado URL             an Azure DevOps pull request\n"
        "                             (https://dev.azure.com/ORG/PROJECT/_git/REPO/pullrequest/N)\n"
        "  yadiff --ado-login [ORG]    sign in to Azure DevOps (token saved in the system keyring)\n"
        "  yadiff --no-index A B       two directories or two files, in or outside a repository\n"
        "                             (automatic when A and B are outside the current repository)\n"
        "  yadiff --patch FILE         a patch file, on top of the files it applies to when they're there\n"
        "  yadiff --session FILE       a session saved with File > Save session\n"
        "  yadiff --register-difftool  make yadiff a git difftool (git difftool -t yadiff, git difftool -d)\n"
        "  yadiff --merge BASE LOCAL REMOTE MERGED   three-way merge (git mergetool; --register-mergetool)\n"
        "  yadiff --add-file-manager-menu   \"Compare with yadiff\" in Dolphin / Explorer\n"
        "  yadiff ... --export FILE    write an HTML report instead of opening a window\n\n"
        "Options: -C DIR (run in DIR), -a/--all (all changes vs HEAD), --wait (stay attached),\n"
        "--algorithm NAME, --title TEXT, --inline, --side-by-side, --unregister-difftool, --ado-logout ORG,\n"
        "--select-left PATH and --compare-to-left PATH (used by the file manager menu),\n"
        "--remove-file-manager-menu, --unregister-mergetool.");
}
