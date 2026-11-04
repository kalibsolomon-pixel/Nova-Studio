"""Generate the report tables only from recorded samples (nearest-rank quantiles)."""
import json
import math
import statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RAW=ROOT/'docs/benchmarks/raw'
def quantile(values,p):
    values=sorted(values)
    return values[max(0,math.ceil(p*len(values))-1)]
def row(name,values):
    return f'| {name} | {len(values)} | {quantile(values,.5):.4f} | {quantile(values,.95):.4f} | {quantile(values,.99):.4f} |'
def table_header():return '| Metric | n | p50 ms | p95 ms | p99 ms |\n|---|---:|---:|---:|---:|'
lines=['# Phase 1 measured benchmarks','',
'Date: 2026-10-27 (America/New_York). Windows 11 Home 10.0.26300 x64; Intel i7-14700F (20 cores/28 logical processors); 34,104,127,488 bytes RAM; AMD Radeon RX 9070 XT, driver 32.0.32015.7029 / 26.10.1. Display: 2560x1440, 240 Hz, native captures at 125% scaling. Desktop applications remained open; these are local workstation measurements, not an isolated lab.','',
'Rust: rustc/cargo 1.98.1, release opt-level=3, debug=1; eframe 0.36.0, egui 0.36.2, wgpu 30.0.1, egui_dock 0.21.1. Final Windows renderer: wgpu/DX12. Qt: 6.12.0 shared MSVC x64, C++20, Release /O2 /DNDEBUG; QPainter canvas and OpenGL 4.6 cube, same AMD GPU. CMake/Ninja versions are in ENVIRONMENT.md.','',
'## Method and comparability','',
'Both use the identical original generated PNG packs (seed 20261027; 1,500/10,000/50,000 assets, 16–256px RGBA). Generator manifests contain hashes. Three alternating-order process invocations per framework and pack, with five index iterations, 200 decode+nearest-thumbnail operations, 1,000 one-pixel brush/undo/redo commands and 30 atomic 256x256 saves per invocation. All are OS-warm cache; caches were not flushed. Raw samples are under `docs/benchmarks/raw/`. Quantiles use nearest rank; small sample p99 values are maxima and should not be overinterpreted.','',
'The microbenchmarks time CPU operations, not end-to-end input latency. Qt uses its PNG codec/QSaveFile, Rust image/tempfile; encoder/compression and sync semantics differ, so save times describe the prototypes rather than identical codecs. Thumbnail throughput includes decode, filesystem reads and nearest resize to 64x64, excludes GPU upload, and is single-worker headless throughput.','',
'The binary identities recorded for the five-minute soak are in benchmark-binaries.json; final delivery identities are in docs/phase-1/delivery-binaries.json. UI/layout/context-lifetime fixes were subsequently validated, and short process/core workloads rerun. The final invalid-project persistence guard affects failure handling only. The earlier shutdown timeout remains recorded even though the final 20-launch rerun completed.', '', '## Core operations','']
summary={}
for count in (1500,10000,50000):
    lines.extend([f'### {count:,} assets','',table_header()])
    throughputs=[]
    for name in ('rust','qt'):
        samples={}
        for path in sorted(RAW.glob(f'{name}-{count}-*.json')):
            data=json.loads(path.read_text())
            assert data['counters']['assets']==count
            for metric,values in data['samples_ms'].items():samples.setdefault(metric,[]).extend(values)
        assert samples
        summary[f'{name}-{count}']={}
        for metric,values in samples.items():
            lines.append(row(name+' '+metric,values))
            summary[f'{name}-{count}'][metric]={'n':len(values),'p50_ms':quantile(values,.5),'p95_ms':quantile(values,.95),'p99_ms':quantile(values,.99)}
        thumbs=samples['thumbnail_decode_resize'];throughput=len(thumbs)/(sum(thumbs)/1000)
        throughputs.append(f'{name} measured decode+thumbnail throughput: **{throughput:,.0f} assets/s**.')
    lines.extend(['',*throughputs])
    lines.append('')
process=json.loads((RAW/'process.json').read_text())
lines.extend(['## Native process startup and idle','',
'Twenty alternating-order launches per framework. External startup is process creation to a ready marker after indexing a small pack, opening its initial texture and GPU readback. Rust uses the first egui framebuffer screenshot callback; Qt reads its cube framebuffer after frameSwapped. These are conservative, framework-specific capture-ready proxies, not proof of an interactive present or equivalent display pipeline latency. The external timer includes process loading and file-polling overhead. Internal first-UI timers are retained separately but are not directly comparable.','',table_header()])
for name,values in process['startup'].items():
    samples=[v['external_ready_ms'] for v in values];lines.append(row(name+' OS-warm capture-ready startup',samples))
    summary[name+'-startup']={'p50_ms':quantile(samples,.5),'p95_ms':quantile(samples,.95),'p99_ms':quantile(samples,.99),'first_run_ms':samples[0]}
lines.extend(['','| Framework | Initial working set p50 MiB | Idle working set p50 MiB | Idle CPU % of one logical processor |','|---|---:|---:|---:|'])
for name,data in process['idle'].items():
    initial=statistics.median(v['initial_working_set_bytes'] for v in process['startup'][name])/2**20
    idle=statistics.median(data['working_set_samples_bytes'])/2**20
    lines.append(f'| {name} | {initial:.2f} | {idle:.2f} | {data["cpu_one_core_percent"]:.3f} |')
    summary[name+'-idle']={'initial_mib':initial,'idle_mib':idle,'cpu_one_core_percent':data['cpu_one_core_percent']}
lines.extend(['','Idle: 10 seconds stabilization, then 20 one-second working-set samples and a process user+kernel CPU delta over that interval. 100% means one logical processor; divide by 28 for whole-machine utilization. No telemetry repaint loop runs during idle. This is a short idle observation, not the proposed five-minute soak. Working set includes shared mapped resources; private/GPU memory were not measured. Both prototypes missed the 150 MB working-set target. A verified cold-cache startup remains unavailable.','',
'## Native editing exercise','',
'One 12-second, 10,000-asset run each: scripted brush commands, periodic undo/redo, changing zoom, grid browser, preview, and repeated window resize requests. Nominal 60/120/180 Hz schedule phases were requested. Actual event intervals are reported below and may miss the request. These use application-driven events, not OS mouse input. Rust UI construction and Qt canvas paint are different scopes; preview CPU submission also differs. Do not compare these as total frame/GPU/present time.'])
for name in ('rust','qt'):
    data=json.loads((RAW/f'{name}-exercise.json').read_text())
    lines.extend(['',f'### {name}', '',table_header()])
    for metric,values in data['samples_ms'].items():
        if metric not in ('first_ui_ms','ready_capture_ms','index'):lines.append(row(name+' '+metric,values))
    lines.extend(['',f'{name} document texture upload counters (requested pixel payload; excludes fonts, UI and thumbnails; not bus traffic): `{json.dumps(data["counters"],sort_keys=True)}`.',''])
lines.extend(['## Interpretation and unavailable metrics','',
'The original index registration walked every ancestor and searched duplicate sibling lists. The 50k baseline was ~1,580 ms Rust / 250 ms Qt (median); both implementations now register ancestors only when discovering a new directory. The core tables above are the rerun, not those baselines.','',
'Initial Vulkan idle diagnostics showed high CPU despite few UI repaints. An early smoke deadline used delayed repaint; it was replaced by a sleeping timer. DX12 subsequently showed lower idle CPU in local checks; Windows now defaults to DX12. Vulkan remains selectable. Initial process/backend files and diagnostics are retained with their names, while `process.json` is the final default run. This does not establish a general Vulkan-versus-OpenGL result.','',
'No true cold-cache run, hardware input-to-photon, event-to-present trace, GPU timestamp duration, present cadence, dropped-frame count, physical 60/120/180 Hz run, five-minute memory soak, Linux or second GPU was available. WPR CPU capture was attempted but denied by system profiling policy (0xc5585011); no policy change was attempted. Resize requests were exercised within the timed UI workload, but resize-specific presentation latency was not isolated. Preview CPU work is instrumented. The provisional latency and refresh targets therefore remain unproven.','',
'Warm-start target checks are based on the stated capture-ready proxy; raw timing variance matters. Memory targets are missed. Microsecond one-pixel commands do not imply microsecond brush presentation. Framework recommendations remain provisional.'])
steady=RAW/'steady-idle.json'
if steady.exists():
    data=json.loads(steady.read_text());lines.extend(['','## Five-minute idle soak','',data['method'],'','| Variant | Last-20-second mean CPU % of one processor | Last-20-second median working set MiB | Recorded UI-build / canvas-paint samples during soak |','|---|---:|---:|---:|'])
    summary['steady_idle']={}
    for name,entry in data['samples'].items():
        tail=[r for r in entry['rows'] if r['elapsed_seconds']>=300]
        cpu=statistics.mean(r['cpu_one_core_percent'] for r in tail);memory=statistics.median(r['working_set_bytes'] for r in tail)/2**20
        frames=len(entry['telemetry']['samples_ms'].get('ui_build_cpu',entry['telemetry']['samples_ms'].get('canvas_paint_cpu',[])))
        lines.append(f'| {name} | {cpu:.3f} | {memory:.2f} | {frames} |')
        summary['steady_idle'][name]={'cpu_one_core_percent':cpu,'working_set_mib':memory,'ui_samples':frames}
    lines.extend(['','The driver-side CPU activity did **not** settle after five minutes in either wgpu backend on this machine. A thread-start-address diagnostic located the busy native thread inside AMD `amdxc64.dll`; it does not prove which driver or wgpu call causes the loop. UI build counts stayed very low. DirectX 12 did not solve this in the longer test, so the earlier short diagnostic improvement is not considered a fix. Qt/OpenGL remained substantially lower. This is a blocking risk for approving egui/wgpu on this hardware, not a general claim that Vulkan or DX12 is inefficient.',''])
    lines=[line.replace('five-minute memory soak, Linux or second GPU was available','Linux or second GPU was available').replace('no five-minute steady-state claim','no five-minute steady-state claim') for line in lines]
(ROOT/'docs/phase-1/BENCHMARKS.md').write_text('\n'.join(lines).rstrip()+'\n',encoding='utf-8')
(RAW/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps({k:v for k,v in summary.items() if '-idle' in k or '-startup' in k},indent=2))
