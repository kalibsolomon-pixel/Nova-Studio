"""Write the closeout only after accepted tests and final measurements exist."""
import json
import pathlib
import statistics

root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-3c'
def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
s = read(out / 'performance-summary.json')
tests = {c: read(out / ('tests-' + c + '-accepted/summary.json')) for c in ('release', 'debug')}
assert all(t['exit_code'] == 0 and t['suite_count'] == 33 for t in tests.values())
preservation = read(out / 'preservation.json')
assert not preservation['protected_changed'] and not preservation['deleted_diff']
assert not any(preservation[k]['missing'] for k in ('tracked', 'untracked', 'ignored'))
native = read(out / 'native-acceptance.json')
assert not native['source_fixture_files_changed']
native_paths = read(out / 'initial-native-paths.json')
shutdown = read(out / 'final-shutdown/rows.json')
assert len(shutdown) == 42 and all(row['exit_code'] == 0 for row in shutdown)
bootstrap_runs = read(out / 'memory-bootstrap/runs.json')
assert len(bootstrap_runs) == 15 and all(row['exit_code'] == 0 for row in bootstrap_runs)
extra = read(out / 'extra-probes.json')
assert extra['animation_followup_runs'] == 12 and extra['cadence_runs'] == 6
def ms(v): return f'{v:.3f}'
def median(v): return statistics.median(v)
before, after = s['baseline'], s['final']
startup_rows = '\n'.join(f"| {key} | {before['process']['small']['startup_stages_ms'][key]['p50']:.3f} | "
    f"{after['process']['small']['startup_stages_ms'][key]['p50']:.3f} |" for key in
    ('application_init', 'settings_load', 'theme_apply', 'editor_menus_icons', 'editor_docks',
     'ui_construction', 'workspace_shell', 'thumbnail_service_init', 'index', 'opengl_initialize'))

text = f'''# Nova Studio — Phase 3-C

Phase 3-C is complete within the measurement limits stated below. Windows native
Qt Widgets/OpenGL remain the implementation. No new feature phase, remote push,
publication, renderer replacement, document-format migration or UI redesign was performed.

## Repository and protection

The actual starting branch was `codex/phase-3-stabilization`, HEAD
`fce50e42d0fed6811f14627cdca322767f482d2e`, matching the accepted 3-B reference.
Initial status was only `?? fixtures/phase-2c5-models/.nova/`. The inventory captured
10,987 tracked, two untracked and 131,233 ignored files. The initial path lists also
identify {len(native_paths['tracked']['native_documents'])} tracked,
{len(native_paths['untracked']['native_documents'])} untracked and
{len(native_paths['ignored']['native_documents'])} ignored native `.nvdoc` documents.
See [initial inventory](baseline-inventory.json), [native paths](initial-native-paths.json)
and [final preservation check](preservation.json). Protected fixture/recovery content
hashes are unchanged, every initially inventoried path remains present, and there
are no deleted tracked files. No `git clean` or broad cleanup was used.

The final measured production source is
`966520a26d3a1b991a767fd39f2164874c33d332`. Release application SHA-256 is
`{tests['release']['executable_sha256']}`. Debug SHA-256 is
`{tests['debug']['executable_sha256']}`. Documentation/tooling commits after that
revision do not change production source. Baseline executables/archives were built
before production edits, retained separately, and never rebuilt from optimized source.
Later common probe recompilation used `--probe-only`: Ninja's existing compile/link
commands against frozen baseline archives, with before/after archive hash assertions.
Each final measurement folder records its executable hashes and build identity.

## What changed

* Asset subtree refresh removes matching file entries in one stable pass instead
  of repeatedly searching/removing from the complete list. Subtree identities reuse
  filesystem information already collected by the scan. Rebasing preserves namespace,
  category, paths, timestamps and sidecar references.
* Content-only asset updates preserve the model's node structure, persistent indexes
  and category selection. They avoid full catalog sorting/reconciliation. Thumbnail
  invalidation includes cached entries outside the currently displayed category;
  structural changes still follow the existing reconciliation/reset paths.
* Layer compositing resolves visible source buffers once, reuses scanline pointers,
  and applies exact integer identities for opaque/first-visible sources. The original
  source-over rounding and transparent-pixel rules are unchanged. The normal
  64-layer admission limit fits the temporary source descriptors on the stack.
* First palette save creates a missing settings directory before the existing
  guarded atomic write. The accepted Release baseline reproduced a first-run modal
  error/timeout; Debug could pass because background recovery created the directory first.
* Window destruction disconnects timeline callbacks before the preview is deleted.
  Direct teardown with an animation reproduced an access violation in the frozen
  baseline (exit `0xC0000005`, [retained reproduction](teardown-baseline-crash.json));
  the new regression passes with the fix. This addresses callback ownership,
  without changing save/discard behavior.

The existing benchmark/probe family was extended, rather than replaced. It now
records raw editor samples/variance, 50k refresh/view operations, workspace software
paint timing, large-batch cancellation, unique/repeated native lifecycle cycles and
optional benchmark-only GPU timer queries. The idle probe's own 250 ms watchdog is
stopped during its observation window. An early uncreated timer-query teardown crash
in the new standalone benchmark instrumentation was corrected by allocating/destroying
the query only with its GL context current; failed characterization runs were retained
and excluded from the final comparison set.

## Measurement method

Machine: Windows 11 Home 10.0.26300; Intel i7-14700F, 20 cores/28 logical processors;
about 31.8 GiB visible RAM; AMD Radeon RX 9070 XT, driver 32.0.32015.7029;
2560×1440 at 240 Hz, 125% display scaling. Hardware snapshots are in
[cpu.json](cpu.json), [os.json](os.json), [gpu.json](gpu.json) and [power.txt](power.txt).
The active Ultimate Performance power plan was recorded and left unchanged.
Builds use the same installed MSVC C++20/CMake/Ninja/Qt 6.12.0 toolchain and Release
configuration. Baseline and final builds run sequentially on the same machine.
No native builds, acceptance suites, native input or other benchmark processes overlap
the controlled comparison runs. Ordinary desktop applications remain open; this
is a workstation study, not an isolated laboratory.

Fresh settings and task-owned fixture copies exclude the protected original `.nova`.
Production startup uses a fresh 1280×820 logical window, Fusion/default dark-violet
theme, the same small-model/50,000-texture fixtures and no OS cache flushing.
The first observed launch of each process scenario is retained separately; ten later
launches per build/scenario form the warm comparison. Internal `first_ui_ms` measures
the first event-loop callback after show. `ready_capture_ms` includes opening/indexing
and first-preview readback: a conservative software capture-ready proxy, not proof
of physical input-to-photon readiness. External marker observation includes process
loading and up to roughly 10 ms polling delay. Startup stages already instrumented
include Qt initialization, settings, editor construction, menus/icons, stylesheet,
workspace shell/restore, thumbnails, index installation and GL initialization.
These are scoped timings; uninstrumented substeps are not given invented numbers.

Three independent invocation pairs cover index/view/editor/animation/model/rendering,
workspace, memory-attribution and window lifecycle workloads; storage lifecycle has
two pairs of 32 cycles per unique/repeated case. Within-operation sample counts,
P50/P95/P99, mean and population variance are in
[performance-summary.json](performance-summary.json). Quantiles use nearest rank.
Small-sample tail estimates are maxima and should not be interpreted as stable
population P99s. Original probes which report invocation summaries retain that scope;
their three medians are not relabeled as hundreds of raw operation samples.

Working set and private committed bytes are recorded separately. CPU percentages
use user+kernel process CPU time over actual elapsed time: 100% means one logical
processor, not the entire 28-processor machine. A reported zero CPU-time delta does
not establish zero activity. Windows CPU accounting quantization limits small deltas.
Qt dispatcher awakenings and per-thread context switches are separate observations;
context switches are not asserted to equal literal hardware/thread wakeups.
Whole-process peak working-set counters in retained probes can include earlier
workloads; they are not unique allocation peaks for the adjacent operation.

See [measured comparison table](PERFORMANCE.md) for the main numerical results.

## Workstream results

### A — Startup

Warm small-pack capture-ready median is {ms(before['process']['small']['ready_capture_ms']['p50'])}
ms before and {ms(after['process']['small']['ready_capture_ms']['p50'])} ms after.
The stylesheet/design system was preserved. No startup deferral was introduced to
move essential work beyond a ready marker. First UI, capture-ready, indexing, stage
timings, external observations and process lifetimes remain separate in the raw data.
True OS-cache-cold startup and physical ready-for-input latency were not measured.

| Small-pack scoped startup stage | Baseline P50 ms | Final P50 ms |
|---|---:|---:|
{startup_rows}

These scopes nest and must not be summed. In particular, `theme_apply` measures
the application call; subsequent widget construction/show/polishing also contributes
to the full first-UI interval. There is no demonstrated startup benefit sufficient
to justify replacing the accepted stylesheet or adding deferred essential widgets.

### B — Settled idle CPU and dispatch

The final widget probe uses 12 s settling, then a 35 s Qt counting window; its external
CPU/thread sample is about 32 s, ending before shutdown. Each of the eight states
has two final runs and one retained baseline run. Welcome, pack with no document,
static texture, 16-layer texture, paused animation, static model, paused animated
materials and settled 50k pack are all represented. The original production process
probe also supplies separate idle observations for empty/small/50k startup scenarios.
No recovery, revision checks, filesystem monitoring or legitimate playback scheduling
was disabled. No idle improvement is claimed from a rounded percentage or a single
baseline observation.
The raw event/module attribution also distinguishes native UI Automation dispatch
from Nova timers and paints; desktop activity is not attributed to a rendering loop
merely because it wakes the Qt dispatcher.

| State | Final CPU % of one processor, median [min–max] | Dispatcher awake/s, median | Thread context switches/s, median |
|---|---:|---:|---:|
'''
for name, row in after['idle']['summary'].items():
    cpu=row['cpu_percent_one_core']; awake=row['dispatcher_awake_per_second']; switches=row['thread_switches_per_second']
    text += f"| {name} | {cpu['median']:.4f} [{cpu['min']:.4f}–{cpu['max']:.4f}] | {awake['median']:.2f} | {switches['median']:.2f} |\n"
text += f'''
### C — Workspace switching

Final median main-thread command time is {ms(after['workspace_ms']['blocking_ms']['p50'])} ms;
last observed Qt paint is {ms(after['workspace_ms']['last_paint_ms']['p50'])} ms, versus
{ms(before['workspace_ms']['last_paint_ms']['p50'])} ms for the matched baseline.
The software paint proxy waits for 30 ms of paint quiet, records the last paint
timestamp, and excludes the first workspace cycle from each invocation. It does
not measure DWM presentation or GPU-only time. The historical approximately 140 ms
number is not substituted for this matched baseline. The sub-100 ms stretch target
was not established. Hiding the GL child while rearranging was tested in
`workspaces-experiment`, provided no meaningful improvement and increased command
overhead; it was reverted. No fragile layout cache or renderer rewrite was added.
All five workspaces, Saved Layouts/Reset, active identity, pixels, layer/frame data,
playback entries, model/UV source, unknown JSON fields, unsaved state and histories
are covered by the accepted regression suite.

### D — Memory and lifecycle

`final-memory` stages 50k index/catalog ownership, destruction, diagnostic heap
compaction, GL initialization and repeated model replacement. `final-lifecycle`
repeats native window document/model/large-layered open/close cycles. `final-storage`
adds 32 native animation load/edit/save-or-discard/close cycles per invocation for
64×4×256² unique and repeated frame workloads. Fixture-generation buffers remain
live by design in the storage probe: its absolute memory must not be compared with
an empty production window. First-use, peak and post-close samples are distinct.
Heap snapshots distinguish busy from free enumerated entry payloads; they are
not a complete allocation-stack or GPU-memory profiler.

The standalone render benchmark and production window have different scopes and
initialization/retention histories. The staged probe quantifies the catalog/allocator
and GL contributions rather than calling the unmatched historical footprint a leak.
Heap compaction is diagnostic only and was not added to production.

| Final memory-probe stage | Median working set MiB | Median private MiB | Median enumerated busy payload MiB | Median enumerated free payload MiB |
|---|---:|---:|---:|---:|
'''
for label in ('start','50k-index','50k-catalog','50k-destroyed','heap-compacted','gl-preview','25-model-cycles','generic-close','generic-close-compacted'):
    rows=[r for run in after['memory_attribution'] for r in run if r['label']==label]
    if rows:
        text += '| '+label+' | '+' | '.join(f'{median([r[k] for r in rows]):.2f}' for k in ('working_set_mib','private_mib','heap_busy_mib','heap_free_committed_mib'))+' |\n'
text += '''
GPU buffers/programs/textures and document counts are retained per cycle in the
raw observations. Delayed allocator reuse or retained process commitment alone
is not classified as an unreachable allocation. Dedicated VRAM and driver-private
allocation stacks remain unattributed; the historical memory number is not claimed
fully equivalent to the current controlled probe.
The raw legacy key `heap_free_committed_mib` sums HeapWalk free-entry payloads,
excluding region headers and uncommitted-range entries. It is not a validated
private-commit counter: after catalog destruction, its 15.81 MiB median even exceeds
the separately sampled 7.02 MiB process private value. These diagnostic totals
are therefore not added to private bytes or presented as a complete memory balance.
Counters are sampled before heap enumeration, and the probe does not reconcile
every Windows heap/driver mapping. HeapWalk also locks heaps and can perturb
other threads, so these snapshots are confined to attribution/lifecycle probes.
See Microsoft's [heap-entry definitions](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-process_heap_entry)
and [HeapWalk limitations](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapwalk).
The [resource ownership audit](RESOURCE_LIFECYCLE.md) records the inspected cache
budgets, timer conditions, cancellation paths and Qt/GL destruction ordering.

The supplementary bootstrap experiment uses the same final archive set and varies
shared GL contexts, Fusion style, and whether the empty-preview QPainter path runs
first (three runs per condition). Sharing is enabled before QApplication construction, so its `start`
sample includes any global context initialization moved into that stage. This is
an initialization-scope experiment, not a production memory-saving change.

| Bootstrap | Start private MiB | GL-preview private MiB | Generic-close private MiB |
|---|---:|---:|---:|
'''
for variant in ('original','shared','fusion','shared-fusion','empty-first'):
    runs=[read(out / 'memory-bootstrap' / f'{variant}-{n}.json') for n in range(3)]
    values=[]
    for stage in ('start','gl-preview','generic-close'):
        values.append(median([r['private_mib'] for run in runs for r in run if r['label']==stage]))
    text += '| '+variant+' | '+' | '.join(f'{v:.2f}' for v in values)+' |\n'
text += f'''
See [bootstrap identities/method](memory-bootstrap/identity.json) and its raw stage
samples. These controls separate initialization choices from document lifetimes;
they do not assign every byte of Qt/driver commitment to a particular allocation stack.
Running the empty-preview QPainter path first moves the later generic-preview
allocation step earlier: private commitment stays approximately flat when the
model is subsequently cleared. Fusion alone has little effect in this experiment;
shared-context runs have a larger first-GL footprint. Together with stable repeated
model-cycle samples, this supports first-use Qt/driver initialization as a contributor
to the discrepancy. It does not identify every allocation. Absolute driver commitment
also differs between the main and supplementary matrices, so their absolute values
are not treated as a production optimization comparison.

The synchronous animation storage cycles retain 177 process handles in every
late final sample. Across cycles 16–31, final private bytes range from
165.96–169.81 MiB for unique frames and 70.18–76.21 MiB for repeated frames across
the two invocations. Native-window post-close samples return to zero documents
and the same bounded GL object counts. Their final six cycles range roughly
398–406 MiB private, comparable with the baseline's 397–409 MiB. The 300 ms
post-close event pump can overlap a bounded safety snapshot, so it is not a proof
that every asynchronous allocation has disappeared. No production memory reduction
or exhaustive leak-free claim is inferred from these observations.

### E — OpenGL

The existing renderer's dirty texture upload, geometry/material separation,
bounded CPU images, resource destruction and paused/hidden scheduling were audited.
CPU submission, synchronous framebuffer readback and optional GPU command-interval
queries are recorded separately in `final-workloads`/`final-gpu`. Query result
collection checks availability; timer queries exist only in the benchmark. No
interactive path gained a GPU readback or blocking query. No measured evidence
justified changing shaders, draw ordering, FBO ownership or rendering architecture.
The existing rectangular-animation mipmap limitation remains documented in
[Phase 2-C4 limitations](../phase-2c4/LIMITATIONS.md). No new Minecraft runtime
comparison is represented as complete.

### F — Large resource packs

The representative fixture contains 50,000 textures / 50,002 indexed files. Initial
scan, file/subtree refresh, model installation, category/folder/thumbnail navigation,
search and file open/close are measured. The major gains are subtree refresh and
content-only view installation (see PERFORMANCE.md). Search/filter setter timings
have their actual CPU scope; a fast setter is not presented as full visual completion.
Native notification propagation, canceled/superseded generations, cache budgets,
category changes and persistent indexes pass the indexing suite. Thumbnail workers
remain bounded to two, pending work to 128, and the icon cache to 4 MiB.

### G — Raster/layers

Retained editor probes cover strokes at 64/512/2048 pixels, large brushes,
selections/shapes, transforms, adjustments/filters/generation, 16/64-layer compositing,
history and native/batch storage. Pixel-equivalence regression compares actual RGBA
buffers with the original integer compositor across random alpha, visibility,
opacity, partial dirty regions, undo/redo and retained QImage snapshots. Legacy
single-layer hidden RGB and encoded-sRGB source-over semantics remain intact.

### H — Animation

Retained animation probes cover 16–1024 source frames, up to 16,384 playback entries,
unique/repeated and layered data, scrubbing/duplication/playback, onion
skin, native save/load, recovery and Minecraft export. Frame reordering is covered
by correctness tests rather than an independently isolated timing. New storage cycles retain
the native/undo guarantees. Frame-composite caches remain bounded by their existing
admission limits; playback references remain separate from source-frame ownership.
`NVLD0001` and `NVAN0002` encoding/decoding contracts were not changed.

The original three-pair matrix showed a higher median for the 1,024-source,
16,384-entry timeline in two final invocations. A separate six-pair animation-only
comparison was therefore run with alternating order; the original observations
remain in the main matrix. The follow-up is reported separately in PERFORMANCE.md,
rather than pooling unlike execution scopes or replacing the earlier result.
No timeline production code was changed to obtain that follow-up.

An additional three-pair probe records five seconds of `displayed` signal
timestamps on that workload, with 100 ms default entry durations. These are CPU
scheduling observations before canvas/preview update submission, not physical
frame presentation. Baseline intervals: n={before['playback_display_interval_ms']['n']},
P50 {ms(before['playback_display_interval_ms']['p50'])} ms,
P95 {ms(before['playback_display_interval_ms']['p95'])} ms;
final: n={after['playback_display_interval_ms']['n']},
P50 {ms(after['playback_display_interval_ms']['p50'])} ms,
P95 {ms(after['playback_display_interval_ms']['p95'])} ms.
This dedicated probe uses the same final/frozen baseline archives and records
its new instrumentation executable hashes in `playback-cadence/identities.json`.

### I — Models and UV

The retained C7/C8/model probes measure loading/resolution, cached lookup, geometry,
selection, bulk/numeric UV and display transformations, history and guarded storage.
An added production-widget test makes unsaved element/UV edits, switches through
all workspaces, checks authoritative sources/unknown fields and disk revision, then
verifies undo/redo. No model semantics, fallback resolution or JSON preservation
policy changed.

### J — Batch/validation

Existing small-image and native layer/animation batches, 50k initial/cached validation,
model-scoped processing, backup/repair and export probes were retained. The extended
editor probe adds 24×1024² three-step batches at one/four workers and cancellation
after the first completion, recording request-to-return time and checking committed
outputs decode correctly. Accepted integrity/batch/native/animation-export suites
exercise failures, interruption, recovery and guarded writes. There is no throughput
gain obtained by removing validation, backups, cancellation checks or transactions.

## Shutdown measurements

The retained standalone shutdown probe was relinked against both verified archive
sets. Three pairs per state cover settled close, active indexing, queued thumbnails,
recovery writing, live operation preview, batch cancellation and floating docks.
It records close-event return, Window destruction, QApplication destruction and
batch cancel/join separately. These are instrumented CPU wall intervals; stdout
logging and the probe's 5 ms automatic answer to its own deliberate unsaved-edit
prompt are included. No owner documents or settings are used. All 42 runs exited
successfully. The smoke deadline/process lifetime is not used as a substitute for
these shutdown intervals.

| State | Build | Close event median ms | Window destructor median ms | QApplication destructor median ms | Close request → application destroyed median ms |
|---|---|---:|---:|---:|---:|
'''
for mode in ('settled','index','thumbnails','recovery','preview','batch','floating'):
    for label, build in (('baseline','studio-phase3c-baseline-release'),('final','studio-phase3c-release')):
        rows=[r['intervals'] for r in shutdown if r['mode']==mode and r['build']==build]
        text += '| '+mode+' | '+label+' | '+' | '.join(f'{median([r[k] for r in rows]):.3f}' for k in
            ('close_event_ms','window_destructor_ms','application_destructor_ms','close_to_application_destroyed_ms'))+' |\n'
text += '''
Raw events, worker states and cancellation-join intervals are in
[final-shutdown/rows.json](final-shutdown/rows.json). Qt/driver CPU teardown intervals
do not establish when every GPU command or Windows loader finalizer completes.

## Native verification and acceptance

Both final configurations pass all 33 registered suites with '''
text += f"{tests['release']['totals_including_init_cleanup']['passed']} Release and {tests['debug']['totals_including_init_cleanup']['passed']} Debug checks"
text += ''' (including init/cleanup), zero failures and zero skips. See
[Release results](tests-release-accepted/summary.json),
[Debug results](tests-debug-accepted/summary.json) and the corresponding text/XML logs.
The accepted baseline Debug run passed all 33 suites; baseline Release passed 32/33,
with the first-palette directory defect causing the retained editor-GUI timeout.

Both native gallery runs pass. Representative baseline/final dark/light, compact,
texture, layered, animation, model/UV and dialog screenshots are retained under
`gallery-*-accepted`. Representative captures were visually inspected; no design tokens, spacing,
menu structure, shortcuts, accessibility labels or theme values were changed.
The real production executable was also controlled through the Computer Use skill:
maximize, Essentials→UV, an OS pointer stroke, Ctrl+Z restoring Saved state and
the model preview, then Alt+F4. All 18 source fixture files remained byte-identical.
See [native acceptance](native-acceptance.json). The launcher did not retain that
manual session's exit handle, so no numeric exit code is invented for it. All timed
probe/process invocations record their actual exit results.

## Limits and remaining performance opportunities

No true cold-cache boot, physical input-to-photon/DWM trace, second GPU, dedicated
VRAM attribution, complete allocation-stack trace, five-minute per-state idle soak,
or exhaustive Minecraft runtime comparison was performed. Available counters and
software proxies are named explicitly. Process lifetime includes the intentional
seven-second smoke deadline; it is not relabeled as pure shutdown/destructor latency.
Separate shutdown probes isolate the application/window CPU intervals described above;
they do not isolate Windows loader finalization or GPU completion. Fine-grained thread wakeups are represented by the supported
dispatcher/context-switch observations, not a fabricated ETW result.

Workspace visual completion and the standalone driver/VRAM share of memory remain
useful future profiling targets; they are not claimed solved. Optimizations were
limited to confirmed bottlenecks with controlled benefits and passing correctness
tests. No Phase 3-D, 3-E or Phase 4 work was begun.

## Reproduction

Use the retained frozen baseline, or build the accepted source separately before
applying the production changes. Do not point a baseline rebuild at optimized source.
On this installation `.tools/python/Scripts/python.exe` supplies psutil/Pillow.

```
python scripts/build-phase3c.py studio-phase3c-release
python scripts/build-phase3c.py studio-phase3c-debug --configuration Debug
.tools/python/Scripts/python.exe scripts/run-phase3c.py tests --label NEW-UNIQUE-LABEL
.tools/python/Scripts/python.exe scripts/run-phase3c.py benchmarks --output-root docs/phase-3c-repeat
.tools/python/Scripts/python.exe scripts/profile-phase3c.py docs/phase-3c-repeat/animation-followup --builds studio-phase3c-baseline-release studio-phase3c-release --runs 6 --mode animation
.tools/python/Scripts/python.exe scripts/profile-phase3c.py docs/phase-3c-repeat/playback-cadence --builds studio-phase3c-baseline-release studio-phase3c-release --runs 3 --mode playback
python scripts/build-phase3c.py studio-phase3c-baseline-release --shutdown-probe
python scripts/build-phase3c.py studio-phase3c-release --shutdown-probe
.tools/python/Scripts/python.exe scripts/profile-phase3c-shutdown.py --output docs/phase-3c/NEW-shutdown
.tools/python/Scripts/python.exe scripts/profile-phase3c-memory-bootstrap.py --output docs/phase-3c/NEW-bootstrap
python scripts/summarize-phase3c.py docs/phase-3c-repeat
python scripts/audit-phase3c.py --verify
```

Output directories deliberately refuse overwrite. Use new output paths for a repeat
of the benchmark commands listed in `benchmarks-steps.json`; preserve existing
developer-local data and fixtures. Rebuilding the optional common probe alone uses
`build-phase3c.py --probe-only TARGET` and verifies frozen archive hashes.
The animation follow-up and cadence use the same native probe. The original matrix
used its earlier instrumentation executable; the cadence addition records a newer
probe hash and leaves production archives untouched.

Technical references used during the audit: Qt's
[QImage/implicit sharing](https://doc.qt.io/qt-6/qimage.html),
[QOpenGLWidget context/lifecycle guidance](https://doc.qt.io/qt-6/qopenglwidget.html),
[QMainWindow docking](https://doc.qt.io/qt-6/qmainwindow.html),
[GPU timer-query semantics](https://doc.qt.io/qt-6/qopengltimerquery.html), and Microsoft's
[process memory definitions](https://learn.microsoft.com/en-us/windows/win32/psapi/process-memory-usage-information).
'''
(out / 'PHASE_3C_REPORT.md').write_text(text, encoding='utf-8')
print(out / 'PHASE_3C_REPORT.md')
