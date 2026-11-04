"""Sequential production workspace observations. No native input is synthesized."""
import argparse, gzip, hashlib, json, os, pathlib, subprocess, time
import psutil
root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('build');p.add_argument('output');p.add_argument('--seconds',type=int,default=75);p.add_argument('--runs',type=int,default=2);p.add_argument('--probe',default='nova-c6-activity.exe');p.add_argument('--scenarios',default='empty,texture,small,model,hidden,animation-stopped,animation-playing,timeline-hidden,floating,large');p.add_argument('--fixture',default='fixtures/phase-2c5-models');a=p.parse_args()
out=root/a.output;out.mkdir(parents=True,exist_ok=False)
exe=root/'build'/a.build/'tests'/a.probe
if 'before' in out.name: exe=exe.with_name('nova-c6-idle-before.exe')
env={**os.environ,'PATH':str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
rows=[]
for run in range(a.runs):
 for scenario in a.scenarios.split(','):
  name=f'{scenario}-{run}';pack='' if scenario=='empty' else 'fixtures/generated/pack-50000' if scenario=='large' else 'fixtures/generated/pack-1500' if scenario=='small' else a.fixture
  args=[str(exe),'--scenario',scenario,'--settings',str(out/(name+'.ini')),'--fresh-layout','--seconds',str(a.seconds),'--activity',str(out/(name+'-activity.json')),'--telemetry',str(out/(name+'-telemetry.json'))]
  if pack: args+=['--project',str(root/pack)]
  with (out/(name+'.log')).open('w') as log:
   child=subprocess.Popen(args,cwd=root,env=env,creationflags=subprocess.CREATE_NO_WINDOW,stdout=log,stderr=log)
   proc=psutil.Process(child.pid);time.sleep(12);start=time.perf_counter();before=proc.cpu_times();threads={t.id:t.user_time+t.system_time for t in proc.threads()};samples=[]
   while time.perf_counter()-start<a.seconds-15:
    time.sleep(5);cpu=proc.cpu_times();mem=proc.memory_info();samples.append({'seconds':time.perf_counter()-start,'cpu_ms':1000*(cpu.user+cpu.system-before.user-before.system),'working_set_bytes':mem.rss,'private_bytes':getattr(mem,'private',0),'threads':proc.num_threads(),'thread_cpu_ms':{str(t.id):1000*(t.user_time+t.system_time-threads.get(t.id,0)) for t in proc.threads()}})
   shutdown='normal'
   try: child.wait(timeout=15)
   except subprocess.TimeoutExpired:
    if not (out/(name+'-activity.json')).exists():raise
    shutdown='terminated owned probe after imported-animation save prompt';child.terminate();child.wait(timeout=10)
  rows.append({'scenario':scenario,'run':run,'project':pack,'exit':child.returncode,'shutdown':shutdown,'cpu_percent_one_processor':samples[-1]['cpu_ms']/samples[-1]['seconds']/10,'samples':samples})
  (out/'summary.json').write_text(json.dumps({'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'rows':rows},indent=2)+'\n')
  print(name,rows[-1]['cpu_percent_one_processor'],flush=True)
