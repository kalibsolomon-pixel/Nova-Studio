# Sanitized source synchronization

`scripts/sync-public-source.py` prepares one committed revision for the separately configured
destination checkout. Its historical name does not make that repository public. No sync, remote
contact or publication is part of local release qualification.

Run without `--apply` to inspect a plan. Review the expected destination URL, branch, cleanliness,
object-store separation and every proposed file before any separately authorized write. The script
never commits or pushes. Deletions require explicit `--allow-deletions` after review.

The required-file lists cover common source/license inputs, MSVC and GCC/MinGW independently.
Toolchain locations are configured through the documented environment/command options rather than
a developer workspace path. Workspace privacy patterns come from Git's current worktree list;
profile/login patterns are derived from the local environment.

The copy excludes private phase/benchmark/review/session evidence and developer-only generators.
`docs/phase-4i/MINGW_BUILD_GUIDE.md` is the explicit exception because it is a corresponding-source
build guide. Historical private material and unused prototypes have also been archived outside
this release tree, with per-file hash verification before duplicate removal.

Secret/profile patterns, unexpected images, known private-evidence images, executable/build outputs,
archives (except the pinned Gradle wrapper), links/junctions, submodules and unexpected large files
are refused. The pinned font exception requires its exact blob identity. The destination's `.tools`
and virtual-environment junctions must never be written through.

Run `python scripts/test-sync-public-source.py` for the self-tests. Qualification additionally scans
the exact staged tree and dry-runs sanitized throwaway source copies. A passing scan cannot prove
legal sufficiency or public visibility; owner review of the actual destination remains necessary.
