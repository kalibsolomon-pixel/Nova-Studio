"""Repeated and unique frames run in separate fresh processes, with sampled private memory."""
import argparse,hashlib,json,os,pathlib,subprocess,time
import psutil
root=pathlib.Path(__file__).resolve().parents[1];p=argparse.ArgumentParser();p.add_argument('label');p.add_argument('--runs',type=int,default=3);a=p.parse_args()
out=root/'docs/phase-2c6'/('storage-'+a.label);out.mkdir(exist_ok=False)
exe=root/'build/studio-c6-release/tests'/('nova-c6-storage-before.exe' if a.label.startswith('before') else 'nova-c6-storage-bench.exe')
env={**os.environ,'PATH':str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']};rows=[]
for kind in ('repeated','unique'):
 for run in range(a.runs):
  target=out/f'{kind}-{run}.json';child=subprocess.Popen([str(exe),str(target),kind,'256'],env=env,cwd=root,creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.PIPE,stderr=subprocess.PIPE);proc=psutil.Process(child.pid);peak=0;samples=0
  while child.poll() is None:
   try:peak=max(peak,proc.memory_info().private);samples+=1
   except psutil.NoSuchProcess:break
   time.sleep(.01)
  stdout,stderr=child.communicate();assert child.returncode==0,(child.returncode,stdout,stderr)
  row=json.loads(target.read_text());row.update(run=run,sampled_peak_private_bytes=peak,memory_samples=samples);rows.append(row)
  (out/'summary.json').write_text(json.dumps({'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'samples':rows},indent=2)+'\n')
  print(kind,run,row['guarded_save_ms'],peak,flush=True)
