"""Matched preserved C6/current C7 process observations; no synthetic native input."""
import argparse, hashlib, json, os, pathlib, statistics, subprocess, time
import psutil
root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();out=root/a.output;out.mkdir(parents=True,exist_ok=False)
env={**os.environ,'PATH':str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
flags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0
rows=[]
for build in ('studio-c6-release','studio-c7-release'):
 exe=root/'build'/build/'src/nova-studio.exe'
 for fixture in ('fixtures/phase-2c5-models','fixtures/generated/pack-50000'):
  label=pathlib.Path(fixture).name
  for n in range(3):
   name=f'{build}-{label}-{n}';telemetry=out/(name+'.json')
   args=[str(exe),'--project',str(root/fixture),'--settings',str(out/(name+'.ini')),'--fresh-layout','--smoke-seconds','7','--telemetry',str(telemetry)]
   with (out/(name+'.log')).open('w') as log:
    begin=time.perf_counter();child=subprocess.Popen(args,cwd=root,env=env,creationflags=flags,stdout=log,stderr=log);proc=psutil.Process(child.pid)
    time.sleep(2);before=proc.cpu_times();start=time.perf_counter();time.sleep(3);after=proc.cpu_times();memory=proc.memory_info();elapsed=time.perf_counter()-start
    child.wait(timeout=20)
   assert child.returncode==0, name
   rows.append(dict(build=build,fixture=fixture,run=n,sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
     idle_sample_seconds=elapsed,idle_cpu_one_core_percent=100*(after.user+after.system-before.user-before.system)/elapsed,
     working_set_bytes=memory.rss,private_bytes=getattr(memory,'private',None),process_total_ms=1000*(time.perf_counter()-begin),telemetry=json.loads(telemetry.read_text())))
   (out/'processes.json').write_text(json.dumps(rows,indent=2)+'\n')
   print(name,round(rows[-1]['idle_cpu_one_core_percent'],3),flush=True)
for n in range(3):
 # This is a throughput observation, not a latency pass/fail gate. Retain slow
 # filesystem observations; the separate event-loop acceptance gates stay strict.
 result=subprocess.run([str(root/'build/studio-c7-release/tests/nova-c7-bench.exe'),str(out/f'authoring-{n}.json')],cwd=root,env=env,creationflags=flags,capture_output=True,timeout=180)
 (out/f'authoring-{n}.log').write_bytes(result.stdout+result.stderr);assert result.returncode==0,result.returncode
for build in ('studio-c6-release','studio-c7-release'):
 result=subprocess.run([str(root/'build'/build/'tests/nova-model-bench.exe'),str(root/'fixtures/phase-2c5-models'),str(out/(build+'-rendering.json'))],cwd=root,env=env,creationflags=flags,capture_output=True,timeout=90)
 (out/(build+'-rendering.log')).write_bytes(result.stdout+result.stderr);assert result.returncode==0,result.returncode
(out/'summary.json').write_text(json.dumps(dict(head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),rows=rows,
 notes='Three warm samples per build/pack. Three-second idle windows, one logical processor=100%. Not a long soak or whole-machine CPU figure. Timings include filesystem and GL synchronization.'),indent=2)+'\n')
