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

The owner-selected labels are unchanged. An actual public preview needs an explicit decision on:
`packaging/mingw/README.txt.in`'s NOT FOR PUBLICATION line; `CPACK_PACKAGE_FILE_NAME` in
`cmake/NovaPackagingMinGW.cmake`; the packager's default output label; experimental manifest fields;
the notice's experimental comment; experimental source comments; and the experimental packaging
script name referenced by the build guide. Changing them requires a new package and qualification.
ACP's separate experimental/off-by-default status is not implied to change with those labels.
