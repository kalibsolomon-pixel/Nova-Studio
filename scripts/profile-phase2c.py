"""Release-only Windows process measurements; isolated preferences, read-only fixtures."""
from pathlib import Path
import hashlib, json, math, os, statistics, subprocess, sys, time
import psutil
ROOT = Path(__file__).resolve().parents[1]
label = sys.argv[1]
OUT = ROOT / 'docs/phase-2c/performance' / (label + '-' + str(time.time_ns()))
OUT.mkdir(parents=True)
EXE = ROOT / (sys.argv[2] if len(sys.argv) > 2 else 'build/studio-release/src/nova-studio.exe')
CONTROL = ROOT / sys.argv[3] if len(sys.argv) > 3 else None
ENV = {**os.environ, 'PATH': str(ROOT / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}
def write(name, value): (OUT / name).write_text(json.dumps(value, indent=2))
def stats(v):
    v = sorted(v)
    return {'n': len(v), 'p50': statistics.median(v), 'p95': v[math.ceil(.95 * len(v))-1]}
summary = {'label': label, 'exe': str(EXE), 'sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(), 'startup': {}, 'core': {}}
write('summary.json', summary)
starts = []
control_starts = []
if CONTROL:
    summary['control'] = {'exe':str(CONTROL),'sha256':hashlib.sha256(CONTROL.read_bytes()).hexdigest()}
for i in range(6):
    variants = [('main',EXE,starts)] + ([('control',CONTROL,control_starts)] if CONTROL else [])
    if i%2: variants.reverse()
    for variant, executable, values in variants:
        trace = OUT / f'{variant}-start-{i}.json'
        args = [str(executable), '--project', str(ROOT/'fixtures/generated/pack-1500'), '--fresh-layout', '--settings', str(OUT/f'{variant}-start-{i}.ini'), '--telemetry', str(trace), '--smoke-seconds', '2']
        before = time.perf_counter(); p = subprocess.Popen(args, env=ENV)
        while time.perf_counter()-before < 20:
            try:
                if json.loads(trace.read_text())['samples_ms'].get('ready_capture_ms'): break
            except (OSError, ValueError, KeyError): pass
            time.sleep(.01)
        else: raise RuntimeError('startup timeout')
        values.append((time.perf_counter()-before)*1000)
        assert p.wait(timeout=15) == 0
summary['startup'] = stats(starts); write('startup-raw.json', starts); write('summary.json', summary)
if CONTROL:
    summary['control']['startup'] = stats(control_starts)
    write('control-startup-raw.json',control_starts)
for size in (1500, 50000):
    path = OUT/f'core-{size}.json'
    subprocess.run([str(EXE), '--bench', str(ROOT/f'fixtures/generated/pack-{size}'), '--output', str(path)], env=ENV, check=True, timeout=90)
    summary['core'][str(size)] = {k: stats(v) for k,v in json.loads(path.read_text())['samples_ms'].items()}
write('summary.json', summary)
subprocess.run([str(EXE), '--cycle-project', str(ROOT/'fixtures/generated/pack-50000'), '--fresh-layout', '--settings', str(OUT/'cycle.ini'), '--telemetry', str(OUT/'cycle.json')] + (['--layer-cycle'] if label in ('integration','final') else []), env=ENV, check=True, timeout=60)
processes = {}; obs = {}
variants = [('small', 'pack-1500', []), ('large', 'pack-50000', []), ('hidden', 'pack-1500', ['--no-preview']), ('empty', None, [])]
if CONTROL: variants += [('control-small','pack-1500',[]),('control-empty',None,[])]
for variant, pack, extra in variants:
    args = [str(CONTROL if variant.startswith('control-') else EXE), '--fresh-layout', '--settings', str(OUT/f'{variant}.ini'), '--telemetry', str(OUT/f'{variant}.json'), '--smoke-seconds', '330'] + extra
    if pack: args += ['--project', str(ROOT/'fixtures/generated'/pack)]
    p = subprocess.Popen(args, env=ENV); processes[variant] = (p, psutil.Process(p.pid)); obs[variant] = []
print('Startup/core/cycle complete; measuring 320 seconds idle: ' + str(OUT), flush=True)
before = time.perf_counter()
for _ in range(64):
    time.sleep(5)
    for name, (_, p) in processes.items():
        m = p.memory_info(); c = p.cpu_times()
        obs[name].append({'seconds': time.perf_counter()-before, 'working_set_mib': m.rss/2**20, 'private_mib': m.private/2**20, 'cpu_seconds': c.user+c.system, 'threads': p.num_threads(), 'handles': p.num_handles()})
    write('idle-raw.json', obs)
summary['idle'] = {}
for name, values in obs.items():
    a,b = values[-5],values[-1]
    summary['idle'][name] = {'initial_working_set_mib': values[0]['working_set_mib'], 'five_minute_working_set_mib': statistics.median(v['working_set_mib'] for v in values[-4:]), 'private_mib': statistics.median(v['private_mib'] for v in values[-4:]), 'cpu_percent_one_processor': 100*(b['cpu_seconds']-a['cpu_seconds'])/(b['seconds']-a['seconds'])}
write('summary.json', summary)
for p,_ in processes.values(): assert p.wait(timeout=30) == 0
print(str(OUT), flush=True)
