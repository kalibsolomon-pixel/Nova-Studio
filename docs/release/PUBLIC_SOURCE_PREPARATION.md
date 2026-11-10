# Public-source preparation record

> Historical source-publication/review record. On 2026-11-10 the owner explicitly authorized
> the first public preview, normal source/tag pushes and artifact publication. Owner visual and
> physical-presentation review remain deferred, not completed. The composite Windows criterion,
> 50k timing exception, intermittent-test history and ACP limitations remain accepted/disclosed.
> Current release policy and limitations: [RELEASE_NOTES.md](RELEASE_NOTES.md).


Prepared 2026-11-10 UTC for the existing `kalibsolomon-pixel/Nova-Studio` repository
(GitHub repository ID `1446125539`). The owner authorized source integration, the exact 26
deletions, a normal branch push and public source visibility. Binary release approval remains pending.

## Provenance and inventory

The reviewed source is private review commit `8b0d6c5c4eb80af44a54e5dd253578587b263d11`, tree
`de1095db4cfd318fecf729de1965f71b7bd7562b`, selected from candidate manifests and complete release
lineage. The separate audit-only documentation commit `e1a2670ce314f6e2c7398b336c8ab57cd7a3553e`
was consulted; private evidence locations in that report are not copied into release documents.
Experimental worktrees were read without integrating their unreviewed changes.

Public-history source commit `9db2242c79be508eb870d69a3157055d0205097d` reproduces all 595 reviewed blobs. The subsequent
documentation update records source correspondence, independently verified runtime provenance,
signing research, this scope and remaining review gates. It preserves the Qt patch bytes.
No application behavior or candidate binary was changed during consolidation.

The file-level inventory classifies 208 product-source paths, 191 tests/fixture paths,
22 documentation paths, 166 build/package paths, 8 license/notice paths, 26 superseded prototype
paths, and 12,320 private/generated evidence paths. Compared with the original home source,
354 release files already matched and 241 required integration. All private/generated files remain
local, unchanged and outside the public branch's tracked tree. New release documents are additional
to these baseline counts. Detailed per-file hashes and preservation evidence are retained privately.

The only physical deletions were the following approved paths. Their original Git blobs matched
the previously reviewed sanitized deletion set and were absent from current release build inputs.
They are unused Phase 1 Rust/Qt prototypes; current source, tests, notices and user work remain.

```text
Cargo.lock
Cargo.toml
prototypes/nova-egui/Cargo.toml
prototypes/nova-egui/src/assets.rs
prototypes/nova-egui/src/document.rs
prototypes/nova-egui/src/io.rs
prototypes/nova-egui/src/main.rs
prototypes/nova-egui/src/metrics.rs
prototypes/nova-egui/src/pixels.rs
prototypes/nova-egui/src/render.rs
prototypes/nova-egui/src/ui.rs
prototypes/nova-qt/CMakeLists.txt
prototypes/nova-qt/src/assets.cpp
prototypes/nova-qt/src/assets.h
prototypes/nova-qt/src/canvas.cpp
prototypes/nova-qt/src/canvas.h
prototypes/nova-qt/src/core.cpp
prototypes/nova-qt/src/core.h
prototypes/nova-qt/src/cube.cpp
prototypes/nova-qt/src/cube.h
prototypes/nova-qt/src/main.cpp
prototypes/nova-qt/src/metrics.cpp
prototypes/nova-qt/src/metrics.h
prototypes/nova-qt/src/window.cpp
prototypes/nova-qt/src/window.h
prototypes/nova-qt/tests/core_test.cpp
```

## Privacy and exposure scope

The audit covers every existing remote branch/tag and reachable object, plus both new integration
commits and the final tree. At baseline GitHub had one branch (`main` at
`1ed2fd6299e564932bf5954145e5407323597f77`), three commits and no tags, releases/assets,
pull requests, issues, Actions workflows/runs/artifacts, Pages, wiki or discussions. LFS pointers,
submodule entries, tracked links and workflow files are checked in reachable trees. External URLs
are inventoried and inspected for private endpoints and obsolete source instructions.

Gitleaks 8.30.1 was obtained from its official release with the published archive checksum.
The full original remote history produces one JWT finding: an explicitly synthetic token in
`tests/minecraft_testing_test.cpp::tokensAreRedacted`, with dummy subject and literal
`signature-value`; it exercises log redaction and is not a credential. The candidate-tree scan
reports the same fixture. The final documentation scan also flags GNU's public release-signing
key fingerprint as a generic API key; it is public verification metadata, not a credential.
Custom object/privacy scanning also checks profile/email/workspace paths,
credentials, key markers, cookie/token assignments, local/private URLs, archives and file layout.
Synthetic Unicode profile paths and loopback test servers are deliberate test inputs. Generic,
configurable toolchain defaults are documented examples. An old branding README's generic local
Aurora asset path is stale provenance, not a credential or private log; the current version uses
the reviewed public attribution. Non-sensitive prototypes and stale instructions persist in
historical commits. Raw findings/logs remain private; every finding is adjudicated before publication.

Private development commits and their evidence are not ancestors of the integration branch.
Only an explicit fast-forward ref is eligible for push; local branches/tags are preserved.
No archive, evidence log, build output, credential or Minecraft screenshot is added to the public tree.
CC0 synthetic fixtures and original Nova branding are retained. A clean scan is bounded evidence,
not proof that all possible secrets or license problems have been excluded.

## Verification and remaining gates

Independent checks match all 595 source ZIP blobs plus its revision marker, all 214 payload files
against the portable ZIP, recorded artifact hashes, the exact GCC sidecar, runtime package contents
and signatures. Both toolchain source-sync layouts pass disposable dry runs; self-tests pass 34/34.
See [runtime verification](RUNTIME_SOURCE_VERIFICATION.json) and [source correspondence](SOURCE_CORRESPONDENCE.md).
The original patch contains required blank unified-diff context lines; these bytes are preserved
and marked with a patch-specific whitespace attribute. A template's extra terminal blank line is
trimmed in the documentation follow-up. No candidate is rebuilt or relabelled as a result.

Historical qualification of the exact candidate remains GCC/MSVC 67/67, installer acceptance pass,
dependency/policy 14/14 and bridge 39-tool self-test. Those tests were not rerun here and do not
qualify a newly rebuilt artifact. No pristine Windows guest was tested; the owner-approved
composite dependency-isolation test on the existing host is the substitute. The owner-approved
approximately 150-second 50k validation exception remains. Known intermittent editor-gui,
tool-placement, texture-pixels, save-drafts, MCP, ACP and Fabric incidents remain disclosed.

Visual and physical-presentation review, changed Ctrl+S behavior, binary release/experimental-label
decisions, signing and compatibility acceptance remain pending. ACP remains experimental and off
by default; real provider conversations and Claude Code/Codex ACP adapters remain untested.
GPL source arrangement, Qt/LGPL notices and replacement/build information, GCC/MinGW source hosting,
NSIS notice placement, Microsoft Build Tools terms, trademark/contact wording and other legal
questions remain for the owner/qualified counsel. No legal conclusion is made.
