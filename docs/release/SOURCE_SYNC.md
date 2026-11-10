# Canonical source and synchronization

The home repository holds release source and release-facing documentation on its public-source
integration branch. GitHub's default branch is `main`. See [source correspondence](SOURCE_CORRESPONDENCE.md)
for the exact candidate commit, tree, archive hashes and upstream source materials.

The consolidation starts from the existing sanitized GitHub ancestry, not the private development
ancestry. Its exact candidate-source commit is `9db2242c79be508eb870d69a3157055d0205097d`. Historical development branches, tags
and private evidence stay local; they must not be included in a push of all refs or a mirror push.
Private evidence retained in the home folder is untracked on this branch and excluded through
local Git metadata. It is not part of a fresh clone or a source archive. Existing files were
preserved; exactly 26 approved obsolete Phase 1 prototype paths were removed from the home tree.
Those prototypes remain in older public commits; current-tree removal does not erase history.

`scripts/sync-public-source.py` remains a legacy isolation helper for copying a committed snapshot
between disposable repositories. It expects a remote-free development copy and a clean destination
on `main`, the exact configured destination URL and separate object stores. Those checks deliberately
refuse direct use against the canonical home repository, which now has a remote and both preserved
private and public history locally. A separate maintained public-source checkout is not required.

Run without `--apply` to inspect a plan; deletions require reviewed `--allow-deletions`. The script
never commits or pushes. Required-file checks cover both MSVC and GCC/MinGW layouts. Privacy
checks refuse credentials, profile/workspace paths, unexpected images, private evidence copies,
build outputs, unexpected archives, large files, links, submodules and junction traversal.
Documented toolchain defaults remain overridable, generic paths.

`python scripts/test-sync-public-source.py` passed 34/34 on throwaway repositories during this
consolidation. Additional disposable dry runs of the exact 595-file candidate-source tree checked
both toolchain layouts, refused the 26 deletions without the flag, and passed with it. No dry run
modified its destination. See [public-source preparation](PUBLIC_SOURCE_PREPARATION.md) for scope.

Publishing uses an explicit reviewed branch/refspec with a normal fast-forward push to GitHub
`main`; no private branch or local RC1 tag is published. Full reachable history, commit metadata
and enabled GitHub surfaces must be reviewed before changing visibility. A clean scanner result
alone does not establish privacy, legal sufficiency or release acceptance.
