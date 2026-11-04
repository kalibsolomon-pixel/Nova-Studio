"""Measured Windows Release startup/core and five-minute memory/CPU ablations.
Uses separate preferences and never edits source fixtures. Shared desktop, warm caches.
"""
from pathlib import Path
import hashlib,json,math,os,statistics,subprocess,time
import psutil
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'docs/phase-2b/performance'/str(time.time_ns()); OUT.mkdir(parents=True)
ENV={**os.environ,'PATH':str(ROOT/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
EXES={'phase2a':ROOT/'build/phase2a-baseline/build/studio-release/src/nova-studio.exe',
      'phase2b':ROOT/'build/studio-release/src/nova-studio.exe',
      'prototype':ROOT/'build/qt-review/nova-qt.exe'}
def write(name,value): (OUT/name).write_text(json.dumps(value,indent=2))
def stats(values):
    v=sorted(values)
    return {'n':len(v),'p50':statistics.median(v),'p95':v[math.ceil(len(v)*.95)-1],'p99':v[math.ceil(len(v)*.99)-1]}
summary={'binaries':{n:{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for n,p in EXES.items()},'core':{},'startup':{}}
write('summary.json',summary)
for size in (1500,10000,50000):
    for name in ('phase2a','phase2b'):
        samples={}
        for iteration in range(3):
            path=OUT/f'{name}-core-{size}-{iteration}.json'
            subprocess.run([str(EXES[name]),'--bench',str(ROOT/f'fixtures/generated/pack-{size}'),'--output',str(path)],env=ENV,check=True,timeout=60)
            data=json.loads(path.read_text())
            for key,values in data['samples_ms'].items(): samples.setdefault(key,[]).extend(values)
        summary['core'][f'{name}-{size}']={key:stats(values) for key,values in samples.items()}
        write('summary.json',summary)
print('Core measurements complete',flush=True)
starts={n:[] for n in ('phase2a','phase2b')}
for iteration in range(6):
    for name in (list(starts) if iteration%2==0 else list(reversed(starts))):
        trace=OUT/f'{name}-startup-{iteration}.json'; before=time.perf_counter()
        proc=subprocess.Popen([str(EXES[name]),'--project',str(ROOT/'fixtures/generated/pack-1500'),'--fresh-layout','--telemetry',str(trace),'--smoke-seconds','2'],env=ENV)
        ready=False
        while time.perf_counter()-before<20:
            if trace.exists():
                try: ready=bool(json.loads(trace.read_text())['samples_ms'].get('ready_capture_ms'))
                except (ValueError,KeyError): pass
            if ready: break
            time.sleep(.01)
        if not ready: raise RuntimeError(f'{name} startup failed')
        starts[name].append((time.perf_counter()-before)*1000)
        if proc.wait(timeout=10)!=0: raise RuntimeError(name)
summary['startup']={n:stats(v) for n,v in starts.items()};write('startup-raw.json',starts);write('summary.json',summary)
print('Startup measurements complete; beginning 320-second soak',flush=True)
variants={'phase2a-small':('phase2a','pack-1500',[]),'prototype-small':('prototype','pack-1500',[]),
          'phase2b-empty':('phase2b',None,[]),'phase2b-small':('phase2b','pack-1500',[]),
          'phase2b-large':('phase2b','pack-50000',[]),'phase2b-no-preview':('phase2b','pack-1500',['--no-preview']),
          'phase2b-no-thumbnails':('phase2b','pack-1500',['--no-thumbnails'])}
variants['phase2b-floating-preview']=('phase2b','pack-1500',['--float-preview'])
processes={}; observations={}
for label,(name,pack,extra) in variants.items():
    args=[str(EXES[name]),'--fresh-layout','--smoke-seconds','335','--telemetry',str(OUT/f'{label}-idle.json')]
    if name!='prototype': args+=['--settings',str(ROOT/f'build/phase2b-profile-{OUT.name}/{label}.ini')]
    if pack: args+=['--project',str(ROOT/'fixtures/generated'/pack)]
    proc=subprocess.Popen(args+extra,env=ENV);processes[label]=(proc,psutil.Process(proc.pid));observations[label]=[]
soak=time.perf_counter()
for sample in range(64):
    time.sleep(5)
    for label,(_,p) in processes.items():
        mem=p.memory_info();cpu=p.cpu_times()
        observations[label].append({'seconds':time.perf_counter()-soak,'working_set_mib':mem.rss/2**20,'private_mib':mem.private/2**20,'cpu_seconds':cpu.user+cpu.system})
    write('idle-observations.json',observations)
summary['idle']={}
for label,obs in observations.items():
    start,end=obs[-5],obs[-1]
    summary['idle'][label]={'initial_working_set_mib':obs[0]['working_set_mib'],'five_minute_working_set_mib':statistics.median(o['working_set_mib'] for o in obs[-4:]),
        'five_minute_private_mib':statistics.median(o['private_mib'] for o in obs[-4:]),'cpu_percent_one_logical_processor':100*(end['cpu_seconds']-start['cpu_seconds'])/(end['seconds']-start['seconds'])}
write('summary.json',summary)
for proc,_ in processes.values():
    if proc.wait(timeout=30)!=0: raise RuntimeError('Idle process failed')
print(str(OUT),flush=True)
