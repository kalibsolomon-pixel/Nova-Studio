# Owner/legal release checklist

Engineering evidence for review; no legal conclusion or approval is recorded.

- GPL: confirm contributor/copyright notices and scope, complete corresponding source, license
  texts, source archive/revision correspondence and the chosen distribution arrangement.
- Modified Qt/LGPL: review dynamic replaceability, the Qt 6.12.0 private-API version requirement,
  exact upstream archive, included build options and source patch. The notice dates modification
  to 2026-11-05 UTC, based on the shipped build record (`built_at` 05:57:59Z), not the later
  consolidation commit. Confirm the date basis and adequacy of relinking/build information.
- GCC/MinGW: review GCC Runtime Library Exception materials, bundled DLLs, MinGW CRT/winpthreads
  notices and exact GCC source sidecar plus `gcc-runtime-source.json`. Decide where all artifacts
  and source sidecars will be hosted together and how continued availability will be ensured.
- MSVC: the candidate ships GCC runtimes; MSVC only builds local regression tests. Confirm the
  relevance of the previously recorded Microsoft Build Tools licensing gate to that use.
- Source hosting: the owner authorized the existing GitHub source repository and home consolidation.
  Verify visibility and exact source correspondence as documented in SOURCE_CORRESPONDENCE.md.
  No binary/sidecar hosting is implied. Set `NOVA_SOURCE_URL` for a future build only after access
  and correspondence are verified, then rebuild/requalify. Existing Help names installed files.
- Minecraft: confirm the trademark disclaimer and a suitable non-chat contact method. Only
  original synthetic fixtures and Nova artwork belong in the source/package; no game assets.
- Signing: choose a policy, certificate and file scope (installer, application, bridge,
  uninstaller). Signing changes hashes and requires a newly identified candidate. Nothing is signed.
  See [current options and process](SIGNING_OPTIONS.md).
- Owner review: Phase 1 visuals, physical presentation, Ctrl+S behavior, compatibility limitations,
  recorded intermittent incidents and the temporary 50k performance exception remain explicit gates.

## Decisions recorded 2026-11-19

The owner reviewed this checklist and decided:

- The licensing/source verification items above (GPL, Qt/LGPL, GCC/MinGW, MSVC-relevance,
  Minecraft disclaimer and contact) are confirmed; work proceeded on that basis.
- Hosting: source, binaries and source sidecars all live on GitHub
  (`https://github.com/kalibsolomon-pixel/Nova-Studio`) for now; a separate project website may
  come later. Continued availability means keeping the release's assets, `SHA256SUMS.txt` and
  sidecars attached to that repository.
- Signing: no signing policy is wanted. The release ships unsigned; free/open-source Authenticode
  signing is not available, so nothing was purchased or ruled out for the future.
- Owner testing and acceptance are granted, including the Phase 1 visuals, physical presentation,
  Ctrl+S behavior, compatibility limitations, the disclosed intermittent incidents and the
  temporary 50k performance exception.
- All experimental / not-for-publication labels are removed for release: the packaging README,
  `CPACK_PACKAGE_FILE_NAME`, the packager's default output label and manifest fields were already
  final; the experimental wording in sources, scripts, docs and the ACP UI badge was removed
  (`scripts/package-mingw-experimental.py` is now `scripts/package-mingw.py`). The ACP session
  stays user-initiated and its real-provider-testing disclosure stays in the limitations doc.
  Changing any of this again requires a new package and qualification.
