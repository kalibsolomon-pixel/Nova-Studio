# Local release review: 0.1.0-preview.1

Source access is authorized separately from binary release acceptance. The existing candidate is
unsigned and unpublished and retains the owner's experimental / not-for-publication labels.
[Source correspondence](SOURCE_CORRESPONDENCE.md) identifies its exact tree and public-history
commit. Source publication is not binary release or legal approval.

The starting release commit is `046d6d0e26b1579dc962386e4cd678ac66a71341` (RC1). The later
review snapshot was `5fb6fa1b9d9269e50347bf84dd3dde6be1e854b2`: GCC 66/67 (ACP fixture failure),
MSVC 67/67. This revision repairs demonstrated ACP scope handling and Fabric exit supervision,
retains strict error assertions, and separates historical private evidence/prototypes from release
source. Their original repositories and independently verified archive remain preserved locally.

Use the candidate's `release-manifest.json`, embedded build revision and source archive's
`.nova-source-revision` together to identify exact bytes. The accompanying local qualification
report records the branch/commit, every run and retry, installer effects and artifact hashes.
It is the result record for this candidate; results from earlier snapshots do not qualify it.

## Build and qualification

- [Build instructions](BUILDING.md): GCC/MinGW produces the package; MSVC is regression-test-only.
- [GCC toolchain guide](../phase-4i/MINGW_BUILD_GUIDE.md): pinned toolchain, Qt patch and overrides.
- Both complete CTest suites must finish **67/67**, serially with short TEMP/TMP and display guards.
  Seven optional QtTest slots require external fixtures and skip by design.
- ACP scope replay: run `python tests/acp_scope_replay.py` with `--agent` pointing to the build's
  `tests/nova-fake-acp-agent.exe` and `--output` pointing to a new evidence folder (Qt/compiler
  runtime folders on PATH). Seven scenarios passed
  on both diagnostic toolchains, including bounded retry exhaustion and permanent-error refusal.
- Fabric regression checks an already exited process at the actual supervision boundary, plus
  immediate detached exits. Historical lingering-process failures and other intermittent incidents
  remain in the separate incident record; a clean final suite alone does not prove them cured.

## Scope and remaining decisions

The ~150-second 50k workload is an owner-approved temporary exception to the former 90-second
threshold, not a new candidate measurement. Visual/physical screening is deferred to the owner.
The pristine Windows guest test was not performed; the accepted composite dependency-isolation
substitute has only been exercised on the development host.

ACP remains experimental and off by default. OpenCode startup/bridge attachment was exercised,
but no real provider conversation was performed. Claude Code and Codex ACP adapters are untested.
The owner must still review Phase 1 visual evidence and accept changed Ctrl+S behavior unless a
newer explicit acceptance is recorded. Known editor-gui, tool-placement, texture-pixels,
save-drafts, ACP and Fabric incidents must remain disclosed.

Installer acceptance is owner-authorized, subject to preserving existing installations, settings,
unknown/user files and unrelated registrations. Its actual result belongs in the candidate's
qualification report; a portable launch is not installer acceptance.

Before public preview: owner approval of the [licensing/source/signing checklist](RELEASE_CHECKLIST.md),
the [source-sync review](SOURCE_SYNC.md), visual/compatibility review and an explicit decision on
experimental labels/filenames. The existing GitHub repository is the verified source destination;
final public visibility is checked separately from this preparation record. No binary download URL
is claimed.
